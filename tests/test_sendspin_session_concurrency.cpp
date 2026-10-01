// SPDX-License-Identifier: MIT
//
// Sendspin companion bridge — EngineSinkSession concurrency regression (C++20).
//
// P1: the SDK contract runs on_audio_write() on the sync-task thread while
// on_stream_start()/on_stream_end() run on the main loop. EngineSinkSession must
// therefore synchronize its OWN session state (state_, generation allocation),
// not merely rely on EngineSink's per-call mutex. This test drives the two
// threads concurrently against a real engine:
//
//   * a writer thread hammers session.write() (the sync-thread path), while
//   * the main thread performs start_stream()/cancel_stream() control ops.
//
// Built for the ThreadSanitizer lane it exposes the unsynchronized
// state_ read/write; with the fix in place it must run clean. It also asserts
// liveness (bounded, no deadlock) and a consistent final state.
//
// The lane is wired in the Makefile as test-sendspin-sink-tsan; the plain
// test-sendspin-sink lane runs this binary too, so the liveness/consistency
// assertions hold without the sanitizer.

#include "engine_sink_session.h"
#include "sendspin_engine_fixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using libreecho::sendspin::EngineSinkSession;
using libreecho::sendspin::EngineSinkSessionConfig;
using libreecho::sendspin::SessionState;
using libreecho::sendspin::SinkResult;
using namespace libreecho::sendspin::test;

namespace {

EngineSinkSessionConfig make_config(const std::string& path) {
    EngineSinkSessionConfig cfg;
    cfg.socket_path = path;
    cfg.connect_timeout_ms = 1000;
    cfg.control_timeout_ms = 20;
    cfg.max_write_timeout_ms = 20;
    return cfg;
}

std::vector<int16_t> pattern_frames(size_t frames) {
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);
    return pcm;
}

// Writer on the "sync thread" (write) vs control ops on the "main loop".
void test_write_vs_control_race() {
    const std::string path = unique_socket_path("race");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "initial start_stream");

    std::atomic<bool> stop{false};
    std::atomic<size_t> writes{0};
    std::atomic<size_t> bytes{0};
    std::atomic<int64_t> max_write_us{0};

    std::thread writer([&] {
        std::vector<int16_t> pcm = pattern_frames(256);
        const uint8_t* data = reinterpret_cast<const uint8_t*>(pcm.data());
        while (!stop.load(std::memory_order_relaxed)) {
            const auto t0 = std::chrono::steady_clock::now();
            const size_t n = s.write(data, 256 * kBytesPerFrame, 20);
            const auto t1 = std::chrono::steady_clock::now();
            const int64_t d = std::chrono::duration_cast<std::chrono::microseconds>(
                                  t1 - t0)
                                  .count();
            int64_t prev = max_write_us.load(std::memory_order_relaxed);
            while (d > prev &&
                   !max_write_us.compare_exchange_weak(
                       prev, d, std::memory_order_relaxed)) {
            }
            bytes.fetch_add(n, std::memory_order_relaxed);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Control storm on the main loop, concurrently with the writer. Measure the
    // real contention bound: a control op must wait at most one in-flight write,
    // not an unbounded time.
    int64_t max_control_us = 0;
    for (int i = 0; i < 64; ++i) {
        const auto c0 = std::chrono::steady_clock::now();
        const SinkResult c = s.cancel_stream();
        (void)c;
        const SinkResult r = s.start_stream();
        const auto c1 = std::chrono::steady_clock::now();
        max_control_us = std::max<int64_t>(
            max_control_us,
            std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count());
        CHECK(r == SinkResult::Ok, "start_stream under write load");
        // Read the session state from the control thread too (races the writer
        // pre-fix on state_).
        (void)s.streaming();
        (void)s.state();
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // Liveness: we reached here, so no deadlock. The last control op was a
    // successful start_stream, so the session must be armed again.
    CHECK(writes.load() > 0, "writer made progress");
    CHECK(s.streaming(), "session is streaming after the control storm");
    CHECK(s.state() == SessionState::Streaming, "state consistent after storm");

    // Contention bound (measured, not nominal): the sync-thread write and the
    // main-loop control op must both stay far below an unbounded stall. The
    // ceiling is a liveness bound, not a tight latency assertion.
    std::fprintf(stderr,
                 "  [P1] contention: max write=%lldus max cancel+start=%lldus\n",
                 (long long)max_write_us.load(), (long long)max_control_us);
    CHECK(max_write_us.load() < 500000,
          "sync-thread write stayed bounded under control load");
    CHECK(max_control_us < 500000,
          "main-loop control op stayed bounded under write load");

    s.disconnect();
    server.stop();
}

// Two writers racing control ops is not the SDK model (one sync thread), but it
// stresses a shared read of the session state from a non-main thread while the
// main loop mutates it: the state read must be well-defined either way.
void test_streaming_query_under_control() {
    const std::string path = unique_socket_path("raceq");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start");

    std::atomic<bool> stop{false};
    std::atomic<size_t> queries{0};
    std::thread querier([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)s.streaming();
            (void)s.state();
            (void)s.accepted_frames();
            queries.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 64; ++i) {
        (void)s.cancel_stream();
        (void)s.start_stream();
    }

    stop.store(true, std::memory_order_relaxed);
    querier.join();

    CHECK(queries.load() > 0, "querier made progress");
    CHECK(s.streaming(), "session armed at the end");
    s.disconnect();
    server.stop();
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_session_concurrency: main-loop/sync-thread race\n");
    test_write_vs_control_race();
    test_streaming_query_under_control();

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr,
                     "test_sendspin_session_concurrency: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr,
                 "test_sendspin_session_concurrency: all checks passed\n");
    return 0;
}
