/*
 * LE_AUDIO_SINK/1 companion bridge — pinned RC1 Sendspin SDK binding.
 *
 * SdkPlayerListener implements the SDK's PlayerRoleListener and forwards each
 * callback into an EngineSinkSession. It is the thin, testable layer named by
 * the landed UI AGENTS.md C++20 companion exception:
 *
 *   on_audio_write (SDK sync thread)  -> session.write()  (acked BYTES)
 *   on_stream_start / on_stream_end   -> session.start_stream() / session control
 *   main-loop feedback pump           -> session.poll() -> role->notify_audio_played()
 *
 * Threading mirrors the SDK contract exactly: on_audio_write() runs on the sync
 * task's background thread; the start/end callbacks and the feedback pump run on
 * the main loop. EngineSinkSession serializes the two with a bounded per-op
 * timeout, so the main loop is never blocked for more than one short op.
 *
 * notify_audio_played() contract (derived from the pinned source + protocol):
 *
 *   The SDK's sync task (sync_task.cpp process_playback_progress()) does
 *
 *       new_audio_client_playtime = finish_timestamp + frames_to_us(buffered_frames)
 *
 *   where buffered_frames is (frames this sink ACCEPTED - frames it reported
 *   PLAYED). So the timestamp passed for a reported played count must be the
 *   finish time of exactly those played frames, or the SDK double-counts the
 *   still-buffered audio ("double finish-delay").
 *
 *   The engine's PROGRESS reply publishes:
 *     * played_frames     — hardware-played source cursor (what we report);
 *     * submitted_frames  — cumulative frames handed to the hardware timeline;
 *     * finish_us         — estimated finish of the last SUBMITTED source frame.
 *
 *   finish_us is the end of the whole submitted tail, NOT of the played cursor.
 *   The played cursor's own finish is therefore
 *
 *       played_finish_us = finish_us - frames_to_us(submitted_frames - played_frames)
 *
 *   (the still-in-flight hardware frames). Reporting the played delta against
 *   this timestamp makes the SDK's own arithmetic land on the true buffer
 *   endpoint (finish_us + frames_to_us(queued)), and reports nothing the engine
 *   has not actually played — no enqueue-as-playback, no fabricated DAC time.
 *
 *   The report is emitted exactly once per played-cursor advance, including the
 *   final natural tail: the engine keeps publishing progress for a FINISHed
 *   generation, so the last frames played are reported as the cursor reaches the
 *   exact tail.
 *
 * This header has the SDK dependency and is compiled only by the companion's
 * own opt-in target, never by the C99 daemon or the dependency-free suite.
 */

#ifndef LIBREECHO_SENDPIN_SDK_PLAYER_LISTENER_H
#define LIBREECHO_SENDPIN_SDK_PLAYER_LISTENER_H

#include <algorithm>
#include <cstdint>
#include <mutex>

#include <sendspin/player_role.h>

#include "engine_sink_session.h"

namespace libreecho {
namespace sendspin {

class SdkPlayerListener : public ::sendspin::PlayerRoleListener {
public:
    SdkPlayerListener(EngineSinkSession& session, ::sendspin::PlayerRole* role)
        : session_(session), role_(role) {}

    void set_role(::sendspin::PlayerRole* role) { std::lock_guard<std::recursive_mutex> guard(mu_); role_ = role; }

    // --- SDK sync thread ---------------------------------------------------

    // Returns the engine-acknowledged BYTES (whole frames, <= length). Bytes
    // merely queued to the kernel, or never credited before the deadline, are
    // never counted; the SDK re-issues the unaccepted tail.
    size_t on_audio_write(uint8_t* data, size_t length,
                          uint32_t timeout_ms) override {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        if (!session_.streaming()) return 0;
        return session_.write(data, length, timeout_ms);
    }

    // --- SDK main loop -----------------------------------------------------

    void on_stream_start() override {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        have_boundary_ = false;
        pending_finish_ = false;
        reset_feedback_locked();
        ++stream_starts_;
        session_.start_stream();
    }

    // Requires the authenticated downstream SDK lifecycle patches. Natural
    // completion must FINISH rather than CANCEL; removal also cancels a tail
    // that has already been FINISHed but has not reached the hardware playhead.
    void on_stream_end(::sendspin::SendspinStreamEndReason reason) override {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        ++stream_ends_;
        if (reason == ::sendspin::SendspinStreamEndReason::NATURAL) {
            pending_finish_ = session_.stop_stream() != SinkResult::Ok;
        } else {
            pending_finish_ = false;
            session_.cancel_stream();
        }
    }

    // Informational main-loop notification only. Fencing here is too late:
    // the sync thread may already have delivered post-seek PCM.
    void on_stream_clear() override {}

    uint32_t on_stream_boundary(uint32_t token) override {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        if (have_boundary_ && token == boundary_token_) return 0;
        have_boundary_ = true;
        boundary_token_ = token;
        if (!session_.streaming()) return 0;
        const uint64_t accepted = session_.accepted_frames();
        if (accepted < reported_frames_ || accepted - reported_frames_ > UINT32_MAX) {
            session_.disconnect();
            return 0;
        }
        // Retire the old generation's *SDK accounting*, not hardware playback.
        // CANCEL discards unrendered frames only; already submitted hardware
        // frames remain untouched. Those frames are abandoned from this stream's
        // feedback ledger, never falsely emitted via notify_audio_played().
        // New-generation timing still includes the real hardware horizon.
        const uint32_t retired = static_cast<uint32_t>(accepted - reported_frames_);
        if (session_.cancel_stream() != SinkResult::Ok) {
            session_.disconnect();
            return 0;
        }
        reset_feedback_locked();
        if (session_.start_stream() != SinkResult::Ok) session_.disconnect();
        return retired;
    }

    // Frequency (Hz) the engine reports frames in; the frozen ABI is 48 kHz.
    static constexpr uint32_t kRateHz = LE_AUDIO_SINK_OUTPUT_RATE;

    // frames -> microseconds, checked against 64-bit overflow.
    static bool frames_to_us(uint64_t frames, uint64_t* out_us) {
        if (frames > UINT64_MAX / 1000000u) return false;
        *out_us = frames * 1000000u / kRateHz;
        return true;
    }

    // Derive the finish time of the played cursor from an engine timing sample.
    // Returns false (never a fabricated value) when the sample is invalid, the
    // cursors are inconsistent, or the back-off would underflow.
    static bool derive_played_finish_us(const EngineSinkTiming& timing,
                                        int64_t* out_finish_us) {
        if (!timing.valid || timing.finish_us == 0) return false;
        if (timing.submitted_frames < timing.played_frames) return false;
        const uint64_t in_flight = timing.submitted_frames - timing.played_frames;
        uint64_t in_flight_us = 0;
        if (!frames_to_us(in_flight, &in_flight_us)) return false;
        if (in_flight_us > timing.finish_us || timing.finish_us > INT64_MAX) return false;
        *out_finish_us = static_cast<int64_t>(timing.finish_us - in_flight_us);
        return true;
    }

    // Main-loop feedback pump: report the engine's played-cursor delta with the
    // finish time of exactly those played frames (see the file header).
    // Returns true when a report was made.
    bool pump_feedback() {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        if (role_ == nullptr) return false;
        if (pending_finish_) {
            const auto result = session_.stop_stream();
            if (result == SinkResult::Ok || result == SinkResult::NotOpen) pending_finish_ = false;
            else return false;
        }
        EngineSinkTiming timing{};
        if (session_.poll(&timing) != SinkResult::Ok) return false;
        if (!timing.valid || timing.finish_us == 0) return false;  // never fabricate
        // A physically played tail may have its CREDIT held for the SDK's
        // identical retry. Report only the known acknowledged source prefix;
        // derive that prefix's finish time from the same engine endpoint.
        timing.played_frames = std::min(timing.played_frames, session_.accepted_frames());
        if (timing.played_frames < reported_frames_) return false;
        const uint64_t delta = timing.played_frames - reported_frames_;
        if (delta == 0) return false;
        int64_t finish_us = 0;
        if (!derive_played_finish_us(timing, &finish_us)) return false;
        if (finish_us < last_report_timestamp_us_) return false;  // never regress
        const uint64_t chunk =
            std::min<uint64_t>(delta, static_cast<uint64_t>(UINT32_MAX));
        role_->notify_audio_played(static_cast<uint32_t>(chunk), finish_us);
        reported_frames_ += chunk;
        last_report_timestamp_us_ = finish_us;
        last_report_frames_ = static_cast<uint32_t>(chunk);
        ++report_count_;
        return true;
    }

    // Explicit natural end (companion control path; not the ambiguous callback).
    SinkResult stop_stream(bool* completed_out = nullptr) {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        return session_.stop_stream(completed_out);
    }

    uint64_t reported_frames() const { std::lock_guard<std::recursive_mutex> guard(mu_); return reported_frames_; }
    uint64_t stream_starts() const { std::lock_guard<std::recursive_mutex> guard(mu_); return stream_starts_; }
    uint64_t stream_ends() const { std::lock_guard<std::recursive_mutex> guard(mu_); return stream_ends_; }
    // Observability for tests (and truthful status): the pair last handed to
    // notify_audio_played().
    uint32_t last_report_frames() const { std::lock_guard<std::recursive_mutex> guard(mu_); return last_report_frames_; }
    int64_t last_report_timestamp_us() const { std::lock_guard<std::recursive_mutex> guard(mu_); return last_report_timestamp_us_; }
    uint64_t report_count() const { std::lock_guard<std::recursive_mutex> guard(mu_); return report_count_; }

private:
    void reset_feedback_locked() {
        reported_frames_ = 0;
        last_report_timestamp_us_ = 0;
        last_report_frames_ = 0;
        report_count_ = 0;
    }
    mutable std::recursive_mutex mu_;
    bool pending_finish_ = false;
    bool have_boundary_ = false;
    uint32_t boundary_token_ = 0;
    EngineSinkSession& session_;
    ::sendspin::PlayerRole* role_ = nullptr;
    uint64_t reported_frames_ = 0;
    uint32_t last_report_frames_ = 0;
    int64_t last_report_timestamp_us_ = 0;
    uint64_t report_count_ = 0;
    uint64_t stream_starts_ = 0;
    uint64_t stream_ends_ = 0;
};

}  // namespace sendspin
}  // namespace libreecho

#endif  // LIBREECHO_SENDPIN_SDK_PLAYER_LISTENER_H
