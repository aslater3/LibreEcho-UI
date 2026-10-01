/*
 * LE_AUDIO_SINK/1 client — the Sendspin companion's native bridge to the
 * LibreEcho shared PCM engine.
 *
 * This is the C++20 companion side of the frozen LE_AUDIO_SINK/1 contract
 * (audio_sink_protocol.h, mirror of the Platform source of truth). It is the
 * object the Sendspin SDK's player role binds to: the SDK sync thread calls
 * on_audio_write() with decoded PCM and the sink turns that into bounded,
 * acknowledged DATA; control callbacks (start, finish, seek-clear, role
 * removal, disconnect) drive the generation lifecycle.
 *
 * Core contract (Task 5):
 *
 *   * on_audio_write() returns ACKNOWLEDGED BYTES — always a whole number of
 *     PCM frames, never more than it was offered. A successful socket write is
 *     NOT acceptance: only an OK CREDIT whose (epoch, generation, sequence)
 *     names the live session advances the acknowledged count. The caller
 *     re-issues any unaccepted tail.
 *   * Source progress is cumulative and exactly-once: each accepted DATA
 *     advances the cumulative source-frame cursor by exactly its frame count,
 *     once. A stale epoch/generation/sequence CREDIT (after a reconnect or an
 *     engine restart) is rejected without moving the cursor.
 *   * The generation lifecycle is explicit and separated: open(), finish()
 *     (natural end), cancel() (seek-clear / role removal) and disconnect().
 *     After a fence the client is disarmed: no audio is accepted (and nothing
 *     is sent) until a strictly higher generation is OPENed, so a late ACK can
 *     never permit audio for a successor.
 *   * The engine owns synchronization. This client never invents a DAC
 *     timestamp, a playhead or a finish time; it reports what the engine
 *     publishes (TIMING_VALID only), and never claims completion.
 *
 * Transport: AF_UNIX SOCK_SEQPACKET, bounded and non-blocking. Every operation
 * takes a wall-clock deadline and returns on it; a stalled engine yields zero
 * accepted bytes rather than blocking the SDK sync thread. The peer credential
 * is validated on connect, and every reply is validated against the live
 * (epoch, generation) before it touches state.
 *
 * Threading: EngineSink is not thread-safe on its own. It serializes its own
 * public calls with an internal mutex, but the SDK contract runs on_audio_write
 * on the sync thread while the control callbacks run on the main loop, so a
 * single owner must dispatch control operations through the sync thread (or the
 * other way round). A control call will wait for an in-flight, deadline-bounded
 * write; it will never deadlock.
 *
 * This header is C++20 and is built only by the companion's own opt-in target.
 * It is never linked into the C99 HTTP daemon.
 */

#ifndef LIBREECHO_SENDPIN_ENGINE_SINK_H
#define LIBREECHO_SENDPIN_ENGINE_SINK_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "audio_sink_protocol.h"

namespace libreecho {
namespace sendspin {

// Result of one EngineSink operation. Ok is success; every other value is a
// bounded, fail-closed outcome that never advances accepted audio.
enum class SinkResult {
    Ok = 0,
    NotConnected,   // no live socket
    NotOpen,        // no live generation (not opened, finished or fenced)
    Timeout,        // the bounded deadline expired
    Closed,         // the peer went away (EOF or fatal socket error)
    Rejected,       // the engine replied ERROR (see last_error())
    Malformed,      // a reply failed framing/geometry/readiness validation
    StaleEpoch,     // a reply named a non-live engine epoch
    StaleGeneration,// a reply named a non-live generation
    Io              // a local socket error
};

// Snapshot of engine progress for the live generation. `valid`/`finish_us` are
// only ever set from a TIMING_VALID reply for the live (epoch, generation); no
// value is fabricated. `completed` comes only from a FINISH_ACK driven by the
// engine's physical playhead.
struct EngineSinkTiming {
    uint32_t epoch = 0;
    uint32_t generation = 0;
    uint32_t sequence = 0;
    bool valid = false;
    bool error = false;
    uint64_t submitted_frames = 0;
    uint64_t finish_us = 0;
    uint64_t played_frames = 0;
    uint32_t queued_frames = 0;
};

class EngineSink {
public:
    EngineSink() = default;
    ~EngineSink();

    EngineSink(const EngineSink&) = delete;
    EngineSink& operator=(const EngineSink&) = delete;

    // Connect to the engine listener with a bounded, non-blocking connect and
    // validate the peer credential (the engine's euid). Reconnecting after a
    // disconnect adopts a fresh socket; any prior generation state is cleared.
    SinkResult connect(const std::string& socket_path, uint32_t timeout_ms);

    // True while a socket is held.
    bool connected() const;

    // Close the socket and fence the current generation. No further audio is
    // accepted until a new connect()/open() pair.
    void disconnect();

    // OPEN a strictly increasing generation and validate OPEN_ACK (status,
    // geometry, readiness) before adopting the engine epoch. On success the
    // cumulative source cursor and sequence restart at zero.
    SinkResult open(uint32_t generation, uint32_t timeout_ms);

    // The SDK sync-thread entry point. `length` is a whole number of frames'
    // worth of interleaved S16_LE bytes. Returns the number of BYTES the engine
    // acknowledged (<= length, frame-aligned). Bytes only queued to the kernel,
    // or never ACKed before the deadline, are never counted.
    size_t on_audio_write(const uint8_t* pcm, size_t length, uint32_t timeout_ms);

    // Declare the exact cumulative total for the live generation and await
    // FINISH_ACK. `completed_out` (optional) reports the engine's completion
    // flag, which is set only once the physical playhead has passed the tail.
    SinkResult finish(uint64_t exact_total_frames, uint32_t timeout_ms,
                      bool* completed_out);

    // Fence the live generation (seek-clear / role removal). Unrendered frames
    // are discarded by the engine; already-rendered frames are untouched. The
    // client is disarmed until a strictly higher generation is OPENed.
    SinkResult cancel(uint32_t timeout_ms);

    // Query engine progress for the live generation into `out`.
    SinkResult progress(EngineSinkTiming* out, uint32_t timeout_ms);

    uint32_t epoch() const;
    uint32_t generation() const;
    uint64_t accepted_frames() const;  // cumulative acknowledged source frames
    uint32_t sequence() const;         // last acknowledged DATA sequence
    uint32_t last_error() const;       // engine status of the last Rejected
    SinkResult last_result() const;    // outcome of the last operation
    bool open_generation() const;      // armed and accepting audio

private:
    // --- bounded I/O -------------------------------------------------------
    SinkResult send_datagram(uint8_t type, const uint8_t* payload, uint32_t length,
                             int64_t deadline_us);
    // Receive one whole datagram, validated against the frozen framing.
    SinkResult recv_datagram(uint8_t* type, const uint8_t** payload,
                             uint32_t* length, int64_t deadline_us);

    SinkResult parse_progress_locked(const uint8_t* payload, uint32_t length, EngineSinkTiming* out);
    SinkResult pending_credit_locked(const uint8_t* payload, uint32_t length);
    SinkResult await_pending_locked(int64_t deadline_us);
    void commit_pending_locked();
    void close_socket_locked();
    void fence_locked();

    int fd_ = -1;
    bool connected_ = false;
    bool armed_ = false;

    uint32_t epoch_ = 0;
    uint32_t generation_ = 0;
    uint32_t last_generation_ = 0;
    uint32_t sequence_ = 0;             // next DATA sequence
    uint32_t last_accepted_sequence_ = 0;  // sequence of the last accepted DATA
    uint64_t next_frame_ = 0;  // cumulative acknowledged source frames
    uint32_t last_error_ = 0;
    SinkResult last_result_ = SinkResult::Ok;
    uint32_t capacity_frames_ = 0;
    uint32_t period_frames_ = 0;

    // Exactly one transmitted DATA may await its CREDIT across SDK deadlines.
    // Never retransmit it; return credit only against the identical PCM prefix.
    uint32_t pending_frames_ = 0;
    bool pending_ready_ = false;
    uint8_t pending_pcm_[LE_AUDIO_SINK_MAX_DATA_FRAMES * LE_AUDIO_SINK_BYTES_PER_FRAME];
    EngineSinkTiming cached_progress_{};
    int64_t cached_progress_us_ = 0;

    uint8_t rx_[LE_AUDIO_SINK_MAX_DATAGRAM_BYTES];
    uint8_t tx_[LE_AUDIO_SINK_MAX_DATAGRAM_BYTES];

    mutable std::mutex mu_;
};

}  // namespace sendspin
}  // namespace libreecho

#endif  // LIBREECHO_SENDPIN_ENGINE_SINK_H
