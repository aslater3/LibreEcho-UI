/*
 * LE_AUDIO_SINK/1 client — Sendspin companion native bridge (C++20).
 * See engine_sink.h for the contract. Bounded, non-blocking, generation-fenced.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "engine_sink.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace libreecho {
namespace sendspin {
namespace {

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Remaining milliseconds until `deadline_us`, clamped to [0, 100000] so poll()
// can never be handed a negative or absurd timeout.
int remaining_ms(int64_t deadline_us) {
    int64_t delta = deadline_us - now_us();
    if (delta <= 0) return 0;
    int64_t ms = (delta + 999) / 1000;
    if (ms > 100000) ms = 100000;
    return static_cast<int>(ms);
}

constexpr size_t kSockaddrPath = sizeof(((struct sockaddr_un*)nullptr)->sun_path);

}  // namespace

EngineSink::~EngineSink() {
    std::lock_guard<std::mutex> guard(mu_);
    close_socket_locked();
}

void EngineSink::close_socket_locked() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    connected_ = false;
    armed_ = false;
    pending_frames_ = 0;
    pending_ready_ = false;
    cached_progress_us_ = 0;
}

void EngineSink::fence_locked() {
    armed_ = false;
    pending_frames_ = 0;
    pending_ready_ = false;
    cached_progress_us_ = 0;
}

bool EngineSink::connected() const {
    std::lock_guard<std::mutex> guard(mu_);
    return connected_;
}

uint32_t EngineSink::epoch() const {
    std::lock_guard<std::mutex> guard(mu_);
    return epoch_;
}

uint32_t EngineSink::generation() const {
    std::lock_guard<std::mutex> guard(mu_);
    return generation_;
}

uint64_t EngineSink::accepted_frames() const {
    std::lock_guard<std::mutex> guard(mu_);
    return next_frame_;
}

uint32_t EngineSink::sequence() const {
    std::lock_guard<std::mutex> guard(mu_);
    return sequence_;
}

uint32_t EngineSink::last_error() const {
    std::lock_guard<std::mutex> guard(mu_);
    return last_error_;
}

SinkResult EngineSink::last_result() const {
    std::lock_guard<std::mutex> guard(mu_);
    return last_result_;
}

bool EngineSink::open_generation() const {
    std::lock_guard<std::mutex> guard(mu_);
    return armed_;
}

SinkResult EngineSink::send_datagram(uint8_t type, const uint8_t* payload,
                                     uint32_t length, int64_t deadline_us) {
    if (fd_ < 0) return SinkResult::NotConnected;
    const size_t total = LE_AUDIO_SINK_HEADER_BYTES + static_cast<size_t>(length);
    if (total > LE_AUDIO_SINK_MAX_DATAGRAM_BYTES) return SinkResult::Malformed;
    le_audio_sink_encode_header(tx_, type, length);
    if (length != 0)
        std::memcpy(tx_ + LE_AUDIO_SINK_HEADER_BYTES, payload, length);

    for (;;) {
        ssize_t n = ::send(fd_, tx_, total, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(total)) return SinkResult::Ok;
        if (n < 0) {
            int err = errno;
            if (err == EINTR) continue;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                struct pollfd p;
                p.fd = fd_;
                p.events = POLLOUT;
                p.revents = 0;
                int r = ::poll(&p, 1, remaining_ms(deadline_us));
                if (r < 0 && errno != EINTR) {
                    connected_ = false;
                    return SinkResult::Io;
                }
                if (r == 0) return SinkResult::Timeout;
                continue;
            }
            connected_ = false;
            return SinkResult::Closed;
        }
        // A SOCK_SEQPACKET send is atomic; a short write is a local failure.
        connected_ = false;
        return SinkResult::Io;
    }
}

SinkResult EngineSink::recv_datagram(uint8_t* type, const uint8_t** payload,
                                     uint32_t* length, int64_t deadline_us) {
    if (fd_ < 0) return SinkResult::NotConnected;
    for (;;) {
        ssize_t n = ::recv(fd_, rx_, sizeof(rx_), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            int err = errno;
            if (err == EINTR) continue;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                struct pollfd p;
                p.fd = fd_;
                p.events = POLLIN;
                p.revents = 0;
                int r = ::poll(&p, 1, remaining_ms(deadline_us));
                if (r < 0 && errno != EINTR) {
                    connected_ = false;
                    return SinkResult::Io;
                }
                if (r == 0) return SinkResult::Timeout;
                continue;
            }
            connected_ = false;
            return SinkResult::Closed;
        }
        if (n == 0) {
            connected_ = false;
            return SinkResult::Closed;
        }
        if (static_cast<size_t>(n) > sizeof(rx_)) return SinkResult::Malformed;
        struct le_audio_sink_header header;
        if (le_audio_sink_decode_header(rx_, static_cast<size_t>(n), &header) !=
            LE_AUDIO_SINK_OK)
            return SinkResult::Malformed;
        if (type) *type = header.type;
        if (length) *length = header.length;
        if (payload) *payload = rx_ + LE_AUDIO_SINK_HEADER_BYTES;
        return SinkResult::Ok;
    }
}

SinkResult EngineSink::connect(const std::string& socket_path,
                               uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (socket_path.empty() || socket_path.size() >= kSockaddrPath)
        return SinkResult::Io;
    close_socket_locked();

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        last_result_ = SinkResult::Io;
        return SinkResult::Io;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        int err = errno;
        if (err == EINPROGRESS || err == EAGAIN || err == EWOULDBLOCK) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            int r = ::poll(&p, 1, remaining_ms(deadline));
            if (r == 0) {
                ::close(fd);
                last_result_ = SinkResult::Timeout;
                return SinkResult::Timeout;
            }
            if (r < 0) {
                ::close(fd);
                last_result_ = SinkResult::Io;
                return SinkResult::Io;
            }
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
                soerr != 0) {
                ::close(fd);
                last_result_ = SinkResult::Closed;
                return SinkResult::Closed;
            }
        } else {
            ::close(fd);
            last_result_ = (err == ECONNREFUSED || err == ENOENT)
                               ? SinkResult::Closed
                               : SinkResult::Io;
            return last_result_;
        }
    }

    // Validate the peer credential: the engine admits the companion's own uid.
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
        cred.uid != ::geteuid()) {
        ::close(fd);
        last_error_ = LE_AUDIO_SINK_ERR_PEER;
        last_result_ = SinkResult::Rejected;
        return SinkResult::Rejected;
    }

    fd_ = fd;
    connected_ = true;
    armed_ = false;
    epoch_ = 0;
    generation_ = 0;
    last_generation_ = 0;
    sequence_ = 0;
    last_accepted_sequence_ = 0;
    next_frame_ = 0;
    capacity_frames_ = 0;
    period_frames_ = 0;
    last_error_ = 0;
    last_result_ = SinkResult::Ok;
    return SinkResult::Ok;
}

void EngineSink::disconnect() {
    std::lock_guard<std::mutex> guard(mu_);
    close_socket_locked();
    epoch_ = 0;
    generation_ = 0;
    last_generation_ = 0;
    sequence_ = 0;
    last_accepted_sequence_ = 0;
    next_frame_ = 0;
    last_error_ = 0;
    last_result_ = SinkResult::Ok;
}

SinkResult EngineSink::open(uint32_t generation, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!connected_) {
        last_result_ = SinkResult::NotConnected;
        return last_result_;
    }
    if (generation <= last_generation_ || generation == 0) {
        last_error_ = LE_AUDIO_SINK_ERR_STALE_GENERATION;
        last_result_ = SinkResult::Rejected;
        return last_result_;
    }

    uint8_t payload[LE_AUDIO_SINK_OPEN_PAYLOAD_BYTES];
    le_audio_sink_put_u32(payload + 0, LE_AUDIO_SINK_SOURCE_SENDPIN);
    le_audio_sink_put_u32(payload + 4, generation);
    le_audio_sink_put_u32(payload + 8, LE_AUDIO_SINK_OUTPUT_RATE);
    le_audio_sink_put_u16(payload + 12, LE_AUDIO_SINK_OUTPUT_CHANNELS);
    le_audio_sink_put_u16(payload + 14, LE_AUDIO_SINK_FORMAT_S16_LE);
    le_audio_sink_put_u32(payload + 16, 0);

    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    SinkResult r = send_datagram(LE_AUDIO_SINK_TYPE_OPEN, payload,
                                 sizeof(payload), deadline);
    if (r != SinkResult::Ok) {
        last_result_ = r;
        return r;
    }

    for (int guard = 0; guard < 8; ++guard) {
        uint8_t type = 0;
        const uint8_t* pl = nullptr;
        uint32_t len = 0;
        r = recv_datagram(&type, &pl, &len, deadline);
        if (r != SinkResult::Ok) {
            last_result_ = r;
            return r;
        }
        if (type == LE_AUDIO_SINK_TYPE_ERROR) {
            if (len >= 4) last_error_ = le_audio_sink_get_u32(pl);
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        if (type != LE_AUDIO_SINK_TYPE_OPEN_ACK) continue;
        if (len != LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES) {
            last_result_ = SinkResult::Malformed;
            return last_result_;
        }
        uint32_t status = le_audio_sink_get_u32(pl + 0);
        uint32_t epoch = le_audio_sink_get_u32(pl + 4);
        uint32_t gen = le_audio_sink_get_u32(pl + 8);
        uint32_t rate = le_audio_sink_get_u32(pl + 12);
        uint16_t channels = le_audio_sink_get_u16(pl + 16);
        uint16_t format = le_audio_sink_get_u16(pl + 18);
        uint32_t readiness = le_audio_sink_get_u32(pl + 28);
        if (status != LE_AUDIO_SINK_OK) {
            last_error_ = status;
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        if (gen != generation || rate != LE_AUDIO_SINK_OUTPUT_RATE ||
            channels != LE_AUDIO_SINK_OUTPUT_CHANNELS ||
            format != LE_AUDIO_SINK_FORMAT_S16_LE || epoch == 0 ||
            (readiness & LE_AUDIO_SINK_OPEN_READY) == 0) {
            last_result_ = SinkResult::Malformed;
            return last_result_;
        }
        epoch_ = epoch;
        generation_ = generation;
        last_generation_ = generation;
        sequence_ = 0;
        last_accepted_sequence_ = 0;
        next_frame_ = 0;
        pending_frames_ = 0;
        pending_ready_ = false;
        cached_progress_us_ = 0;
        armed_ = true;
        last_error_ = 0;
        period_frames_ = le_audio_sink_get_u32(pl + 20);
        capacity_frames_ = le_audio_sink_get_u32(pl + 24);
        last_result_ = SinkResult::Ok;
        return SinkResult::Ok;
    }
    last_result_ = SinkResult::Malformed;
    return last_result_;
}

SinkResult EngineSink::parse_progress_locked(const uint8_t* pl, uint32_t len,
                                              EngineSinkTiming* out) {
    if (len != LE_AUDIO_SINK_PROGRESS_PAYLOAD_BYTES) return SinkResult::Malformed;
    if (le_audio_sink_get_u32(pl) != epoch_) return SinkResult::StaleEpoch;
    if (le_audio_sink_get_u32(pl + 4) != generation_) return SinkResult::StaleGeneration;
    const uint32_t flags = le_audio_sink_get_u32(pl + 12);
    out->epoch = epoch_;
    out->generation = generation_;
    out->sequence = le_audio_sink_get_u32(pl + 8);
    out->valid = (flags & LE_AUDIO_SINK_PROGRESS_TIMING_VALID) != 0;
    out->error = (flags & LE_AUDIO_SINK_PROGRESS_TIMING_ERROR) != 0;
    out->submitted_frames = le_audio_sink_get_u64(pl + 16);
    out->finish_us = out->valid ? le_audio_sink_get_u64(pl + 24) : 0u;
    out->played_frames = le_audio_sink_get_u64(pl + 32);
    out->queued_frames = le_audio_sink_get_u32(pl + 40);
    return SinkResult::Ok;
}

SinkResult EngineSink::pending_credit_locked(const uint8_t* pl, uint32_t len) {
    if (pending_frames_ == 0 || pending_ready_ || len != LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES)
        return SinkResult::Malformed;
    if (le_audio_sink_get_u32(pl + 4) != epoch_) return SinkResult::StaleEpoch;
    if (le_audio_sink_get_u32(pl + 8) != generation_) return SinkResult::StaleGeneration;
    if (le_audio_sink_get_u32(pl + 12) != sequence_) return SinkResult::Rejected;
    const uint32_t status = le_audio_sink_get_u32(pl);
    if (status != LE_AUDIO_SINK_OK) {
        last_error_ = status;
        pending_frames_ = 0; // explicit rejection: no frames were accepted
        return SinkResult::Rejected;
    }
    uint64_t expected = 0;
    if (le_audio_sink_cursor_add(next_frame_, pending_frames_, &expected) != LE_AUDIO_SINK_OK ||
        le_audio_sink_get_u64(pl + 16) != expected) {
        last_error_ = LE_AUDIO_SINK_ERR_FRAME_CURSOR;
        return SinkResult::Rejected;
    }
    pending_ready_ = true;
    return SinkResult::Ok;
}

SinkResult EngineSink::await_pending_locked(int64_t deadline) {
    if (pending_ready_ || pending_frames_ == 0) return SinkResult::Ok;
    for (int guard = 0; guard < 8; ++guard) {
        uint8_t type = 0;
        const uint8_t* pl = nullptr;
        uint32_t len = 0;
        SinkResult r = recv_datagram(&type, &pl, &len, deadline);
        if (r != SinkResult::Ok) return r;
        if (type == LE_AUDIO_SINK_TYPE_ERROR) {
            if (len >= 4) last_error_ = le_audio_sink_get_u32(pl);
            pending_frames_ = 0;
            return SinkResult::Rejected;
        }
        if (type == LE_AUDIO_SINK_TYPE_CREDIT) return pending_credit_locked(pl, len);
        if (type == LE_AUDIO_SINK_TYPE_PROGRESS) {
            r = parse_progress_locked(pl, len, &cached_progress_);
            if (r != SinkResult::Ok) return r;
            cached_progress_us_ = now_us();
        }
    }
    return SinkResult::Malformed;
}

void EngineSink::commit_pending_locked() {
    last_accepted_sequence_ = sequence_++;
    next_frame_ += pending_frames_;
    pending_frames_ = 0;
    pending_ready_ = false;
}

size_t EngineSink::on_audio_write(const uint8_t* pcm, size_t length,
                                  uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!connected_) { last_result_ = SinkResult::NotConnected; return 0; }
    if (!armed_) { last_result_ = SinkResult::NotOpen; return 0; }
    if (!pcm || length == 0) return 0;
    if (length % LE_AUDIO_SINK_BYTES_PER_FRAME != 0) {
        last_result_ = SinkResult::Malformed;
        return 0;
    }
    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    size_t accepted_bytes = 0;
    while (accepted_bytes < length) {
        const uint8_t* src = pcm + accepted_bytes;
        const size_t remaining = length - accepted_bytes;
        if (pending_frames_) {
            const size_t pending_bytes = pending_frames_ * LE_AUDIO_SINK_BYTES_PER_FRAME;
            // An SDK retry must offer the same unaccepted prefix. A changed
            // offer cannot inherit somebody else's delayed application credit.
            if (remaining < pending_bytes || std::memcmp(src, pending_pcm_, pending_bytes) != 0) {
                close_socket_locked();
                last_result_ = SinkResult::Malformed;
                break;
            }
        } else {
            const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(
                remaining / LE_AUDIO_SINK_BYTES_PER_FRAME, LE_AUDIO_SINK_MAX_DATA_FRAMES));
            uint8_t payload[LE_AUDIO_SINK_DATA_PREFIX_BYTES +
                            LE_AUDIO_SINK_MAX_DATA_FRAMES * LE_AUDIO_SINK_BYTES_PER_FRAME];
            le_audio_sink_put_u32(payload + 0, epoch_);
            le_audio_sink_put_u32(payload + 4, generation_);
            le_audio_sink_put_u32(payload + 8, sequence_);
            le_audio_sink_put_u32(payload + 12, 0);
            le_audio_sink_put_u64(payload + 16, next_frame_);
            le_audio_sink_put_u32(payload + 24, chunk);
            le_audio_sink_put_u32(payload + 28, 0);
            const size_t bytes = chunk * LE_AUDIO_SINK_BYTES_PER_FRAME;
            std::memcpy(payload + LE_AUDIO_SINK_DATA_PREFIX_BYTES, src, bytes);
            SinkResult r = send_datagram(LE_AUDIO_SINK_TYPE_DATA, payload,
                LE_AUDIO_SINK_DATA_PREFIX_BYTES + static_cast<uint32_t>(bytes), deadline);
            if (r != SinkResult::Ok) { last_result_ = r; break; }
            pending_frames_ = chunk;
            pending_ready_ = false;
            std::memcpy(pending_pcm_, src, bytes);
        }
        SinkResult r = await_pending_locked(deadline);
        if (r != SinkResult::Ok) { last_result_ = r; break; }
        accepted_bytes += pending_frames_ * LE_AUDIO_SINK_BYTES_PER_FRAME;
        commit_pending_locked();
        last_result_ = SinkResult::Ok;
        if (now_us() >= deadline) break;
    }
    return accepted_bytes;
}

SinkResult EngineSink::finish(uint64_t exact_total_frames, uint32_t timeout_ms,
                              bool* completed_out) {
    std::lock_guard<std::mutex> guard(mu_);
    if (completed_out) *completed_out = false;
    if (!connected_) {
        last_result_ = SinkResult::NotConnected;
        return last_result_;
    }
    if (!armed_) {
        last_result_ = SinkResult::NotOpen;
        return last_result_;
    }

    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    // The caller's exact total is its acknowledged prefix at entry. A DATA
    // already sent before natural end may gain its authenticated CREDIT now.
    // Resolve that single bounded tail before declaring the engine total.
    if (pending_frames_ && exact_total_frames == next_frame_) {
        SinkResult pending = await_pending_locked(deadline);
        if (pending != SinkResult::Ok) { last_result_ = pending; return pending; }
        commit_pending_locked();
        exact_total_frames = next_frame_;
    }
    uint8_t payload[LE_AUDIO_SINK_FINISH_PAYLOAD_BYTES];
    le_audio_sink_put_u32(payload + 0, epoch_);
    le_audio_sink_put_u32(payload + 4, generation_);
    le_audio_sink_put_u64(payload + 8, exact_total_frames);
    le_audio_sink_put_u32(payload + 16, last_accepted_sequence_);
    le_audio_sink_put_u32(payload + 20, 0);

    SinkResult r = send_datagram(LE_AUDIO_SINK_TYPE_FINISH, payload,
                                 sizeof(payload), deadline);
    if (r != SinkResult::Ok) {
        last_result_ = r;
        return r;
    }

    for (int guard = 0; guard < 8; ++guard) {
        uint8_t type = 0;
        const uint8_t* pl = nullptr;
        uint32_t len = 0;
        r = recv_datagram(&type, &pl, &len, deadline);
        if (r != SinkResult::Ok) {
            last_result_ = r;
            return r;
        }
        if (type == LE_AUDIO_SINK_TYPE_ERROR) {
            if (len >= 4) last_error_ = le_audio_sink_get_u32(pl);
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        if (type != LE_AUDIO_SINK_TYPE_FINISH_ACK) continue;
        if (len != LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES) {
            last_result_ = SinkResult::Malformed;
            return last_result_;
        }
        uint32_t status = le_audio_sink_get_u32(pl + 0);
        uint32_t epoch = le_audio_sink_get_u32(pl + 4);
        uint32_t gen = le_audio_sink_get_u32(pl + 8);
        uint64_t total = le_audio_sink_get_u64(pl + 12);
        uint32_t completed = le_audio_sink_get_u32(pl + 20);
        if (status != LE_AUDIO_SINK_OK) {
            last_error_ = status;
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        // Validate the full (epoch, generation) identity before disarming: a
        // mismatched ack must never fence the live client.
        if (epoch != epoch_) {
            last_result_ = SinkResult::StaleEpoch;
            return last_result_;
        }
        if (gen != generation_) {
            last_result_ = SinkResult::StaleGeneration;
            return last_result_;
        }
        if (total != exact_total_frames) {
            last_result_ = SinkResult::Malformed;
            return last_result_;
        }
        armed_ = false;
        last_generation_ = generation_;
        if (completed_out) *completed_out = completed != 0;
        last_result_ = SinkResult::Ok;
        return SinkResult::Ok;
    }
    last_result_ = SinkResult::Malformed;
    return last_result_;
}

SinkResult EngineSink::cancel(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (!connected_) {
        last_result_ = SinkResult::NotConnected;
        return last_result_;
    }
    if (generation_ == 0) {
        last_result_ = SinkResult::NotOpen;
        return last_result_;
    }

    uint8_t payload[LE_AUDIO_SINK_RESET_PAYLOAD_BYTES];
    le_audio_sink_put_u32(payload + 0, epoch_);
    le_audio_sink_put_u32(payload + 4, generation_);
    le_audio_sink_put_u32(payload + 8, 0);
    le_audio_sink_put_u32(payload + 12, 0);

    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    SinkResult r = send_datagram(LE_AUDIO_SINK_TYPE_CANCEL, payload,
                                 sizeof(payload), deadline);
    if (r != SinkResult::Ok) {
        last_result_ = r;
        return r;
    }

    for (int guard = 0; guard < 8; ++guard) {
        uint8_t type = 0;
        const uint8_t* pl = nullptr;
        uint32_t len = 0;
        r = recv_datagram(&type, &pl, &len, deadline);
        if (r != SinkResult::Ok) {
            last_result_ = r;
            return r;
        }
        if (type == LE_AUDIO_SINK_TYPE_ERROR) {
            if (len >= 4) last_error_ = le_audio_sink_get_u32(pl);
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        if (type != LE_AUDIO_SINK_TYPE_RESET_ACK) continue;
        if (len != LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES) {
            last_result_ = SinkResult::Malformed;
            return last_result_;
        }
        uint32_t status = le_audio_sink_get_u32(pl + 0);
        uint32_t old_epoch = le_audio_sink_get_u32(pl + 4);
        uint32_t old_generation = le_audio_sink_get_u32(pl + 8);
        if (status != LE_AUDIO_SINK_OK) {
            last_error_ = status;
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        // A RESET_ACK must name this client's live (epoch, generation) before
        // it fences anything; a mismatched ack must not fence the live client.
        if (old_epoch != epoch_) {
            last_result_ = SinkResult::StaleEpoch;
            return last_result_;
        }
        if (old_generation != generation_) {
            last_result_ = SinkResult::StaleGeneration;
            return last_result_;
        }
        // Fence: no further audio is accepted until a strictly higher OPEN, and
        // the fenced generation's source cursor is discarded.
        fence_locked();
        last_generation_ = generation_;
        sequence_ = 0;
        last_accepted_sequence_ = 0;
        next_frame_ = 0;
        last_result_ = SinkResult::Ok;
        return SinkResult::Ok;
    }
    last_result_ = SinkResult::Malformed;
    return last_result_;
}

SinkResult EngineSink::progress(EngineSinkTiming* out, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> guard(mu_);
    if (out == nullptr) {
        last_result_ = SinkResult::Malformed;
        return last_result_;
    }
    if (!connected_) {
        last_result_ = SinkResult::NotConnected;
        return last_result_;
    }
    // Progress is readable for any opened generation (including a FINISHed one):
    // it does not itself accept audio, so it is not gated on `armed_`.
    if (generation_ == 0) {
        last_result_ = SinkResult::NotOpen;
        return last_result_;
    }

    // One fresh, identity-validated late reply may arrive during a DATA wait.
    // Preserve it rather than discarding it and starving the main-loop pump.
    if (cached_progress_us_ != 0) {
        const auto age = now_us() - cached_progress_us_;
        cached_progress_us_ = 0;
        if (age >= 0 && age <= 100000) {
            *out = cached_progress_;
            last_result_ = SinkResult::Ok;
            return last_result_;
        }
    }
    uint8_t payload[LE_AUDIO_SINK_PROGRESS_REQ_PAYLOAD_BYTES];
    le_audio_sink_put_u32(payload + 0, epoch_);
    le_audio_sink_put_u32(payload + 4, generation_);

    const int64_t deadline = now_us() + static_cast<int64_t>(timeout_ms) * 1000;
    SinkResult r = send_datagram(LE_AUDIO_SINK_TYPE_PROGRESS_REQ, payload,
                                 sizeof(payload), deadline);
    if (r != SinkResult::Ok) {
        last_result_ = r;
        return r;
    }

    for (int guard = 0; guard < 8; ++guard) {
        uint8_t type = 0;
        const uint8_t* pl = nullptr;
        uint32_t len = 0;
        r = recv_datagram(&type, &pl, &len, deadline);
        if (r != SinkResult::Ok) {
            last_result_ = r;
            return r;
        }
        if (type == LE_AUDIO_SINK_TYPE_ERROR) {
            if (len >= 4) last_error_ = le_audio_sink_get_u32(pl);
            last_result_ = SinkResult::Rejected;
            return last_result_;
        }
        if (type == LE_AUDIO_SINK_TYPE_CREDIT && pending_frames_) {
            r = pending_credit_locked(pl, len);
            if (r != SinkResult::Ok) { last_result_ = r; return r; }
            continue;
        }
        if (type != LE_AUDIO_SINK_TYPE_PROGRESS) continue;
        r = parse_progress_locked(pl, len, out);
        if (r != SinkResult::Ok) { last_result_ = r; return r; }
        last_result_ = SinkResult::Ok;
        return SinkResult::Ok;
    }
    last_result_ = SinkResult::Malformed;
    return last_result_;
}

}  // namespace sendspin
}  // namespace libreecho
