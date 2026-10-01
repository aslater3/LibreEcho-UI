// SPDX-License-Identifier: MIT
//
// LibreEcho Sendspin adapter — Task 2 SDK feasibility fixture.
//
// This is a standalone (no GoogleTest) host fixture that drives the *real* pinned
// sendspin-cpp RC1 SDK through its real player-role callbacks against the SDK's own
// real, strict, encrypted loopback server fixture (Noise KKpsk2 over a real
// WebSocket). It exists to answer the feasibility questions the implementation plan
// calls out for Task 2:
//
//   1. on_audio_write() must return BYTES, always a whole number of PCM frames, and
//      the sync task must consume that byte count and re-issue any unaccepted tail.
//   2. The sink must be able to report notify_audio_played() with a *future* DAC
//      finish timestamp (never "enqueue time"), and the role must keep advancing.
//   3. on_audio_write() runs on the SDK sync thread; the other player callbacks run
//      on the main-loop thread.
//   4. The SDK must leave initial synchronization and deliver decoded PCM (not just
//      priming/underflow silence) — the fixture checks the payload bytes, not only a
//      callback counter.
//
// It is deliberately PCM 48 kHz / stereo / S16 only; Opus and the non-player roles
// are compiled out (see CMakeLists.txt).

#include "lifecycle_test_fixtures.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

namespace sendspin {
namespace {

// ---------------------------------------------------------------------------
// Tiny check harness (no gtest dependency, matching the dependency-free UI build)
// ---------------------------------------------------------------------------
int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// The PCM geometry this spike advertises: 48 kHz / 2 ch / S16 = 4 bytes/frame.
// ---------------------------------------------------------------------------
constexpr uint32_t kSampleRate = 48000;
constexpr size_t kBytesPerFrame = 4;                              // 2 ch * 16-bit
constexpr size_t kChunk20ms = kSampleRate / 50 * kBytesPerFrame;  // 3840 bytes

// A constant, non-zero PCM payload so the sink can tell decoded programme audio
// apart from the SDK's priming/underflow silence (which is all zeros). 0x11,0x22
// is a valid non-zero little-endian S16 sample and survives fine sync correction.
constexpr uint8_t kPatternA = 0x11;
constexpr uint8_t kPatternB = 0x22;

// ---------------------------------------------------------------------------
// A sink listener that models a real DAC: it returns accepted BYTES, blocks the SDK
// sync thread with real-time backpressure the way a hardware sink does, reports
// playback with a future finish timestamp, and records the evidence we assert on.
// ---------------------------------------------------------------------------
struct DacSinkListener : public PlayerRoleListener {
    // The role to feed feedback into. Set before the stream starts.
    PlayerRole* role = nullptr;

    // Sink policy: accept only half of the offered bytes (frame-aligned) for the
    // first N calls, to prove the SDK honours a partial byte return and re-issues
    // the remainder. 0 disables partial acceptance.
    int partial_writes_remaining = 0;

    std::atomic<size_t> calls{0};
    std::atomic<size_t> offered_bytes{0};
    std::atomic<size_t> accepted_bytes{0};
    std::atomic<size_t> nonframe_lengths{0};   // SDK offered a non-frame-aligned length
    std::atomic<size_t> nonframe_returns{0};   // our own return was not frame-aligned
    std::atomic<size_t> nonzero_payload_writes{0};
    std::atomic<size_t> notify_calls{0};
    std::atomic<uint64_t> notified_frames{0};
    std::atomic<int64_t> min_programme_lead_us{0};
    std::atomic<bool> finish_not_increasing{false};

    std::thread::id write_tid{};
    std::thread::id start_tid{};
    std::thread::id end_tid{};
    std::atomic<size_t> stream_starts{0};
    std::atomic<size_t> stream_ends{0};

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override {
        if (this->calls.load() == 0) {
            this->write_tid = std::this_thread::get_id();
        }
        this->offered_bytes.fetch_add(length);
        if ((length % kBytesPerFrame) != 0) {
            this->nonframe_lengths.fetch_add(1);
        }

        // Model a hardware DAC consuming at 48 kHz. The sink — not the SDK — bounds
        // the rate, exactly as the real PortAudio/ALSA sink would.
        if (this->dac_epoch_us_ == 0) {
            this->dac_epoch_us_ = platform_time_us();
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(std::max<uint32_t>(timeout_ms, 100));
        for (;;) {
            const int64_t elapsed_frames =
                (platform_time_us() - this->dac_epoch_us_) * static_cast<int64_t>(kSampleRate) /
                1000000;
            if (this->submitted_frames_.load() - elapsed_frames <= kMaxLeadFrames) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        size_t accept = length;
        if (this->partial_writes_remaining > 0 && length > kBytesPerFrame) {
            accept = (length / 2) - ((length / 2) % kBytesPerFrame);
            if (accept == 0) {
                accept = kBytesPerFrame;
            }
            this->partial_writes_remaining--;
        }
        if ((accept % kBytesPerFrame) != 0) {
            this->nonframe_returns.fetch_add(1);
        }

        bool nonzero = false;
        for (size_t i = 0; i < accept; ++i) {
            if (data[i] != 0) {
                nonzero = true;
                break;
            }
        }
        if (nonzero) {
            this->nonzero_payload_writes.fetch_add(1);
        }

        this->calls.fetch_add(1);
        this->accepted_bytes.fetch_add(accept);
        const size_t frames = accept / kBytesPerFrame;

        // Report the frames as played with a *future* DAC finish timestamp, the way
        // the reference PortAudio sink does. Never report enqueue time.
        if (this->role != nullptr) {
            const int64_t submitted =
                this->submitted_frames_.fetch_add(static_cast<int64_t>(frames)) +
                static_cast<int64_t>(frames);
            const int64_t finish_us =
                this->dac_epoch_us_ + submitted * 1000000LL / static_cast<int64_t>(kSampleRate);
            const int64_t now_us = platform_time_us();
            const int64_t prev = this->last_finish_us_.load();
            if (prev != 0 && finish_us < prev) {
                this->finish_not_increasing.store(true);
            }
            this->last_finish_us_.store(finish_us);
            if (nonzero) {
                const int64_t lead = finish_us - now_us;
                const int64_t observed = this->min_programme_lead_us.load();
                if (observed == 0 || lead < observed) {
                    this->min_programme_lead_us.store(lead);
                }
            }
            this->role->notify_audio_played(static_cast<uint32_t>(frames), finish_us);
            this->notify_calls.fetch_add(1);
            this->notified_frames.fetch_add(frames);
        }
        return accept;
    }

    void on_stream_start() override {
        this->start_tid = std::this_thread::get_id();
        this->stream_starts.fetch_add(1);
    }

    void on_stream_end() override {
        this->end_tid = std::this_thread::get_id();
        this->stream_ends.fetch_add(1);
    }

private:
    // Allow the DAC to run up to ~85 ms (two periods) ahead of real time.
    static constexpr int64_t kMaxLeadFrames = 4096;
    int64_t dac_epoch_us_ = 0;
    std::atomic<int64_t> submitted_frames_{0};
    std::atomic<int64_t> last_finish_us_{0};
};

// ---------------------------------------------------------------------------
// A strict, encrypted server fixture with a non-zero PCM payload. It is the SDK's
// own FakeEncryptedServer (real Noise KKpsk2 over a real WebSocket) plus a helper
// that lays out a known PCM pattern in place of the fixture's default zeros.
// ---------------------------------------------------------------------------
class PatternServer : public FakeEncryptedServer {
public:
    using FakeEncryptedServer::FakeEncryptedServer;

    bool send_pcm(int64_t timestamp_us, size_t payload_bytes, uint32_t send_ahead_us = 0) {
        std::string body;
        body.reserve(4 + payload_bytes);
        for (int shift = 24; shift >= 0; shift -= 8) {
            body.push_back(static_cast<char>((send_ahead_us >> shift) & 0xFF));
        }
        for (size_t i = 0; i < payload_bytes; i += 2) {
            body.push_back(static_cast<char>(kPatternA));
            if (i + 1 < payload_bytes) {
                body.push_back(static_cast<char>(kPatternB));
            }
        }
        return this->send_binary(4, timestamp_us, body);
    }
};

std::unique_ptr<PatternServer> connect_pattern_server(const PairedPeer& peer, uint16_t port,
                                                      FakeEncryptedServerOptions options) {
    return std::make_unique<PatternServer>(server_url(port), std::string(NOISE_SUITE_CHACHAPOLY),
                                           peer.server_identity, peer.record.psk_id, peer.psk,
                                           std::move(options));
}

SendspinClientConfig make_test_config(uint16_t port) {
    SendspinClientConfig config;
    config.name = "LibreEcho Sendspin SDK Spike";
    config.server_port = port;
    config.time_burst_interval_ms = 100;
    return config;
}

// Feeds real-time paced 20 ms PCM chunks until `target` decoded (non-silence)
// writes have been seen, or a bounded wall-clock deadline expires.
void feed_programme_audio(SendspinClient& client, PatternServer& server, DacSinkListener& sink,
                          size_t target) {
    int64_t ts = platform_time_us() + 100 * 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (sink.nonzero_payload_writes.load() < target &&
           std::chrono::steady_clock::now() < deadline) {
        server.send_pcm(ts, kChunk20ms);
        ts += 20 * 1000;
        pump_for(client, 20);
    }
}

// ---------------------------------------------------------------------------
// Test 1 — real decoded playback with future-DAC feedback.
// ---------------------------------------------------------------------------
int run_playback_with_feedback() {
    constexpr uint16_t PORT = 19201;
    constexpr size_t kTargetRealWrites = 10;

    DacSinkListener sink;
    PairedClientBundle bundle(make_test_config(PORT));
    SendspinClient& client = bundle.client();
    PlayerRole& player = client.add_player(make_pcm_player_config());
    player.set_listener(&sink);
    sink.role = &player;

    const std::thread::id main_tid = std::this_thread::get_id();
    CHECK(bundle.start(), "client start()");

    FakeEncryptedServerOptions options;
    options.answer_time = true;
    options.first_roles_json = R"(["player@v1"])";
    auto server = connect_pattern_server(bundle.peer, PORT, std::move(options));
    pump_until_synced(client);
    CHECK(client.is_connected(), "client is connected to the strict encrypted server");
    CHECK(client.is_time_synced(), "time filter converged (left initial synchronization)");

    CHECK(server->send_app_json(stream_start_pcm_json()), "stream/start accepted");
    pump_until(client, [&] { return sink.stream_starts.load() >= 1; });
    CHECK(sink.stream_starts.load() >= 1, "on_stream_start fired");
    CHECK(sink.start_tid == main_tid, "on_stream_start ran on the main-loop thread");

    feed_programme_audio(client, *server, sink, kTargetRealWrites);
    CHECK(sink.nonzero_payload_writes.load() >= kTargetRealWrites,
          "SDK delivered decoded non-silence PCM beyond initial synchronization");

    // Stop the session so the counters are frozen before we read them.
    client.disconnect(SendspinGoodbyeReason::SHUTDOWN);
    pump_for(client, 400);
    CHECK(sink.stream_ends.load() >= 1, "on_stream_end fired after teardown");
    CHECK(sink.end_tid == main_tid, "on_stream_end ran on the main-loop thread");

    CHECK(sink.calls.load() > 0, "on_audio_write fired");
    CHECK(sink.write_tid != main_tid, "on_audio_write ran on the SDK sync thread, not main loop");
    CHECK(sink.nonframe_lengths.load() == 0, "SDK never offered a non-frame-aligned length");
    CHECK(sink.nonframe_returns.load() == 0, "sink return stayed whole-frame aligned");
    CHECK((sink.accepted_bytes.load() % kBytesPerFrame) == 0,
          "total accepted bytes are a whole number of frames");
    CHECK(sink.notify_calls.load() == sink.calls.load(),
          "one notify_audio_played per accepted on_audio_write");
    CHECK(sink.notified_frames.load() * kBytesPerFrame == sink.accepted_bytes.load(),
          "byte return -> frame accounting is exact (frames * 4 == accepted bytes)");
    CHECK(!sink.finish_not_increasing.load(), "DAC finish timestamps never go backwards");
    CHECK(sink.min_programme_lead_us.load() > 0,
          "every reported programme DAC finish timestamp was in the future");

    std::fprintf(stderr,
                 "  [playback] calls=%zu real_writes=%zu accepted_bytes=%zu notify=%zu frames=%llu "
                 "min_lead_us=%lld\n",
                 sink.calls.load(), sink.nonzero_payload_writes.load(), sink.accepted_bytes.load(),
                 sink.notify_calls.load(),
                 static_cast<unsigned long long>(sink.notified_frames.load()),
                 static_cast<long long>(sink.min_programme_lead_us.load()));
    return 0;
}

// ---------------------------------------------------------------------------
// Test 2 — the byte-return contract with partial, frame-aligned acceptance.
// ---------------------------------------------------------------------------
int run_partial_byte_accept() {
    constexpr uint16_t PORT = 19202;
    constexpr size_t kTargetRealWrites = 6;

    DacSinkListener sink;
    sink.partial_writes_remaining = 3;  // accept half the offered bytes for the first 3 calls

    PairedClientBundle bundle(make_test_config(PORT));
    SendspinClient& client = bundle.client();
    PlayerRole& player = client.add_player(make_pcm_player_config());
    player.set_listener(&sink);
    sink.role = &player;

    CHECK(bundle.start(), "client start()");

    FakeEncryptedServerOptions options;
    options.answer_time = true;
    options.first_roles_json = R"(["player@v1"])";
    auto server = connect_pattern_server(bundle.peer, PORT, std::move(options));
    pump_until_synced(client);
    CHECK(server->send_app_json(stream_start_pcm_json()), "stream/start accepted");
    pump_until(client, [&] { return sink.stream_starts.load() >= 1; });

    feed_programme_audio(client, *server, sink, kTargetRealWrites);
    CHECK(sink.nonzero_payload_writes.load() >= kTargetRealWrites,
          "playback continued after partial byte acceptance");

    client.disconnect(SendspinGoodbyeReason::SHUTDOWN);
    pump_for(client, 400);

    CHECK(sink.nonframe_returns.load() == 0, "partial returns stayed frame-aligned");
    CHECK(sink.offered_bytes.load() > sink.accepted_bytes.load(),
          "SDK re-issued bytes the sink did not accept (byte, not frame, return semantics)");
    CHECK(sink.notified_frames.load() * kBytesPerFrame == sink.accepted_bytes.load(),
          "frame accounting still exact across partial writes");

    std::fprintf(stderr, "  [partial] offered=%zu accepted=%zu real_writes=%zu\n",
                 sink.offered_bytes.load(), sink.accepted_bytes.load(),
                 sink.nonzero_payload_writes.load());
    return 0;
}

}  // namespace
}  // namespace sendspin

int main() {
    using namespace sendspin;
    std::fprintf(stderr, "sendspin_adapter_tests: real pinned RC1 SDK player fixture\n");
    run_playback_with_feedback();
    run_partial_byte_accept();

    if (g_failures != 0) {
        std::fprintf(stderr, "sendspin_adapter_tests: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "sendspin_adapter_tests: all checks passed\n");
    return 0;
}
