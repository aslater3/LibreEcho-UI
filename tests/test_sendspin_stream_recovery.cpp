// SPDX-License-Identifier: MIT
//
// Sendspin companion bridge — engine stream loss and re-arm (C++20) against the
// real compiled C99 engine (Platform airplay audio_sink.c).
//
// The engine drops a Sendspin generation on its own when a PCM write fails
// (sendspin_drop_generation -> le_audio_sink_cancel). The companion must then
// stop offering audio into a dead generation, notice the loss, and open a
// strictly higher generation so playback resumes without the server restarting
// the stream. Pinned here:
//   * a DATA refused with STALE_GENERATION after an engine drop is reported as
//     a stream loss, not an endless run of refusals on a still-armed sink;
//   * the loss is also noticed by a progress poll with no writes in flight;
//   * rearm_stream() opens a strictly higher generation and audio flows again;
//   * a dead engine gets a bounded number of re-arm attempts, then the companion
//     waits for the next stream start instead of retrying forever;
//   * a stream that ends while lost does not re-arm.

#include "engine_sink_session.h"
#include "sendspin_engine_fixture.h"

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

size_t write_frames(EngineSinkSession& s, size_t frames, uint32_t timeout_ms) {
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);
    return s.write(reinterpret_cast<uint8_t*>(pcm.data()),
                   frames * kBytesPerFrame, timeout_ms);
}

// Drop the live generation on the engine exactly as its PCM-error path does.
void engine_drops_generation(EngineServer& server, uint32_t generation) {
    const uint32_t epoch = server.epoch();
    server.with_sink([&](le_audio_sink* sink) {
        (void)le_audio_sink_cancel(sink, epoch, generation);
    });
}

// Keep calling rearm_stream() while the engine is still opening its PCM.
SinkResult rearm_until_settled(EngineSinkSession& s, int max_calls) {
    SinkResult r = SinkResult::Timeout;
    for (int i = 0; i < max_calls && r == SinkResult::Timeout; ++i)
        r = s.rearm_stream();
    return r;
}

void test_write_refusal_after_engine_drop_rearms() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 512, 1000) == 512 * kBytesPerFrame, "gen1 audio accepted");

    engine_drops_generation(server, gen1);

    // The engine refuses the stale generation. The companion must surface the
    // loss (and stop offering audio into it) rather than stay armed.
    CHECK(write_frames(s, 256, 500) == 0u, "dropped generation accepts no audio");
    CHECK(s.state() == SessionState::Lost, "write refusal reports the stream as lost");
    CHECK(s.rearm_pending(), "a re-arm is pending after the loss");
    CHECK(!s.streaming(), "lost stream is not streaming");

    CHECK(rearm_until_settled(s, 200) == SinkResult::Ok, "re-arm opens a successor");
    CHECK(s.generation() > gen1, "successor generation is strictly higher");
    CHECK(s.state() == SessionState::Streaming, "streaming again after re-arm");
    CHECK(s.accepted_frames() == 0u, "successor cursor starts at zero");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame,
          "audio flows again after re-arm");
    CHECK(!s.rearm_pending(), "no re-arm pending once streaming");

    struct le_audio_sink_progress progress;
    CHECK(le_audio_sink_get_progress(server.sink(), &progress) == LE_AUDIO_SINK_OK,
          "engine progress after re-arm");
    CHECK(progress.active && progress.generation == s.generation() &&
              progress.accepted_frames == 256u,
          "the real engine credited the successor generation");

    s.disconnect();
    server.stop();
}

void test_progress_poll_notices_drop_without_writes() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio accepted");

    engine_drops_generation(server, gen1);

    // No write is in flight (the feedback pump runs anyway): the progress poll
    // must see the engine's stale-generation reply and report the loss.
    EngineSinkTiming timing{};
    CHECK(s.poll(&timing) != SinkResult::Ok, "poll on a dropped generation fails");
    CHECK(s.state() == SessionState::Lost, "poll reports the stream as lost");
    CHECK(s.rearm_pending(), "a re-arm is pending after the poll loss");

    CHECK(rearm_until_settled(s, 200) == SinkResult::Ok, "re-arm after poll loss");
    CHECK(s.generation() > gen1, "successor generation is strictly higher");
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame,
          "audio accepted after poll-driven re-arm");

    s.disconnect();
    server.stop();
}

void test_rearm_attempts_are_bounded_when_engine_is_gone() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame, "gen1 audio accepted");

    engine_drops_generation(server, gen1);
    server.stop();  // the engine is gone: no listener to re-arm against

    CHECK(write_frames(s, 128, 500) == 0u, "no audio into a dead engine");
    CHECK(s.state() == SessionState::Lost, "dead engine reports the stream as lost");

    uint32_t failed_calls = 0;
    for (int i = 0; i < 50 && s.rearm_pending(); ++i) {
        CHECK(s.rearm_stream() != SinkResult::Ok, "re-arm cannot succeed without an engine");
        ++failed_calls;
    }
    CHECK(failed_calls == libreecho::sendspin::kRearmMaxFailures,
          "re-arm budget is exactly kRearmMaxFailures");
    CHECK(!s.rearm_pending(), "budget spent: no further re-arm is pending");
    CHECK(s.rearm_stream() == SinkResult::NotOpen, "spent budget does not attempt again");
    CHECK(s.rearm_attempts() == libreecho::sendspin::kRearmMaxFailures,
          "exactly kRearmMaxFailures attempts reached the engine path");

    s.disconnect();
}

void test_stream_end_while_lost_does_not_rearm() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio accepted");

    engine_drops_generation(server, gen1);
    CHECK(write_frames(s, 64, 500) == 0u, "refused after drop");
    CHECK(s.state() == SessionState::Lost, "lost after refusal");

    // The SDK ends the stream before a re-arm succeeded: nothing is re-opened.
    CHECK(s.stop_stream() == SinkResult::NotOpen, "natural end of a lost stream is NotOpen");
    CHECK(!s.rearm_pending(), "ended stream leaves no re-arm pending");
    const uint32_t attempts_before = s.rearm_attempts();
    CHECK(s.rearm_stream() == SinkResult::NotOpen, "no re-arm after the stream ended");
    CHECK(s.rearm_attempts() == attempts_before, "no engine attempt after the stream ended");

    // A later stream starts normally at a higher generation.
    CHECK(s.start_stream() == SinkResult::Ok, "next stream opens normally");
    CHECK(s.generation() > gen1, "next stream generation is strictly higher");

    s.disconnect();
    server.stop();
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_stream_recovery: engine drop and re-arm vs real engine\n");
    test_write_refusal_after_engine_drop_rearms();
    test_progress_poll_notices_drop_without_writes();
    test_rearm_attempts_are_bounded_when_engine_is_gone();
    test_stream_end_while_lost_does_not_rearm();

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr,
                     "test_sendspin_stream_recovery: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_stream_recovery: all checks passed\n");
    return 0;
}
