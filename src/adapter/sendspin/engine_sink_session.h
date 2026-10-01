/*
 * LE_AUDIO_SINK/1 companion bridge — EngineSinkSession.
 *
 * Task 5 core: the object that owns one EngineSink and turns the Sendspin
 * player-role callbacks into a bounded, generation-fenced engine session. It has
 * NO Sendspin SDK dependency, so it is exercised directly against the real
 * compiled C99 engine (the Platform airplay audio_sink.c) by the opt-in
 * `make test-sendspin-sink` lane; the SDK PlayerRoleListener binding
 * (sdk_player_listener.h) is a thin adapter on top of this.
 *
 * Responsibilities:
 *
 *  * Generation ownership. The session owns a strictly increasing generation
 *    counter that persists across reconnects to the same surviving engine. It
 *    is never reset downward: the engine retains its `last_generation` across a
 *    peer disconnect (audio_sink.c), so reconnecting and re-opening at 1 would
 *    be refused. On a true engine restart (fresh epoch) a higher generation is
 *    still valid, so monotonic increase is correct for both. A brand-new
 *    companion process that inherits a long-lived engine must therefore start
 *    above the previous generation; that persisted value is a later
 *    (pairing/persistence) task, not invented here.
 *
 *  * Thread safety. The SDK contract runs on_audio_write() on the sync-task
 *    thread and the lifecycle callbacks (on_stream_start/on_stream_end) plus the
 *    feedback pump on the main loop, so this session is genuinely used by two
 *    threads. `mu_` serializes ALL session state (state_, next_generation_) AND
 *    is held across the EngineSink call that each transition drives, so a
 *    control op and the in-flight write can never interleave half-way through a
 *    transition. EngineSink's own mutex is nested inside `mu_`; the lock order is
 *    therefore always mu_ -> sink_.mu and there is no reverse path, so the two
 *    threads cannot deadlock. Each op is deadline-bounded (see below), so a
 *    control op waits at most one in-flight write (<= max_write_timeout_ms).
 *
 *  * Bounded control dispatch. Every public op is deadline-bounded and the
 *    per-op timeout is clamped to a small ceiling (<= 50 ms, and control ops
 *    default to one 20 ms engine period) so a main-loop control op can wait at
 *    most that behind an in-flight sync-thread write. No command queue, no
 *    per-request thread.
 *
 *  * Byte-return semantics. write() returns exactly the engine-acknowledged
 *    bytes (whole frames, <= offered); the SDK re-issues any unaccepted tail.
 *
 *  * Feedback source of truth. poll() returns the engine's own timing snapshot;
 *    the SDK binding derives the played-cursor finish time from the engine's own
 *    cursors and never fabricates a DAC timestamp.
 *
 * This header is C++20 and is built only by the companion's own opt-in target.
 */

#ifndef LIBREECHO_SENDPIN_ENGINE_SINK_SESSION_H
#define LIBREECHO_SENDPIN_ENGINE_SINK_SESSION_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "engine_sink.h"

namespace libreecho {
namespace sendspin {

// Hard ceiling on any single EngineSink operation issued by the session, so a
// main-loop control call cannot block on the sync thread for longer than this.
inline constexpr uint32_t kSessionMaxOpTimeoutMs = 50;

struct EngineSinkSessionConfig {
    std::string socket_path;
    uint32_t connect_timeout_ms = 1000;
    // Per-op timeout for control ops (open/finish/cancel/progress).
    uint32_t control_timeout_ms = 20;
    // Ceiling the per-write timeout passed in by the SDK is clamped to.
    uint32_t max_write_timeout_ms = 20;
    // First generation this session will open. Kept for explicit, printable
    // configuration; the session never allocates a generation below it.
    uint32_t first_generation = 1;
};

// What the last stream-start attempt did, for truthful startup readiness.
enum class SessionState {
    Disconnected,
    Connected,
    Streaming,
    Fenced,
};

class EngineSinkSession {
public:
    EngineSinkSession() = default;
    explicit EngineSinkSession(EngineSinkSessionConfig config);

    EngineSinkSession(const EngineSinkSession&) = delete;
    EngineSinkSession& operator=(const EngineSinkSession&) = delete;

    // Connect (bounded, non-blocking, peer-credential validated) if not already
    // connected. Idempotent while connected.
    SinkResult connect();

    // Ensure connected, then OPEN the next strictly-increasing generation.
    // Returns NotOpen-with-last_result on failure; on success the session is
    // Streaming and accepting audio.
    SinkResult start_stream();

    // Natural end of the stream: declare the exact cumulative total and await
    // FINISH_ACK. The engine reports completion only from its physical
    // playhead. Returns NotOpen when there is no live generation.
    SinkResult stop_stream(bool* completed_out = nullptr);

    // Seek-clear / role removal / supersession: fence the live generation. No
    // further audio is accepted until a strictly higher start_stream().
    SinkResult cancel_stream();

    // Fence and close the socket (disconnect / teardown).
    void disconnect();

    // Sync-thread entry point. Bounded, frame-aligned byte return; 0 on any
    // non-accepting outcome. Never invents bytes.
    size_t write(const uint8_t* pcm, size_t length, uint32_t timeout_ms);

    // Main-loop progress poll for the live/opened generation.
    SinkResult poll(EngineSinkTiming* out);

    SessionState state() const {
        std::lock_guard<std::mutex> guard(mu_);
        return state_;
    }
    uint32_t generation() const { return sink_.generation(); }
    uint32_t epoch() const { return sink_.epoch(); }
    uint64_t accepted_frames() const { return sink_.accepted_frames(); }
    bool streaming() const {
        std::lock_guard<std::mutex> guard(mu_);
        return state_ == SessionState::Streaming;
    }
    SinkResult last_result() const { return sink_.last_result(); }
    uint32_t last_error() const { return sink_.last_error(); }

    // The raw sink. EngineSink is internally synchronized, but callers that
    // mutate the session lifecycle (open/finish/cancel) must go through the
    // session methods above so `state_` stays consistent. Prefer this only for
    // read-only inspection.
    EngineSink& sink() { return sink_; }
    const EngineSinkSessionConfig& config() const { return config_; }

private:
    uint32_t clamp(uint32_t ms, uint32_t ceiling) const {
        return ms == 0 || ms > ceiling ? ceiling : ms;
    }

    // connect() without taking mu_ (callers already hold it).
    SinkResult connect_locked();

    EngineSinkSessionConfig config_{};
    EngineSink sink_{};
    // Guards state_ and next_generation_, and is held across the sink_ op each
    // transition drives. Lock order: mu_ -> sink_.mu. See the file header.
    mutable std::mutex mu_;
    // Monotonic; never reset downward. See the file header.
    uint32_t next_generation_ = 1;
    SessionState state_ = SessionState::Disconnected;
};

}  // namespace sendspin
}  // namespace libreecho

#endif  // LIBREECHO_SENDPIN_ENGINE_SINK_SESSION_H
