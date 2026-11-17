/*
 * EngineSinkSession — see engine_sink_session.h. Bounded, generation-fenced.
 *
 * Thread safety: every public op takes `mu_` and holds it for the whole op,
 * including the EngineSink call it drives, so the SDK's two threads (sync-task
 * on_audio_write, main-loop lifecycle/feedback) can never observe or produce a
 * half-applied transition. Lock order is always mu_ -> sink_.mu.
 */

#include "engine_sink_session.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace libreecho {
namespace sendspin {
namespace {

// Wall-clock bound on one loss: a re-arm whose OPEN is still in progress (the
// engine opening its PCM) stops counting attempts after this long.
constexpr int64_t kRearmDeadlineUs = 10 * 1000 * 1000;

int64_t session_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Stale-generation probing: at most kStaleProbeMax extra OPENs, steps doubling
// up to kStaleProbeMaxStep (about one million generations of reach).
constexpr uint32_t kStaleProbeMax = 32;
constexpr uint32_t kStaleProbeMaxStep = 1u << 16;

bool stale_refusal(SinkResult r, uint32_t err) {
    return r == SinkResult::Rejected && err == LE_AUDIO_SINK_ERR_STALE_GENERATION;
}

}  // namespace

EngineSinkSession::EngineSinkSession(EngineSinkSessionConfig config)
    : config_(std::move(config)),
      next_generation_(config_.first_generation == 0 ? 1u
                                                     : config_.first_generation) {}

SinkResult EngineSinkSession::connect_locked() {
    if (sink_.connected()) {
        if (state_ == SessionState::Disconnected) state_ = SessionState::Connected;
        return SinkResult::Ok;
    }
    const uint32_t timeout =
        clamp(config_.connect_timeout_ms, kSessionMaxOpTimeoutMs * 20);
    SinkResult r = sink_.connect(config_.socket_path, timeout);
    state_ = (r == SinkResult::Ok) ? SessionState::Connected
                                   : SessionState::Disconnected;
    return r;
}

SinkResult EngineSinkSession::connect() {
    std::lock_guard<std::mutex> guard(mu_);
    return connect_locked();
}

SinkResult EngineSinkSession::start_stream() {
    std::lock_guard<std::mutex> guard(mu_);
    SinkResult r = start_stream_locked();
    if (r == SinkResult::Ok) {
        rearm_attempts_ = 0;
        rearm_failures_ = 0;
    }
    return r;
}

SinkResult EngineSinkSession::start_stream_locked() {
    SinkResult r = connect_locked();
    if (r != SinkResult::Ok) return r;

    const uint32_t timeout = clamp(config_.control_timeout_ms,
                                   kSessionMaxOpTimeoutMs);
    // Allocate the next strictly-increasing generation. next_generation_ is
    // never reset downward, so a reconnect to a surviving engine cannot reuse a
    // generation the engine has already seen.
    uint32_t attempt = next_generation_;
    r = sink_.open(attempt, timeout);
    // A reconnect after the engine restarted can leave a stale fd: rebuild the
    // socket once and retry the same (still-unused) generation.
    if (r == SinkResult::Closed || r == SinkResult::Io ||
        r == SinkResult::NotConnected) {
        sink_.disconnect();
        state_ = SessionState::Disconnected;
        r = connect_locked();
        if (r == SinkResult::Ok) r = sink_.open(attempt, timeout);
    }
    // The engine outlives sendspind, so it may have seen generations far above
    // ours. Probe upward with doubling steps (bounded) until it accepts. Only a
    // STALE_GENERATION refusal keeps probing; a Timeout leaves the OPEN in
    // flight for a resuming retry and must not be read as stale.
    uint32_t probes = 0;
    uint32_t step = 1;
    while (stale_refusal(r, sink_.last_error()) && probes < kStaleProbeMax &&
           UINT32_MAX - attempt > step) {
        attempt += step;
        if (step < kStaleProbeMaxStep) step *= 2u;
        ++probes;
        r = sink_.open(attempt, timeout);
    }
    if (r != SinkResult::Ok) {
        state_ = SessionState::Connected;
        // Never lose ground: every generation up to `attempt` was refused as
        // stale, so continue above it. Any other outcome (Timeout in flight,
        // another error) retries `attempt` itself.
        if (stale_refusal(r, sink_.last_error()))
            next_generation_ = attempt == UINT32_MAX ? attempt : attempt + 1u;
        else
            next_generation_ = attempt;
        return r;
    }
    if (probes > 0)
        std::fprintf(stderr,
                     "sendspind: engine had seen newer generations; opened generation %u after %u probe(s)\n",
                     static_cast<unsigned>(attempt), static_cast<unsigned>(probes));
    next_generation_ = attempt == UINT32_MAX ? attempt : attempt + 1u;
    state_ = SessionState::Streaming;
    return SinkResult::Ok;
}

SinkResult EngineSinkSession::stop_stream(bool* completed_out) {
    std::lock_guard<std::mutex> guard(mu_);
    if (state_ == SessionState::Lost) {
        // The engine already dropped the generation: there is nothing to FINISH
        // and the stream ending means no re-arm is wanted.
        state_ = SessionState::Connected;
        return SinkResult::NotOpen;
    }
    if (!sink_.open_generation()) {
        return SinkResult::NotOpen;
    }
    // Natural end stops local offers immediately, even if FINISH_ACK is late.
    // The sink remains readable/retryable until its bounded control completes.
    state_ = SessionState::Fenced;
    const uint64_t total = sink_.accepted_frames();
    const uint32_t timeout = clamp(config_.control_timeout_ms,
                                   kSessionMaxOpTimeoutMs);
    SinkResult r = sink_.finish(total, timeout, completed_out);
    if (r == SinkResult::Ok) {
        state_ = SessionState::Fenced;
    } else if (r == SinkResult::NotConnected || r == SinkResult::Closed) {
        state_ = SessionState::Disconnected;
    }
    // A late ACK leaves the sink retryable, but local writes stay fenced.
    // The listener retries bounded FINISH operations from its feedback loop.
    return r;
}

SinkResult EngineSinkSession::cancel_stream() {
    std::lock_guard<std::mutex> guard(mu_);
    if (state_ == SessionState::Lost) {
        // The dropped generation is already gone on the engine; just fence the
        // session so no re-arm happens for the ended/cancelled stream.
        state_ = SessionState::Fenced;
        return SinkResult::Ok;
    }
    const uint32_t timeout = clamp(config_.control_timeout_ms,
                                   kSessionMaxOpTimeoutMs);
    SinkResult r = sink_.cancel(timeout);
    if (r == SinkResult::Ok) {
        state_ = SessionState::Fenced;
    } else if (r == SinkResult::NotConnected || r == SinkResult::Closed) {
        state_ = SessionState::Disconnected;
    }
    return r;
}

void EngineSinkSession::disconnect() {
    std::lock_guard<std::mutex> guard(mu_);
    sink_.disconnect();
    state_ = SessionState::Disconnected;
    rearm_attempts_ = 0;
    rearm_failures_ = 0;
}

void EngineSinkSession::begin_loss_locked() {
    state_ = SessionState::Lost;
    rearm_attempts_ = 0;
    rearm_failures_ = 0;
    lost_since_us_ = session_now_us();
}

void EngineSinkSession::note_loss_locked(const char* cause) {
    if (state_ != SessionState::Streaming) return;
    begin_loss_locked();
    std::fprintf(stderr,
                 "sendspind: engine dropped stream generation %u (%s, err=%u); re-arming\n",
                 static_cast<unsigned>(sink_.generation()), cause,
                 static_cast<unsigned>(sink_.last_error()));
}

void EngineSinkSession::request_rearm(const char* cause) {
    std::lock_guard<std::mutex> guard(mu_);
    begin_loss_locked();
    std::fprintf(stderr, "sendspind: no live stream generation (%s); re-arming\n", cause);
}

size_t EngineSinkSession::write(const uint8_t* pcm, size_t length,
                                uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (state_ != SessionState::Streaming) {
        // Not armed: accept nothing (and send nothing).
        return 0;
    }
    const size_t accepted = sink_.on_audio_write(
        pcm, length, clamp(timeout_ms, config_.max_write_timeout_ms));
    if (sink_.stream_lost()) note_loss_locked("write");
    // Safety net: a Streaming session never outlives its armed, connected sink.
    else if (!sink_.connected() || !sink_.open_generation()) note_loss_locked("sink closed");
    return accepted;
}

SinkResult EngineSinkSession::poll(EngineSinkTiming* out) {
    std::lock_guard<std::mutex> guard(mu_);
    const uint32_t timeout = clamp(config_.control_timeout_ms,
                                   kSessionMaxOpTimeoutMs);
    const SinkResult r = sink_.progress(out, timeout);
    if (sink_.stream_lost()) note_loss_locked("progress");
    else if (!sink_.connected() || !sink_.open_generation()) note_loss_locked("sink closed");
    return r;
}

bool EngineSinkSession::rearm_budget_left_locked() const {
    return rearm_failures_ < kRearmMaxFailures &&
           session_now_us() - lost_since_us_ <= kRearmDeadlineUs;
}

bool EngineSinkSession::rearm_pending() const {
    std::lock_guard<std::mutex> guard(mu_);
    return state_ == SessionState::Lost && rearm_budget_left_locked();
}

SinkResult EngineSinkSession::rearm_stream() {
    std::lock_guard<std::mutex> guard(mu_);
    if (state_ != SessionState::Lost) return SinkResult::NotOpen;
    if (!rearm_budget_left_locked()) {
        if (rearm_failures_ < kRearmMaxFailures) {
            rearm_failures_ = kRearmMaxFailures;
            std::fprintf(stderr,
                         "sendspind: engine re-arm timed out after %u attempt(s); waiting for the next stream start\n",
                         static_cast<unsigned>(rearm_attempts_));
        }
        return SinkResult::NotOpen;
    }
    ++rearm_attempts_;
    const SinkResult r = start_stream_locked();
    if (r == SinkResult::Ok) {
        rearm_failures_ = 0;
        std::fprintf(stderr,
                     "sendspind: re-armed engine generation %u after %u attempt(s)\n",
                     static_cast<unsigned>(sink_.generation()),
                     static_cast<unsigned>(rearm_attempts_));
        return r;
    }
    // start_stream_locked() leaves Connected/Disconnected on failure; the stream
    // is still open on the SDK side, so it stays Lost until the budget is spent.
    state_ = SessionState::Lost;
    // A timed-out OPEN is still in flight on the engine and resumes next call;
    // only a refusal or a dead socket spends the budget.
    if (r != SinkResult::Timeout) {
        ++rearm_failures_;
        if (rearm_failures_ >= kRearmMaxFailures) {
            std::fprintf(stderr,
                         "sendspind: engine re-arm failed %u time(s) (last=%d, err=%u); waiting for the next stream start\n",
                         static_cast<unsigned>(rearm_failures_), static_cast<int>(r),
                         static_cast<unsigned>(sink_.last_error()));
        }
    }
    return r;
}

}  // namespace sendspin
}  // namespace libreecho
