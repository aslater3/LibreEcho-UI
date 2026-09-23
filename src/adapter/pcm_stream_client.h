/* Bounded PCM producer client. No ALSA or provider knowledge belongs here. */
#ifndef LIBREECHO_PCM_STREAM_CLIENT_H
#define LIBREECHO_PCM_STREAM_CLIENT_H
#include "pcm_stream_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct le_pcm_progress {
    unsigned int state;
    uint64_t accepted_frames;
    uint64_t played_frames;
};
static inline int le_pcm_is_stream(int fd)
{
    int type = 0;
    socklen_t size = sizeof(type);
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 &&
           type == SOCK_SEQPACKET;
}
static inline int le_pcm_command(int fd, unsigned int kind)
{
    unsigned char message[LE_PCM_HEADER];
    ssize_t sent;
    le_pcm_header(message, kind, 0, 0);
    do { sent = send(fd, message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)sizeof(message) ? 0 : -1;
}
/* Legacy is an explicit migration option for old producers/test fixtures.
 * Live requires the managed endpoint: it must never cancel a shared FIFO. */
static inline int le_pcm_open(const char *bus_path, unsigned int role,
                               int focus, int allow_legacy)
{
    struct sockaddr_un address;
    unsigned char message[LE_PCM_HEADER];
    const char *slash;
    size_t directory;
    int fd, saved;
    if (!bus_path || role > 3U || !(slash = strrchr(bus_path, '/'))) {
        errno = EINVAL; return -1;
    }
    directory = (size_t)(slash - bus_path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (directory + 1U + sizeof(LE_PCM_SOCKET) > sizeof(address.sun_path)) {
        errno = ENAMETOOLONG; return -1;
    }
    memcpy(address.sun_path, bus_path, directory);
    address.sun_path[directory] = '/';
    memcpy(address.sun_path + directory + 1U, LE_PCM_SOCKET, sizeof(LE_PCM_SOCKET));
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        saved = errno; close(fd);
        /* Never bypass a live but overloaded/refusing engine. */
        if (allow_legacy && (saved == ENOENT || saved == ECONNREFUSED))
            return open(bus_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        errno = saved; return -1;
    }
    le_pcm_header(message, LE_PCM_OPEN, 0, role | (focus ? LE_PCM_FOCUS : 0U));
    if (send(fd, message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) !=
        (ssize_t)sizeof(message)) {
        saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}
/* A packet is atomic. EAGAIN consumes no frames, so the caller can retain the
 * exact remainder rather than lose a partial nonblocking write. */
static inline ssize_t le_pcm_write(int fd, const void *pcm, size_t bytes)
{
    unsigned char message[LE_PCM_PACKET_BYTES];
    ssize_t sent;
    size_t count = bytes;
    if (!le_pcm_is_stream(fd)) return write(fd, pcm, bytes);
    if (!pcm || !bytes || bytes % LE_PCM_FRAME_BYTES) { errno = EINVAL; return -1; }
    if (count > LE_PCM_PACKET_FRAMES * LE_PCM_FRAME_BYTES)
        count = LE_PCM_PACKET_FRAMES * LE_PCM_FRAME_BYTES;
    le_pcm_header(message, LE_PCM_DATA, (unsigned int)(count / LE_PCM_FRAME_BYTES), 0);
    memcpy(message + LE_PCM_HEADER, pcm, count);
    sent = send(fd, message, LE_PCM_HEADER + count, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0) return -1;
    if (sent != (ssize_t)(LE_PCM_HEADER + count)) { errno = EIO; return -1; }
    return (ssize_t)count;
}
static inline int le_pcm_finish(int fd)
{
    return le_pcm_is_stream(fd) ? le_pcm_command(fd, LE_PCM_FINISH) : 0;
}
/* 1=new progress, 0=no update, -1=closed/malformed/error. The engine replies
 * contain counters only, never microphone or model audio. */
static inline int le_pcm_progress_read(int fd, struct le_pcm_progress *progress)
{
    unsigned char message[LE_PCM_HEADER];
    ssize_t n;
    int changed = 0;
    unsigned int budget;
    if (!progress || !le_pcm_is_stream(fd)) { errno = EINVAL; return -1; }
    for (budget = 0; budget < 16U; ++budget) {
        n = recv(fd, message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return changed;
        if (n != (ssize_t)sizeof(message) || le_pcm_get32(message) != LE_PCM_MAGIC ||
            le_pcm_get32(message + 4) != LE_PCM_STATE) return -1;
        progress->state = le_pcm_get32(message + 8);
        progress->accepted_frames = le_pcm_get64(message + 16);
        progress->played_frames = le_pcm_get64(message + 24);
        if (progress->state < LE_PCM_ACCEPTING || progress->state > LE_PCM_FAILED ||
            progress->played_frames > progress->accepted_frames) return -1;
        changed = 1;
    }
    return changed;
}
static inline uint64_t le_pcm_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
/* Worker-side finite producers keep their socket until DRAINED. Main event
 * loops use nonblocking finish/progress instead. Every wait is bounded. */
static inline int le_pcm_finish_wait(int fd, unsigned int timeout_ms,
                                      const volatile sig_atomic_t *cancel)
{
    uint64_t deadline = le_pcm_monotonic_ms() + timeout_ms;
    struct le_pcm_progress progress = {0, 0, 0};
    int finished = 0;
    if (!le_pcm_is_stream(fd)) return 0;
    for (;;) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        if (cancel && *cancel) { errno = ECANCELED; return -1; }
        if (!finished) {
            if (le_pcm_finish(fd) == 0) finished = 1;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            else descriptor.events |= POLLOUT;
        }
        if (le_pcm_progress_read(fd, &progress) < 0) return -1;
        if (progress.state == LE_PCM_DRAINED) return 0;
        if (progress.state == LE_PCM_CANCELLED || progress.state == LE_PCM_FAILED) {
            errno = ECANCELED; return -1;
        }
        if (le_pcm_monotonic_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        if (poll(&descriptor, 1, 10) < 0 && errno != EINTR) return -1;
    }
}
#endif
