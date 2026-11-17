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

// Offer `frames` frames of pattern PCM with the first sample XORed by `flip`,
// so a retry can differ from the offer that left a DATA pending.
size_t write_variant(EngineSinkSession& s, size_t frames, uint32_t timeout_ms,
                     int16_t flip) {
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);
    pcm[0] = static_cast<int16_t>(pcm[0] ^ flip);
    return s.write(reinterpret_cast<uint8_t*>(pcm.data()),
                   frames * kBytesPerFrame, timeout_ms);
}

// A stalled engine leaves a DATA pending without credit; the SDK then offers
// different bytes (hard-sync prefix drop, gap-fill silence, mute change).
void leave_data_pending(EngineServer& server, EngineSinkSession& s) {
    server.pause();
    CHECK(write_variant(s, 128, 20, 0) == 0u, "stalled engine credits nothing");
    CHECK(s.streaming(), "stream still open while the DATA awaits its credit");
}

void test_changed_offer_with_pending_data_rearms() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio accepted");
    const uint64_t accepted_before = s.accepted_frames();

    leave_data_pending(server, s);
    CHECK(write_variant(s, 128, 20, 0x55) == 0u,
          "changed offer is not credited with the abandoned DATA's bytes");
    server.resume();

    CHECK(s.accepted_frames() == accepted_before,
          "no frames of the abandoned datagram are counted as accepted");
    CHECK(s.state() == SessionState::Lost, "changed offer reports the stream as lost");
    CHECK(s.rearm_pending(), "a re-arm is pending after the changed offer");

    CHECK(rearm_until_settled(s, 200) == SinkResult::Ok, "re-arm opens a successor");
    CHECK(s.generation() > gen1, "successor generation is strictly higher");
    CHECK(s.state() == SessionState::Streaming, "streaming again after re-arm");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame,
          "audio accepted again after re-arm");
    CHECK(s.accepted_frames() == 256u, "only the new offer is counted on the successor");

    s.disconnect();
    server.stop();
}

void test_fresh_stream_after_changed_offer() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio accepted");

    leave_data_pending(server, s);
    CHECK(write_variant(s, 128, 20, 0x55) == 0u, "changed offer accepts nothing");
    server.resume();

    // What the listener does on a stream end then start, with no re-arm between.
    (void)s.cancel_stream();
    CHECK(s.start_stream() == SinkResult::Ok, "fresh stream opens after the failure");
    CHECK(s.generation() > gen1, "fresh stream generation is strictly higher");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame,
          "audio accepted on the fresh stream");

    s.disconnect();
    server.stop();
}

void test_request_rearm_retries_a_failed_start() {
    std::string path = unique_socket_path("rearm");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(s.cancel_stream() == SinkResult::Ok, "fence gen1");
    CHECK(!s.rearm_pending(), "a fenced stream has no re-arm pending");

    // A failed start/cancel leaves the SDK stream open with no generation.
    s.request_rearm("test");
    CHECK(s.state() == SessionState::Lost && s.rearm_pending(), "request marks the stream lost");
    CHECK(rearm_until_settled(s, 200) == SinkResult::Ok, "re-arm opens a successor");
    CHECK(s.generation() > gen1, "successor generation is strictly higher");
    CHECK(write_frames(s, 128, 1000) == 128 * kBytesPerFrame, "audio accepted after the requested re-arm");

    s.disconnect();
    server.stop();
}


// An engine that outlived a previous sendspind has seen far higher generations
// than a restarted session (which starts at 1). Raise the engine's
// last_generation by opening and fencing `gen` from a throwaway session.
void engine_has_seen(EngineServer& server, const std::string& path, uint32_t gen) {
    (void)server;
    EngineSinkSessionConfig cfg = make_config(path);
    cfg.first_generation = gen;
    EngineSinkSession prior(cfg);
    SinkResult r = SinkResult::Timeout;
    for (int i = 0; i < 200 && r == SinkResult::Timeout; ++i) r = prior.start_stream();
    CHECK(r == SinkResult::Ok, "prior session opens high generation");
    r = SinkResult::Timeout;
    for (int i = 0; i < 200 && r == SinkResult::Timeout; ++i) r = prior.cancel_stream();
    CHECK(r == SinkResult::Ok, "prior session fences it");
    prior.disconnect();
}

void test_start_probes_past_high_engine_generation() {
    std::string path = unique_socket_path("probe");
    EngineServer server;
    CHECK(server.start(path), "server start");
    engine_has_seen(server, path, 1000);

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start probes past generation 1000");
    const uint32_t gen = s.generation();
    CHECK(gen > 1000u && gen < 1000u + 100000u, "opened just above the engine's generation");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "audio accepted after probing");

    // The next start continues upward; it does not probe again from 1.
    CHECK(s.cancel_stream() == SinkResult::Ok, "fence");
    CHECK(s.start_stream() == SinkResult::Ok, "second start");
    CHECK(s.generation() == gen + 1u, "second start used the next generation, no re-probe");

    s.disconnect();
    server.stop();
}

void test_rearm_probes_past_high_engine_generation() {
    std::string path = unique_socket_path("probe");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Ok, "start gen1");
    const uint32_t gen1 = s.generation();
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "gen1 audio accepted");

    // The engine loses the generation and has meanwhile moved far ahead.
    engine_drops_generation(server, gen1);
    s.disconnect();  // the engine serves one peer at a time
    engine_has_seen(server, path, 5000);
    s.request_rearm("test");
    CHECK(s.state() == SessionState::Lost, "loss recorded");
    CHECK(rearm_until_settled(s, 200) == SinkResult::Ok, "re-arm probes past the engine's generation");
    CHECK(s.generation() > 5000u, "re-armed above the engine's generation");
    CHECK(write_frames(s, 256, 1000) == 256 * kBytesPerFrame, "audio accepted after re-arm");

    s.disconnect();
    server.stop();
}

// Scripted engine answering every OPEN with `status_for(generation)`; records
// the generations it was asked to open.
struct OpenScript {
    std::mutex mu;
    std::vector<uint32_t> seen;
    std::vector<uint32_t> snapshot() {
        std::lock_guard<std::mutex> g(mu);
        return seen;
    }
};

void serve_opens(OpenScript& script, int fd, uint32_t (*status_for)(uint32_t)) {
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = raw_recv(fd, buf, sizeof(buf));
        if (n < (ssize_t)(LE_AUDIO_SINK_HEADER_BYTES + 8)) return;
        const uint32_t gen = le_audio_sink_get_u32(buf + LE_AUDIO_SINK_HEADER_BYTES + 4);
        { std::lock_guard<std::mutex> g(script.mu); script.seen.push_back(gen); }
        uint8_t out[LE_AUDIO_SINK_HEADER_BYTES + LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
        le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_OPEN_ACK,
                                    LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES);
        uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
        std::memset(p, 0, LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES);
        le_audio_sink_put_u32(p + 0, status_for(gen));
        le_audio_sink_put_u32(p + 4, 0x1234u);
        le_audio_sink_put_u32(p + 8, gen);
        le_audio_sink_put_u32(p + 12, LE_AUDIO_SINK_OUTPUT_RATE);
        le_audio_sink_put_u16(p + 16, LE_AUDIO_SINK_OUTPUT_CHANNELS);
        le_audio_sink_put_u16(p + 18, LE_AUDIO_SINK_FORMAT_S16_LE);
        le_audio_sink_put_u32(p + 28, LE_AUDIO_SINK_OPEN_READY);
        raw_send(fd, out, sizeof(out));
    }
}

uint32_t geometry_after_two_stale(uint32_t gen) {
    return gen <= 2u ? LE_AUDIO_SINK_ERR_STALE_GENERATION : LE_AUDIO_SINK_ERR_GEOMETRY;
}

uint32_t always_stale(uint32_t) { return LE_AUDIO_SINK_ERR_STALE_GENERATION; }

void test_non_stale_rejection_stops_probing() {
    std::string path = unique_socket_path("probe");
    OpenScript script;
    RawPeer peer;
    CHECK(peer.start(path, [&](int fd) { serve_opens(script, fd, geometry_after_two_stale); }),
          "peer start");
    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Rejected, "GEOMETRY refusal fails the start");
    const std::vector<uint32_t> seen = script.snapshot();
    CHECK(seen.size() == 3u && seen[0] == 1u && seen[1] == 2u && seen[2] == 4u,
          "probed 1, 2, 4 then stopped on the non-stale refusal");
    CHECK(s.state() == SessionState::Connected, "session not armed");
    s.disconnect();
    peer.stop();
}

void test_probe_bound_and_never_lose_ground() {
    std::string path = unique_socket_path("probe");
    OpenScript script;
    RawPeer peer;
    CHECK(peer.start(path, [&](int fd) { serve_opens(script, fd, always_stale); }),
          "peer start");
    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Rejected, "engine too far ahead: start fails");
    std::vector<uint32_t> first = script.snapshot();
    CHECK(first.size() == 33u, "one OPEN plus 32 probes, then gave up");
    CHECK(first.front() == 1u, "first call starts at 1");
    for (size_t i = 1; i < first.size(); ++i)
        CHECK(first[i] > first[i - 1], "probe generations strictly increase");
    CHECK(s.start_stream() == SinkResult::Rejected, "still too far ahead");
    std::vector<uint32_t> all = script.snapshot();
    CHECK(all.size() == 66u, "second call probed again");
    CHECK(all[33] == first.back() + 1u,
          "second call starts one above the highest generation already refused");
    s.disconnect();
    peer.stop();
}

void test_probe_bound_then_success_on_later_call() {
    std::string path = unique_socket_path("probe");
    EngineServer server;
    CHECK(server.start(path), "server start");
    engine_has_seen(server, path, 3000000u);

    EngineSinkSession s(make_config(path));
    CHECK(s.start_stream() == SinkResult::Rejected, "beyond one call's reach: start fails");
    CHECK(s.state() == SessionState::Connected, "not armed");
    SinkResult r = SinkResult::Rejected;
    for (int i = 0; i < 4 && r != SinkResult::Ok; ++i) r = s.start_stream();
    CHECK(r == SinkResult::Ok, "later calls continue upward and succeed");
    CHECK(s.generation() > 3000000u, "opened above the engine's generation");
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
    test_changed_offer_with_pending_data_rearms();
    test_fresh_stream_after_changed_offer();
    test_request_rearm_retries_a_failed_start();
    test_start_probes_past_high_engine_generation();
    test_rearm_probes_past_high_engine_generation();
    test_non_stale_rejection_stops_probing();
    test_probe_bound_and_never_lose_ground();
    test_probe_bound_then_success_on_later_call();

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr,
                     "test_sendspin_stream_recovery: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_stream_recovery: all checks passed\n");
    return 0;
}
