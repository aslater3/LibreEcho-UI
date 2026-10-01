// SPDX-License-Identifier: MIT
//
// Sendspin companion bridge — EngineSinkSession tests (C++20) against the real
// compiled C99 engine (Platform airplay audio_sink.c).
//
// Pins the Task 5 bridge-core contracts the SDK PlayerRoleListener binding
// relies on:
//   * start_stream() opens a strictly-increasing generation and accepts audio;
//   * write() returns acknowledged BYTES (whole frames) and 0 when not armed;
//   * cancel_stream() fences; no audio is accepted until a higher generation;
//   * the generation counter persists across a reconnect to the same surviving
//     engine (never restarts at 1);
//   * an engine restart yields a fresh epoch; the session adopts it and keeps its
//     monotonic generation;
//   * a stalled engine bounds write() by the clamped per-op timeout (no hang);
//   * poll() reports the engine's own timing, never a fabricated finish.

#include "engine_sink_session.h"
#include "sendspin_engine_fixture.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using libreecho::sendspin::EngineSinkSession;
using libreecho::sendspin::EngineSinkSessionConfig;
using libreecho::sendspin::EngineSinkTiming;
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

size_t write_frames(EngineSinkSession& s, size_t frames, uint32_t timeout_ms) {
    std::vector<int16_t> pcm = pattern_frames(frames);
    return s.write(reinterpret_cast<uint8_t*>(pcm.data()),
                   frames * kBytesPerFrame, timeout_ms);
}

void test_stream_write_and_progress() {
    std::string path = unique_socket_path("sess");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start_stream ok");
    CHECK(s.state() == SessionState::Streaming, "session streaming");
    CHECK(s.generation() == 1u, "first generation is 1");
    CHECK(s.epoch() == server.epoch(), "session adopted the engine epoch");

    CHECK(write_frames(s, 512, 1000) == 512 * kBytesPerFrame,
          "write accepted exactly (acknowledged bytes)");
    CHECK(s.accepted_frames() == 512u, "session accepted cursor exact");

    struct le_audio_sink_progress progress;
    CHECK(le_audio_sink_get_progress(server.sink(), &progress) ==
              LE_AUDIO_SINK_OK,
          "engine progress");
    CHECK(progress.active && progress.accepted_frames == 512u &&
              progress.queued_frames == 512u,
          "the real engine credited exactly the session's frames");

    EngineSinkTiming timing{};
    CHECK(s.poll(&timing) == SinkResult::Ok, "poll ok");
    CHECK(!timing.valid && timing.finish_us == 0u,
          "no fabricated finish with no DAC timing model");

    bool completed = true;
    CHECK(s.stop_stream(&completed) == SinkResult::Ok, "stop_stream ok");
    CHECK(completed == false, "no completion without a physical playhead");
    CHECK(write_frames(s, 128, 1000) == 0u, "no write accepted after FINISH");

    s.disconnect();
    server.stop();
}

void test_write_when_not_streaming_is_zero() {
    std::string path = unique_socket_path("sess");
    EngineServer server;
    CHECK(server.start(path), "server start");
    EngineSinkSession s(make_config(path));
    CHECK(write_frames(s, 64, 500) == 0u, "no write before start_stream");
    CHECK(s.accepted_frames() == 0u, "cursor untouched");
    server.stop();
}

void test_cancel_fences_and_generation_increases() {
    std::string path = unique_socket_path("sess");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio");

    CHECK(s.cancel_stream() == SinkResult::Ok, "seek-clear cancel ok");
    CHECK(!s.streaming(), "fenced after cancel");
    CHECK(write_frames(s, 256, 500) == 0u, "no audio between fence and reopen");

    CHECK(s.start_stream() == SinkResult::Ok, "start successor");
    CHECK(s.generation() > gen1, "successor generation is strictly higher");
    CHECK(s.accepted_frames() == 0u, "successor cursor starts at zero");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame,
          "successor audio accepted");

    s.disconnect();
    server.stop();
}

void test_reconnect_same_engine_keeps_generation() {
    std::string path = unique_socket_path("sess");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame, "audio");

    // Disconnect the peer; the engine keeps running and retains its
    // last_generation. Reconnecting and re-opening at 1 would be refused.
    s.disconnect();
    CHECK(server.sink() != nullptr, "engine survived the peer disconnect");
    CHECK(s.start_stream() == SinkResult::Ok,
          "reconnect to the surviving engine opens a fresh generation");
    CHECK(s.generation() > gen1,
          "generation did NOT restart at 1 (engine retains last_generation)");
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame,
          "audio accepted after reconnect");

    s.disconnect();
    server.stop();
}

void test_engine_restart_new_epoch() {
    std::string path = unique_socket_path("sess");

    EngineServer server_a;
    CHECK(server_a.start(path), "server A start");
    const uint32_t epoch_a = server_a.epoch();

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start A");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame, "A audio");

    server_a.stop();
    EngineServer server_b;
    CHECK(server_b.start(path), "server B start");
    CHECK(server_b.epoch() != epoch_a, "restarted engine has a fresh epoch");

    CHECK(s.start_stream() == SinkResult::Ok, "recover on the restarted engine");
    CHECK(s.epoch() == server_b.epoch(), "session adopted the restarted epoch");
    CHECK(s.generation() > gen1, "generation stayed strictly increasing");
    CHECK(s.accepted_frames() == 0u, "no predecessor progress carried over");
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame,
          "audio accepted on the restarted engine");

    s.disconnect();
    server_b.stop();
}

void test_stalled_engine_bounds_write() {
    std::string path = unique_socket_path("sess");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start");

    // Stall the engine: a write can send to the kernel but can never be
    // credited, so it must return 0 within the clamped per-op timeout.
    server.pause();
    const auto begin = std::chrono::steady_clock::now();
    const size_t accepted = write_frames(s, 4096, 100000 /* clamped to 20 ms */);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
    CHECK(accepted == 0u, "a stalled engine credits zero bytes");
    CHECK(elapsed_ms < 2000, "a stalled engine bounds the write (no hang)");
    CHECK(s.accepted_frames() == 0u, "stalled write did not advance the cursor");

    server.resume();
    s.disconnect();
    server.stop();
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_session: bridge core vs real engine\n");
    test_stream_write_and_progress();
    test_write_when_not_streaming_is_zero();
    test_cancel_fences_and_generation_increases();
    test_reconnect_same_engine_keeps_generation();
    test_engine_restart_new_epoch();
    test_stalled_engine_bounds_write();

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr,
                     "test_sendspin_session: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_session: all checks passed\n");
    return 0;
}
