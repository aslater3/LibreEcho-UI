// SPDX-License-Identifier: MIT
//
// Shared host harness for the Sendspin engine-sink C++20 client tests.
//
// It builds on the *real* Platform LE_AUDIO_SINK/1 server (audio_sink.c) and on
// a minimal scripted raw peer used only to inject malformed / adversarial
// replies. Nothing here reimplements the protocol: the happy path and the
// generation lifecycle run against the compiled real engine, and the scripted
// peer exists so the client's own reply validation can be driven with bytes the
// real engine would never emit.
//
// No host hardware, /run path or device node is touched: sockets live under a
// private scratch directory.

#ifndef LIBREECHO_SENDPIN_TEST_ENGINE_FIXTURE_H
#define LIBREECHO_SENDPIN_TEST_ENGINE_FIXTURE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio_sink.h"

namespace libreecho {
namespace sendspin {
namespace test {

inline int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// --- socket paths under a private scratch directory ------------------------

inline std::string scratch_dir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string base = (tmp && *tmp) ? tmp : ".";
    return base + "/sendspin-native-bridge";
}

// A short, unique AF_UNIX path (sun_path is 108 bytes). Kept deliberately short
// so nested workspace paths cannot overflow it.
inline std::string unique_socket_path(const char* tag) {
    static std::atomic<unsigned> counter{0};
    std::string dir = scratch_dir();
    ::mkdir(dir.c_str(), 0700);
    char name[128];
    std::snprintf(name, sizeof(name), "%s/%s-%d-%u.sock", dir.c_str(), tag,
                  (int)::getpid(), counter.fetch_add(1));
    return std::string(name);
}

// --- the real engine server ------------------------------------------------

// Owns a real le_audio_sink listener and services it from a background thread,
// exactly as the production engine's poll loop would.
class EngineServer {
public:
    EngineServer() = default;
    ~EngineServer() { stop(); }
    EngineServer(const EngineServer&) = delete;
    EngineServer& operator=(const EngineServer&) = delete;

    bool start(const std::string& path, uint32_t capacity_frames = 0,
               uid_t allowed_uid = (uid_t)-1, int allow_root = 0) {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        if (sink_) return false;
        struct le_audio_sink_config config;
        std::memset(&config, 0, sizeof(config));
        config.socket_path = path.c_str();
        config.capacity_frames = capacity_frames;
        config.allowed_uid = allowed_uid;
        config.allow_root = allow_root;
        sink_ = le_audio_sink_create(&config);
        if (!sink_) return false;
        epoch_ = le_audio_sink_epoch(sink_);
        path_ = path;
        stop_.store(false);
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                if (!paused_.load()) {
                    // Hold mu_ so an injected render/commit/note_* (with_sink)
                    // is serialized with servicing, as run_engine's single
                    // thread does in production.
                    std::lock_guard<std::recursive_mutex> guard(mu_);
                    if (sink_) le_audio_sink_service(sink_);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        return true;
    }

    // Run fn(sink) with the service loop excluded, so a test can inject timing /
    // render / commit directly without racing the background servicer. Returns
    // false when there is no live sink.
    template <class F>
    bool with_sink(F&& fn) {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        if (!sink_) return false;
        fn(sink_);
        return true;
    }

    // Pause/resume servicing to model a stalled engine without tearing the
    // listener down (the socket stays bound and the queue keeps its frames).
    void pause() {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        paused_.store(true);
    }
    void resume() {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        paused_.store(false);
    }

    void stop() {
        {
            std::lock_guard<std::recursive_mutex> guard(mu_);
            if (!sink_) return;
            stop_.store(true);
        }
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::recursive_mutex> guard(mu_);
        le_audio_sink_destroy(sink_);
        sink_ = nullptr;
    }

    le_audio_sink* sink() {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        return sink_;
    }
    uint32_t epoch() const { return epoch_; }
    const std::string& path() const { return path_; }

    // Service a bounded number of cycles, for tests that need a synchronous
    // pump rather than the background thread.
    void pump(int cycles) {
        std::lock_guard<std::recursive_mutex> guard(mu_);
        for (int i = 0; i < cycles && sink_; ++i) le_audio_sink_service(sink_);
    }

private:
    std::recursive_mutex mu_;
    le_audio_sink* sink_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::thread thread_;
    uint32_t epoch_ = 0;
    std::string path_;
};

// --- a scripted raw peer ----------------------------------------------------

inline void raw_send(int fd, const void* data, size_t len) {
    (void)::send(fd, data, len, MSG_NOSIGNAL);
}

// Reads one datagram from `fd` (blocking with a timeout). Returns bytes or -1.
inline ssize_t raw_recv(int fd, uint8_t* buf, size_t cap) {
    for (;;) {
        ssize_t n = ::recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

class RawPeer {
public:
    using Handler = std::function<void(int fd)>;

    RawPeer() = default;
    ~RawPeer() { stop(); }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    bool start(const std::string& path, Handler handler) {
        ::unlink(path.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) { ::close(listen_fd_); return false; }
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            ::close(listen_fd_); listen_fd_ = -1; return false;
        }
        ::chmod(path.c_str(), 0600);
        if (::listen(listen_fd_, 4) != 0) { ::close(listen_fd_); listen_fd_ = -1; return false; }
        path_ = path;
        stop_.store(false);
        handler_ = std::move(handler);
        thread_ = std::thread([this] {
            struct timeval tv{2, 0};
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            while (!stop_.load()) {
                int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                    break;
                }
                handler_(fd);
                ::close(fd);
            }
        });
        return true;
    }

    void stop() {
        if (listen_fd_ >= 0) {
            stop_.store(true);
            ::shutdown(listen_fd_, SHUT_RDWR);
            if (thread_.joinable()) thread_.join();
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (!path_.empty()) { ::unlink(path_.c_str()); path_.clear(); }
    }

private:
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    Handler handler_;
    std::string path_;
};

// --- PCM helpers ------------------------------------------------------------

// Deterministic per-frame payload: every frame differs, so a constant pattern
// could never satisfy an identity check.
inline void fill_pattern(int16_t* frames, size_t frame_count) {
    for (size_t i = 0; i < frame_count; ++i) {
        frames[i * 2] = (int16_t)(0x1234 + (int)(i * 7u));
        frames[i * 2 + 1] = (int16_t)(0x0abc - (int)(i * 3u));
    }
}

inline constexpr size_t kBytesPerFrame = LE_AUDIO_SINK_BYTES_PER_FRAME;

}  // namespace test
}  // namespace sendspin
}  // namespace libreecho

#endif  // LIBREECHO_SENDPIN_TEST_ENGINE_FIXTURE_H
