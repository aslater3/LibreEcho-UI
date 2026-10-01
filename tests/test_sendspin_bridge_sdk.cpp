// SPDX-License-Identifier: MIT
//
// Task 5 — real-SDK bridge tests.
//
// Unlike ui/tests/test_sendspin_sdk.cpp (which drives the SDK against a *mock*
// sink listener), this file binds the *real* SdkPlayerListener + the *real*
// EngineSinkSession to the *real* RC1 SDK PlayerRole and the *real* compiled
// C99 engine (Platform airplay audio_sink.c). It is the regression net for the
// three Task 5 defects:
//
//   P3 feedback semantics — notify_audio_played() must be handed the finish
//      time of exactly the frames it reports played, never the end of the whole
//      submitted tail (which the SDK would then lengthen again by the still
//      buffered frames: a double finish-delay), and never enqueue time.
//   P2 natural-tail lifecycle — a natural server stream/end must let the engine
//      drain its queued tail, while a role removal / disconnect must fence and
//      drop. The pinned RC1 public API is checked for a discriminator.
//   P4 SDK-linked loopback — client connection, player-role registration and
//      main-loop pumping against a loopback fake server, with real PCM accepted
//      by the real engine and real feedback derived from the engine's cursors.
//
// No host hardware, /run path or device node is touched; every socket lives
// under a short private scratch dir.

#include "lifecycle_test_fixtures.h"   // real RC1 client + strict encrypted fake server
#include "sendspin_engine_fixture.h"   // real compiled C99 engine server

#include "engine_sink_session.h"
#include "sdk_player_listener.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace sendspin;  // RC1 SDK
using libreecho::sendspin::EngineSinkSession;
using libreecho::sendspin::EngineSinkSessionConfig;
using libreecho::sendspin::EngineSinkTiming;
using libreecho::sendspin::SdkPlayerListener;
using libreecho::sendspin::SessionState;
using libreecho::sendspin::SinkResult;
using namespace libreecho::sendspin::test;

namespace {

constexpr uint32_t kRate = LE_AUDIO_SINK_OUTPUT_RATE;      // 48000
constexpr size_t kFrameBytes = LE_AUDIO_SINK_BYTES_PER_FRAME;

// Exit codes: 0 all good; 1 a real regression; 2 blocked by a documented SDK
// interface gap (see P2). Kept distinct so CI can tell a bug from a pin.
int g_blocked = 0;

#define BRIDGE_CHECK(cond, msg)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

#define BLOCKED_CHECK(cond, msg)                                                \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "BLOCKED %s:%d: %s\n", __FILE__, __LINE__,      \
                         (msg));                                                \
            ++g_blocked;                                                        \
        }                                                                       \
    } while (0)

// ---- deterministic, *ordered* fake PCM ------------------------------------
// A monotone ramp so ordering, duplication and shuffling are all detectable:
// frame k is (0x1234 + 7k, 0x0abc - 3k) modulo 16 bits. (fill_pattern() in the
// shared fixture is a *constant* frame, which an ordering check could not see.)
std::vector<int16_t> ramp_pcm(size_t frame_count, uint64_t first_frame) {
    std::vector<int16_t> pcm(frame_count * 2);
    for (size_t i = 0; i < frame_count; ++i) {
        const uint64_t k = first_frame + i;
        pcm[i * 2] = static_cast<int16_t>(0x1234 + (k * 7u));
        pcm[i * 2 + 1] = static_cast<int16_t>(0x0abc - (k * 3u));
    }
    return pcm;
}

// Fraction of consecutive pairs whose left channel advances by the ramp step,
// measured over the non-silent part (the SDK primes with silence and may
// duplicate/drop a frame for fine sync correction, which the ramp tolerates).
double ramp_step_fraction(const std::vector<int16_t>& interleaved) {
    size_t begin = 0;
    while (begin + 1 < interleaved.size() && interleaved[begin * 2] == 0 &&
           interleaved[begin * 2 + 1] == 0) {
        ++begin;
    }
    if (interleaved.size() < begin + 2) return 0.0;
    size_t ok = 0, total = 0;
    for (size_t i = begin; i + 1 < interleaved.size() / 2; ++i) {
        const int diff = static_cast<int>(interleaved[(i + 1) * 2]) -
                         static_cast<int>(interleaved[i * 2]);
        ++total;
        if (diff == 7 || diff == 7 - 0x10000) ++ok;
    }
    return total == 0 ? 0.0
                      : static_cast<double>(ok) / static_cast<double>(total);
}

EngineSinkSessionConfig make_session_config(const std::string& path) {
    EngineSinkSessionConfig cfg;
    cfg.socket_path = path;
    cfg.connect_timeout_ms = 1000;
    cfg.control_timeout_ms = 20;
    cfg.max_write_timeout_ms = 20;
    return cfg;
}

// ---- a real engine DAC/playhead driver (the fakeTinyALSA analogue) ---------
// One background thread does render + commit + note_playhead + note_timing on
// the real audio_sink.c, exactly as run_engine's single thread does, so the
// SDK's sync thread is handed genuine engine feedback rather than a stub. It
// holds the fixture's mutex for every engine call, so it never races servicing.
class DacDriver {
public:
    DacDriver(EngineServer& server, uint32_t epoch, uint32_t generation)
        : server_(server), epoch_(epoch), generation_(generation) {}

    void start() {
        t0_us_ = platform_time_us();
        run_.store(true);
        thread_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_.store(false);
        if (thread_.joinable()) thread_.join();
    }
    // Freeze playback (stop consuming) without tearing the sink down, so a test
    // can observe exactly what a lifecycle callback did to the queue.
    void freeze() { freeze_.store(true); }

    uint64_t played() const { return played_.load(); }
    uint64_t submitted() const { return submitted_.load(); }
    uint64_t rendered_frames() const { return (uint64_t)captured_.size(); }
    std::vector<int16_t> captured() {
        std::lock_guard<std::mutex> g(capture_mu_);
        return captured_;
    }

private:
    void loop() {
        int16_t scratch[LE_AUDIO_SINK_MAX_DATA_FRAMES * 2];
        while (run_.load()) {
            if (!freeze_.load()) {
                const int64_t elapsed_us = platform_time_us() - t0_us_;
                const uint64_t target =
                    (uint64_t)std::max<int64_t>(elapsed_us, 0) * kRate / 1000000u;
                server_.with_sink([&](le_audio_sink* sink) {
                    uint64_t first = 0;
                    const size_t got = le_audio_sink_render(
                        sink, scratch, LE_AUDIO_SINK_MAX_DATA_FRAMES, &first);
                    if (got > 0 &&
                        le_audio_sink_commit(sink, epoch_, generation_, got) ==
                            LE_AUDIO_SINK_OK) {
                        std::lock_guard<std::mutex> g(capture_mu_);
                        cap_.assign(scratch, scratch + got * 2);
                        captured_.insert(captured_.end(), cap_.begin(), cap_.end());
                        submitted_.fetch_add(got);
                    }
                    const uint64_t sub = submitted_.load();
                    const uint64_t play = std::min(target, sub);
                    if (play > 0) {
                        le_audio_sink_note_playhead(sink, epoch_, generation_, play);
                        played_.store(play);
                    }
                    if (sub > 0) {
                        const uint64_t finish_us =
                            (uint64_t)t0_us_ + sub * 1000000ull / kRate;
                        le_audio_sink_note_timing(sink, epoch_, generation_,
                                                  LE_AUDIO_SINK_PROGRESS_TIMING_VALID,
                                                  finish_us);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    EngineServer& server_;
    uint32_t epoch_;
    uint32_t generation_;
    int64_t t0_us_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<bool> freeze_{false};
    std::thread thread_;
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> played_{0};
    std::mutex capture_mu_;
    std::vector<int16_t> captured_;
    std::vector<int16_t> cap_;
};

// ===========================================================================
// P3 — feedback semantics: real SdkPlayerListener, real engine timing.
// ===========================================================================
void test_feedback_pair_is_the_played_cursor_finish() {
    const std::string path = unique_socket_path("brdg");
    EngineServer server;
    BRIDGE_CHECK(server.start(path), "engine server start");

    EngineSinkSession session(make_session_config(path));
    BRIDGE_CHECK(session.start_stream() == SinkResult::Ok, "session OPEN gen1");

    // A *real* SDK PlayerRole. Its sync task is not running here, so
    // notify_audio_played() is a guarded no-op that still exercises the real
    // object; the assertion reads the listener's own emitted pair.
    SendspinClientConfig cfg;
    cfg.name = "LibreEcho bridge P3";
    SendspinClient client(std::move(cfg));
    PlayerRole& player = client.add_player(make_pcm_player_config());

    SdkPlayerListener listener(session, &player);
    listener.on_stream_start();

    // Deterministic fake PCM: 2048 frames accepted by the engine.
    const size_t kFrames = 2048;
    std::vector<int16_t> pcm = ramp_pcm(kFrames, 0);
    const size_t accepted =
        listener.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                kFrames * kFrameBytes, 1000);
    BRIDGE_CHECK(accepted == kFrames * kFrameBytes,
                 "engine acknowledged all offered frames");

    // Model the DAC: hand 1024 frames to the hardware timeline, then move the
    // physical playhead to 768 and publish the submitted tail's endpoint.
    const uint32_t epoch = session.epoch();
    const uint32_t gen = session.generation();
    const size_t kHw = 1024;
    const uint64_t kPlayed = 768;
    const uint64_t kTailFinishUs = 4'000'000;  // engine-published endpoint
    BRIDGE_CHECK(server.with_sink([&](le_audio_sink* sink) {
        int16_t scratch[LE_AUDIO_SINK_MAX_DATA_FRAMES * 2];
        uint64_t first = 0;
        if (le_audio_sink_render(sink, scratch, kHw, &first) != kHw) return;
        le_audio_sink_commit(sink, epoch, gen, kHw);
        le_audio_sink_note_playhead(sink, epoch, gen, kPlayed);
        le_audio_sink_note_timing(sink, epoch, gen,
                                  LE_AUDIO_SINK_PROGRESS_TIMING_VALID,
                                  kTailFinishUs);
    }), "inject engine timing");

    EngineSinkTiming t{};
    BRIDGE_CHECK(session.poll(&t) == SinkResult::Ok && t.valid, "poll valid");
    BRIDGE_CHECK(t.submitted_frames == kHw && t.played_frames == kPlayed &&
                     t.finish_us == kTailFinishUs,
                 "engine published exactly the injected cursors");

    BRIDGE_CHECK(listener.pump_feedback(), "one feedback report emitted");
    const uint64_t in_flight = t.submitted_frames - t.played_frames;  // 256
    const uint64_t in_flight_us = in_flight * 1000000ull / kRate;
    // The reported timestamp must be the *played cursor's* finish, not the
    // submitted tail's (which would double-count the still-buffered frames).
    BRIDGE_CHECK(listener.last_report_frames() == kPlayed,
                 "reported the played-cursor delta, not the accepted total");
    BRIDGE_CHECK(listener.last_report_timestamp_us() ==
                     (int64_t)(kTailFinishUs - in_flight_us),
                 "reported the played cursor's finish (tail endpoint backed off "
                 "by the in-flight hardware frames)");
    BRIDGE_CHECK(listener.last_report_timestamp_us() != (int64_t)kTailFinishUs,
                 "did not report the whole submitted tail's finish");

    // The SDK's own arithmetic (sync_task.cpp) is
    //   endpoint = reported_timestamp + frames_to_us(accepted - played)
    // and it must land on the true buffer endpoint, or it double-counts.
    const uint64_t accepted_frames = session.accepted_frames();  // 2048
    const uint64_t queued = accepted_frames - t.submitted_frames; // 1024
    const int64_t derived_endpoint =
        listener.last_report_timestamp_us() +
        (int64_t)((accepted_frames - kPlayed) * 1000000ull / kRate);
    BRIDGE_CHECK(derived_endpoint ==
                     (int64_t)(kTailFinishUs + queued * 1000000ull / kRate),
                 "SDK-derived endpoint == engine tail endpoint (no double delay)");

    // Exactly once: no second report without a played-cursor advance.
    const uint32_t frames_before = listener.last_report_frames();
    BRIDGE_CHECK(!listener.pump_feedback(), "no duplicate report for the same cursor");
    BRIDGE_CHECK(listener.last_report_frames() == frames_before &&
                     listener.report_count() == 1,
                 "report count stayed exactly one");

    // Advance the playhead: a second, strictly larger report.
    BRIDGE_CHECK(server.with_sink([&](le_audio_sink* sink) {
        le_audio_sink_note_playhead(sink, epoch, gen, kHw);
        le_audio_sink_note_timing(sink, epoch, gen,
                                  LE_AUDIO_SINK_PROGRESS_TIMING_VALID,
                                  kTailFinishUs);
    }), "advance playhead");
    BRIDGE_CHECK(listener.pump_feedback(), "second report emitted");
    BRIDGE_CHECK(listener.last_report_frames() == (uint32_t)(kHw - kPlayed) &&
                     listener.last_report_timestamp_us() == (int64_t)kTailFinishUs,
                 "second report = remaining delta at the tail endpoint");
    BRIDGE_CHECK(listener.reported_frames() == kHw,
                 "cumulative reported frames == engine played cursor");

    // Invalid timing must never fabricate a report.
    BRIDGE_CHECK(server.with_sink([&](le_audio_sink* sink) {
        le_audio_sink_note_timing(sink, epoch, gen,
                                  LE_AUDIO_SINK_PROGRESS_TIMING_INVALID, 0);
    }), "invalidate timing");
    BRIDGE_CHECK(!listener.pump_feedback(), "no report without valid timing");

    session.disconnect();
    server.stop();
}

// Natural end via the companion's explicit control path: the queued tail must
// still be reported as it drains, exactly once.
void test_natural_tail_is_reported_exactly_once() {
    const std::string path = unique_socket_path("brdg");
    EngineServer server;
    BRIDGE_CHECK(server.start(path), "engine server start");

    EngineSinkSession session(make_session_config(path));
    BRIDGE_CHECK(session.start_stream() == SinkResult::Ok, "session OPEN");

    SendspinClientConfig cfg;
    cfg.name = "LibreEcho bridge tail";
    SendspinClient client(std::move(cfg));
    PlayerRole& player = client.add_player(make_pcm_player_config());
    SdkPlayerListener listener(session, &player);
    listener.on_stream_start();

    const size_t kFrames = 4096;
    std::vector<int16_t> pcm = ramp_pcm(kFrames, 0);
    BRIDGE_CHECK(listener.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                         kFrames * kFrameBytes, 1000) ==
                     kFrames * kFrameBytes,
                 "2048 periods accepted");

    const uint32_t epoch = session.epoch();
    const uint32_t gen = session.generation();

    // Partial drain, then a natural FINISH of the exact accepted total.
    BRIDGE_CHECK(server.with_sink([&](le_audio_sink* sink) {
        int16_t scratch[LE_AUDIO_SINK_MAX_DATA_FRAMES * 2];
        uint64_t first = 0;
        size_t got = le_audio_sink_render(sink, scratch, 2048, &first);
        le_audio_sink_commit(sink, epoch, gen, got);
        le_audio_sink_note_playhead(sink, epoch, gen, 1024);
        le_audio_sink_note_timing(sink, epoch, gen,
                                  LE_AUDIO_SINK_PROGRESS_TIMING_VALID, 2'000'000);
    }), "partial drain");
    BRIDGE_CHECK(listener.pump_feedback(), "partial feedback");

    bool completed = true;
    BRIDGE_CHECK(listener.stop_stream(&completed) == SinkResult::Ok,
                 "natural FINISH accepted the exact total");
    BRIDGE_CHECK(!completed, "not complete before the playhead passes the tail");

    // The queued tail must be handed to the hardware timeline before its
    // playhead can advance (the engine rejects a cursor above `submitted`), then
    // the physical playhead reaches the exact tail. The remaining frames must be
    // reported once and the generation must report completion.
    BRIDGE_CHECK(server.with_sink([&](le_audio_sink* sink) {
        int16_t scratch[LE_AUDIO_SINK_MAX_DATA_FRAMES * 2];
        uint64_t first = 0;
        const size_t got = le_audio_sink_render(sink, scratch, kFrames, &first);
        le_audio_sink_commit(sink, epoch, gen, got);
        le_audio_sink_note_playhead(sink, epoch, gen, kFrames);
        le_audio_sink_note_timing(sink, epoch, gen,
                                  LE_AUDIO_SINK_PROGRESS_TIMING_VALID, 3'000'000);
    }), "playhead reaches the tail");
    BRIDGE_CHECK(listener.pump_feedback(), "final tail reported");
    BRIDGE_CHECK(listener.reported_frames() == kFrames,
                 "the natural tail was reported in full, no frames left behind");
    // After the exact tail there is nothing left to report.
    BRIDGE_CHECK(!listener.pump_feedback(),
                 "no further report once the exact tail is reached");
    uint64_t completed_flag = 0;
    server.with_sink([&](le_audio_sink* sink) {
        struct le_audio_sink_progress p;
        if (le_audio_sink_get_progress(sink, &p) == LE_AUDIO_SINK_OK) {
            completed_flag = (uint64_t)p.completed;
        }
    });
    BRIDGE_CHECK(completed_flag == 1u,
                 "the engine reports the FINISHed generation complete only once "
                 "the playhead passed the exact tail");

    session.disconnect();
    server.stop();
}

// P3 arithmetic: the played-finish derivation must never overflow, underflow,
// or fabricate a timestamp, and must reject inconsistent cursors. Derived from
// the real SDK/engine contract (frames -> us at the frozen 48 kHz ABI).
void test_timing_arithmetic_bounds() {
    uint64_t us = 0;
    BRIDGE_CHECK(!SdkPlayerListener::frames_to_us(UINT64_MAX, &us),
                 "P3: frames_to_us refuses an overflow");
    BRIDGE_CHECK(SdkPlayerListener::frames_to_us(kRate, &us) &&
                     us == 1'000'000,
                 "P3: frames_to_us(48000) == 1s");

    EngineSinkTiming t{};
    int64_t finish = 0;
    // Invalid / zero-finish samples never produce a value.
    BRIDGE_CHECK(!SdkPlayerListener::derive_played_finish_us(t, &finish),
                 "P3: invalid timing yields no finish time");
    t.valid = true;
    BRIDGE_CHECK(!SdkPlayerListener::derive_played_finish_us(t, &finish),
                 "P3: zero finish_us yields no finish time");
    // played > submitted is inconsistent.
    t.finish_us = 8'000;
    t.submitted_frames = 100;
    t.played_frames = 200;
    BRIDGE_CHECK(!SdkPlayerListener::derive_played_finish_us(t, &finish),
                 "P3: played > submitted is rejected");
    // The in-flight back-off must not underflow the finish time.
    t.submitted_frames = 1024;
    t.played_frames = 768;
    t.finish_us = 5'000;  // in-flight = 256 frames = 5333us > 5000
    BRIDGE_CHECK(!SdkPlayerListener::derive_played_finish_us(t, &finish),
                 "P3: in-flight back-off can never underflow past the endpoint");
    // A valid sample derives exactly finish - frames_to_us(in_flight).
    t.finish_us = 8'000;
    const uint64_t in_flight_us = 256 * 1'000'000ull / kRate;  // 5333
    BRIDGE_CHECK(SdkPlayerListener::derive_played_finish_us(t, &finish) &&
                     finish == (int64_t)(8'000 - in_flight_us),
                 "P3: valid sample derives the played cursor's finish");
}

}  // namespace

// ===========================================================================
// P2 — the natural-end disposition pin and P4 — SDK-linked loopback.
// Defined in a second TU section below (same file, main() at the end).
// ===========================================================================
namespace {

// The fake server the bridge talks to: the SDK's strict encrypted server plus a
// monotone ramp PCM sender, so accepted bytes are provably ordered.
class BridgeServer : public FakeEncryptedServer {
public:
    using FakeEncryptedServer::FakeEncryptedServer;

    bool send_ramp_pcm(int64_t timestamp_us, size_t frames, uint64_t first_frame) {
        std::string body;
        body.reserve(4 + frames * kFrameBytes);
        for (int shift = 24; shift >= 0; shift -= 8) {
            body.push_back(static_cast<char>(0));  // send_ahead_us = 0
        }
        for (size_t i = 0; i < frames; ++i) {
            const uint64_t k = first_frame + i;
            const uint16_t l = static_cast<uint16_t>(0x1234 + (k * 7u));
            const uint16_t r = static_cast<uint16_t>(0x0abc - (k * 3u));
            body.push_back(static_cast<char>(l & 0xFF));
            body.push_back(static_cast<char>((l >> 8) & 0xFF));
            body.push_back(static_cast<char>(r & 0xFF));
            body.push_back(static_cast<char>((r >> 8) & 0xFF));
        }
        return this->send_binary(4, timestamp_us, body);
    }
};

std::string activate_without_player_json() {
    return R"({"type":"server/activate","payload":{"activities":["playback"],)"
           R"("active_roles":["controller@v1"]}})";
}

SendspinClientConfig make_client_config(uint16_t port) {
    SendspinClientConfig config;
    config.name = "LibreEcho Sendspin bridge";
    config.server_port = port;
    config.time_burst_interval_ms = 100;
    return config;
}

struct BridgeRig {
    std::string socket_path;
    EngineServer engine;
    std::unique_ptr<EngineSinkSession> session;
    std::unique_ptr<PairedClientBundle> bundle;
    PlayerRole* player = nullptr;
    std::unique_ptr<SdkPlayerListener> listener;
    std::unique_ptr<DacDriver> dac;
    std::unique_ptr<BridgeServer> server;

    // Ordered teardown: stop the fake DAC, then the fake server, then the
    // client (which stops the SDK sync task and its callbacks), then the
    // listener, then the engine session.
    void close() {
        if (dac) { dac->stop(); dac.reset(); }
        server.reset();
        bundle.reset();
        listener.reset();
        if (session) session->disconnect();
        engine.stop();
    }
    ~BridgeRig() { close(); }
};

void build_rig(BridgeRig& rig, uint16_t port) {
    rig.socket_path = unique_socket_path("brdg");
    BRIDGE_CHECK(rig.engine.start(rig.socket_path), "engine server start");
    rig.session = std::make_unique<EngineSinkSession>(
        make_session_config(rig.socket_path));
    rig.bundle = std::make_unique<PairedClientBundle>(make_client_config(port));
    rig.player = &rig.bundle->client().add_player(make_pcm_player_config());
    rig.listener = std::make_unique<SdkPlayerListener>(*rig.session, rig.player);
    rig.player->set_listener(rig.listener.get());
    BRIDGE_CHECK(rig.bundle->start(), "client start()");
}

void connect_server(BridgeRig& rig, uint16_t port, const char* roles) {
    FakeEncryptedServerOptions options;
    options.answer_time = true;
    options.first_roles_json = roles;
    rig.server = std::make_unique<BridgeServer>(
        server_url(port), std::string(NOISE_SUITE_CHACHAPOLY),
        rig.bundle->peer.server_identity, rig.bundle->peer.record.psk_id,
        rig.bundle->peer.psk, options);
}

uint64_t engine_queued(EngineServer& engine) {
    uint64_t queued = 0;
    engine.with_sink([&](le_audio_sink* sink) {
        struct le_audio_sink_progress p;
        if (le_audio_sink_get_progress(sink, &p) == LE_AUDIO_SINK_OK) {
            queued = p.queued_frames;
        }
    });
    return queued;
}

// Pump the SDK main loop for `ms`, running the companion's feedback pump each
// iteration exactly as the real sendspind main loop does.
void pump_bridge_for(SendspinClient& client, SdkPlayerListener& listener, int ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        listener.pump_feedback();
        pump_for(client, 5);
    }
}

// Bounded predicate pump: never hangs, unlike the fixture's unbounded helpers.
bool pump_bridge_until(SendspinClient& client, SdkPlayerListener& listener,
                       const std::function<bool()>& pred, int max_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(max_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        listener.pump_feedback();
        pump_for(client, 5);
    }
    return pred();
}

// Feed real ramp PCM through the SDK until the engine has accepted `target`
// frames, or a bounded deadline expires.
void feed_until_accepted(SendspinClient& client, BridgeServer& server,
                         EngineSinkSession& session, SdkPlayerListener& listener,
                         uint64_t target) {
    int64_t ts = platform_time_us() + 100 * 1000;
    uint64_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (session.accepted_frames() < target &&
           std::chrono::steady_clock::now() < deadline) {
        server.send_ramp_pcm(ts, 960, sent);
        ts += 20 * 1000;
        sent += 960;
        listener.pump_feedback();
        pump_for(client, 20);
    }
}

// Bounded wait for the SDK sync thread to go idle: returns the accepted-frame
// count once it has been observed unchanged across several short intervals (or
// the deadline expires). Used before sampling engine queue state so the
// measurement is not taken while a feeder is still advancing it.
uint64_t wait_accepted_stable(EngineSinkSession& session, int max_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(max_ms);
    uint64_t last = session.accepted_frames();
    int stable = 0;
    while (std::chrono::steady_clock::now() < deadline && stable < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const uint64_t now = session.accepted_frames();
        stable = (now == last) ? stable + 1 : 0;
        last = now;
    }
    return last;
}

// Deterministic queued tail: the fake DAC has been stopped+joined (so nothing
// can drain the queue), the SDK sync thread has been allowed to go idle, and a
// bounded burst is pushed through the SAME real listener -> session -> engine
// path. Returns the frames the engine acknowledged. The engine queue then holds
// at least this many unrendered frames, so a lifecycle callback's effect on the
// queue is measured deterministically rather than sampled out from under an
// asynchronous drainer.
size_t push_deterministic_tail(SdkPlayerListener& listener,
                               size_t burst_frames) {
    std::vector<int16_t> burst = ramp_pcm(burst_frames, 1'000'000);
    const size_t bytes = burst_frames * kFrameBytes;
    return listener.on_audio_write(reinterpret_cast<uint8_t*>(burst.data()),
                                   bytes, 1000);
}

// P4 + P2: connect the real SDK to a real loopback server, register the player
// role, pump PCM into the real engine, and observe the two stream-end causes.
int run_sdk_bridge_lifecycle(bool expect_gap_blocked) {
    // ---- scenario 1: connect, register, pump real PCM, natural end ----------
    constexpr uint16_t kPort1 = 19311;
    {
        BridgeRig rig;
        build_rig(rig, kPort1);
        connect_server(rig, kPort1, R"(["player@v1"])");

        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.bundle->client().is_connected() &&
                                       rig.bundle->client().is_time_synced(); },
                          4000);
        BRIDGE_CHECK(rig.bundle->client().is_connected(),
                     "P4: connected to the loopback server");
        BRIDGE_CHECK(rig.bundle->client().is_time_synced(),
                     "P4: time filter converged");

        BRIDGE_CHECK(rig.server->send_app_json(stream_start_pcm_json()),
                     "P4: stream/start accepted");
        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.session->streaming(); }, 3000);
        BRIDGE_CHECK(rig.session->streaming(),
                     "P4: on_stream_start opened a generation on the real engine");
        BRIDGE_CHECK(rig.listener->stream_starts() >= 1,
                     "P4: the real SdkPlayerListener observed the stream start");

        // Drive PCM through the SDK sync thread into the real engine, with a
        // driver playing it out so the SDK is not starved by back-pressure.
        rig.dac = std::make_unique<DacDriver>(rig.engine, rig.session->epoch(),
                                              rig.session->generation());
        rig.dac->start();
        feed_until_accepted(rig.bundle->client(), *rig.server, *rig.session,
                            *rig.listener, 4096);

        BRIDGE_CHECK(rig.session->accepted_frames() >= 4096,
                     "P4: the real engine accepted decoded PCM from the SDK sync thread");
        BRIDGE_CHECK(rig.dac->rendered_frames() > 0,
                     "P4: the engine rendered queued frames to the fake DAC");
        BRIDGE_CHECK(ramp_step_fraction(rig.dac->captured()) > 0.9,
                     "P4: rendered PCM is ordered (monotone ramp step preserved)");

        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.listener->report_count() > 0; }, 4000);
        BRIDGE_CHECK(rig.listener->report_count() > 0,
                     "P4: feedback reported from real engine timing");
        BRIDGE_CHECK(rig.listener->reported_frames() <= rig.dac->played(),
                     "P4: never reported more frames than the engine played");

        // ---- P2: natural stream/end ---------------------------------------
        // Deterministic barrier: stop AND join the fake DAC so nothing can drain
        // the queue, let the SDK sync thread go idle, then push a bounded burst
        // through the same real listener -> session -> engine path. The queued
        // tail is then exactly known (>= the burst), not a racy sample taken
        // while the asynchronous drainer may have already emptied the queue.
        rig.dac->stop();
        (void)wait_accepted_stable(*rig.session, 1000);
        constexpr size_t kBurstFrames = 4096;
        const size_t burst_acked =
            push_deterministic_tail(*rig.listener, kBurstFrames);
        BRIDGE_CHECK(burst_acked == kBurstFrames * kFrameBytes,
                     "P2: deterministic burst accepted by the real engine");
        const uint64_t queued_before = engine_queued(rig.engine);
        BRIDGE_CHECK(queued_before >= kBurstFrames,
                     "P2: a deterministic queued tail exists before stream/end");
        const bool connected_at_natural = rig.bundle->client().is_connected();

        BRIDGE_CHECK(rig.server->send_app_json(R"({"type":"stream/end"})"),
                     "P2: natural stream/end accepted");
        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.listener->stream_ends() >= 1; }, 3000);
        BRIDGE_CHECK(rig.listener->stream_ends() >= 1,
                     "P2: on_stream_end fired for the natural end");
        const uint64_t queued_after = engine_queued(rig.engine);

        std::fprintf(stderr,
                     "  [P2] natural-end: connected=%d queued_before=%llu "
                     "queued_after=%llu generation=%u\n",
                     (int)connected_at_natural,
                     (unsigned long long)queued_before,
                     (unsigned long long)queued_after, rig.session->generation());

        BRIDGE_CHECK(queued_after == queued_before,
                     "natural end preserves the queued tail through the typed SDK callback");
        if (expect_gap_blocked) {
            // Required behaviour: a NATURAL end must let the engine drain its
            // queued tail. The pinned RC1 callback cannot tell the two causes
            // apart, so it cancels and the tail is dropped: this is the pin.
            BLOCKED_CHECK(queued_after == queued_before,
                          "P2: natural stream/end must keep the queued tail for "
                          "the engine to drain (pinned RC1 on_stream_end() cannot "
                          "tell a natural end from a role removal, so it cancels)");
            BLOCKED_CHECK(connected_at_natural,
                          "P2: the client is still connected, yet the callback "
                          "still fenced (no public discriminator)");
            std::fprintf(stderr,
                         "  [P2] missing public interface (exact):\n"
                         "        PlayerRoleListener::on_stream_end() carries no "
                         "disposition, and\n"
                         "        SendspinClient exposes no is_role_active()/"
                         "get_active_roles().\n"
                         "        Required: on_stream_end(StreamEndReason{Natural,"
                         "RoleRemoved,Disconnected})\n"
                         "        or SendspinClient::is_role_active(SendspinRole::"
                         "PLAYER).\n"
                         "        Pinned player_role.cpp: drain_events() STREAM_END "
                         "fires for both\n"
                         "        handle_stream_end() (natural) and cleanup() "
                         "(role removal/teardown).\n");
        }
    }

    // ---- scenario 2: a role removal must fence and drop ---------------------
    constexpr uint16_t kPort2 = 19312;
    {
        BridgeRig rig;
        build_rig(rig, kPort2);
        connect_server(rig, kPort2, R"(["player@v1"])");
        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.bundle->client().is_connected() &&
                                       rig.bundle->client().is_time_synced(); },
                          4000);

        BRIDGE_CHECK(rig.server->send_app_json(stream_start_pcm_json()),
                     "P2b: stream/start accepted");
        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.session->streaming(); }, 3000);
        BRIDGE_CHECK(rig.session->streaming(), "P2b: streaming");

        rig.dac = std::make_unique<DacDriver>(rig.engine, rig.session->epoch(),
                                              rig.session->generation());
        rig.dac->start();
        feed_until_accepted(rig.bundle->client(), *rig.server, *rig.session,
                            *rig.listener, 2048);
        // Deterministic barrier: stop+join the drainer, let the SDK sync thread
        // idle, then push a bounded burst so the queued tail is exactly known.
        rig.dac->stop();
        (void)wait_accepted_stable(*rig.session, 1000);
        constexpr size_t kBurstFrames = 4096;
        const size_t burst_acked =
            push_deterministic_tail(*rig.listener, kBurstFrames);
        BRIDGE_CHECK(burst_acked == kBurstFrames * kFrameBytes,
                     "P2b: deterministic burst accepted by the real engine");
        const uint64_t queued_before = engine_queued(rig.engine);
        BRIDGE_CHECK(queued_before >= kBurstFrames,
                     "P2b: a deterministic queued tail exists before removal");

        BRIDGE_CHECK(rig.server->send_app_json(activate_without_player_json()),
                     "P2b: server/activate removing the player accepted");
        pump_bridge_until(rig.bundle->client(), *rig.listener,
                          [&] { return rig.listener->stream_ends() >= 1; }, 3000);
        BRIDGE_CHECK(rig.listener->stream_ends() >= 1,
                     "P2b: on_stream_end fired for the role removal");
        BRIDGE_CHECK(!rig.session->streaming(),
                     "P2b: the role removal fenced the live generation");
        // Cancellation must suppress stale frames: the fenced session accepts
        // nothing until a strictly higher start_stream().
        const size_t stale = push_deterministic_tail(*rig.listener, 256);
        BRIDGE_CHECK(stale == 0,
                     "P2b: cancellation suppresses stale frames (write returns 0)");
        const uint64_t queued_after = engine_queued(rig.engine);
        std::fprintf(stderr,
                     "  [P2b] role-removal: queued_before=%llu queued_after=%llu\n",
                     (unsigned long long)queued_before,
                     (unsigned long long)queued_after);
        BRIDGE_CHECK(queued_after < queued_before,
                     "P2b: the role removal dropped the queued tail (correct)");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool blocked_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--expect-disposition-blocked") {
            blocked_mode = true;
        }
    }
    std::fprintf(stderr,
                 "test_sendspin_bridge_sdk: real SDK <-> real engine bridge\n");
    test_feedback_pair_is_the_played_cursor_finish();
    test_natural_tail_is_reported_exactly_once();
    test_timing_arithmetic_bounds();
    run_sdk_bridge_lifecycle(blocked_mode);

    if (g_failures != 0) {
        std::fprintf(stderr, "test_sendspin_bridge_sdk: %d check(s) FAILED\n",
                     g_failures);
        return 1;
    }
    if (g_blocked != 0) {
        std::fprintf(stderr,
                     "test_sendspin_bridge_sdk: %d check(s) BLOCKED by a "
                     "documented SDK interface gap (see [P2] above)\n",
                     g_blocked);
        return 2;
    }
    std::fprintf(stderr, "test_sendspin_bridge_sdk: all checks passed\n");
    return 0;
}
