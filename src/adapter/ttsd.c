/*
 * LibreEcho TTS companion daemon (ttsd).
 *
 * Listens on a UNIX socket for "speak" and "stop_speech" commands using
 * the same JSON adapter protocol as audiod.  On "speak", it synthesizes
 * text to S16_LE PCM via the tts_engine interface, resamples to the
 * announcement bus rate (48 kHz stereo), and writes the result to the
 * announcement bus FIFO at /run/libreecho-audio/announcement.pcm.
 *
 * The audio_engine (in the Kernel repo) already handles ducking media
 * by 12 dB and driving the green LED pulse whenever the announcement
 * bus has active samples — ttsd only needs to produce PCM.
 *
 * Design:
 *   - Pure C99, no C++ in this file (sherpa-onnx lives behind tts_engine.h)
 *   - Fork model: each "speak" forks a child that synthesizes + writes
 *     PCM, keeping the daemon responsive for "stop_speech" and status
 *   - At most one active utterance; a new "speak" kills the previous
 *   - "stop_speech" kills the active child immediately
 */
#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "log.h"
#include "tts_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef LE_TTSD_ENGINE_SHERPA
# define LE_TTSD_ENGINE_NAME "sherpa-onnx"
#else
# define LE_TTSD_ENGINE_NAME "mock"
#endif

#define LE_TTS_MAX_CLIENTS 4
#define LE_TTS_DEFAULT_SOCKET "/run/libreecho/tts.sock"
#define LE_ANNOUNCEMENT_BUS "/run/libreecho-audio/announcement.pcm"
#define LE_TTS_CHUNK_FRAMES 1024
#define LE_TTS_MAX_TEXT 4096
#define LE_TTS_STREAM_MAX_PHRASES 8
#define LE_TTS_STREAM_GAP_FRAMES 2048
#define LE_TTS_STREAM_GAP_WAIT_MS 40
#define LE_TTS_STREAM_MAX_GAP_MS 5000
#define LE_TTS_MAX_CPUS 16
#define LE_TTS_REQUEST_ID_MAX 64
#define LE_TTS_FIRST_PCM_FILE "/run/libreecho/tts-first-pcm"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_child_exited;
static pid_t g_active_child = -1;
static char g_pending_text[LE_TTS_MAX_TEXT];
static char g_pending_request_id[LE_TTS_REQUEST_ID_MAX];
static int g_pending_speech;

static void mark_first_pcm(const char *request_id);

static const char *announcement_bus_path(void)
{
    const char *path = getenv("LE_TTS_ANNOUNCEMENT_BUS");

    return path && path[0] != '\0' ? path : LE_ANNOUNCEMENT_BUS;
}

struct client {
    int fd;
    char input[LE_ADAPTER_MSG_MAX];
    size_t input_used;
    char output[LE_ADAPTER_MSG_MAX];
    size_t output_used;
    size_t output_sent;
};

struct pcm_stream_chunk {
    int16_t *samples;
    size_t frames;
    struct pcm_stream_chunk *next;
};

struct pcm_stream_queue {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct pcm_stream_chunk *head;
    struct pcm_stream_chunk *tail;
    int closed;
    int failed;
    unsigned int inserted_gap_periods;
    char request_id[LE_TTS_REQUEST_ID_MAX];
};

/* ---- Signal handlers ---- */

static void signal_stop(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void signal_child(int signo)
{
    (void)signo;
    g_child_exited = 1;
}

/* ---- Utility ---- */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    char *p;
    size_t n;

    if (!path || !*path)
        return -1;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);
    for (p = tmp + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int ensure_socket_parent(const char *socket_path)
{
    char parent[PATH_MAX];
    char *slash;
    size_t n;

    n = strlen(socket_path);
    if (n >= sizeof(parent))
        return -1;
    memcpy(parent, socket_path, n + 1);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return 0;
    *slash = '\0';
    return mkdir_p(parent, 0755);
}

static int listen_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;
    size_t path_len;

    if (ensure_socket_parent(path) < 0) {
        le_log_perr("ttsd: create socket directory");
        return -1;
    }
    path_len = strlen(path);
    if (path_len >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) < 0 || set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_len + 1);
    (void)unlink(path);
    if (bind(fd, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1)) < 0 ||
        listen(fd, LE_TTS_MAX_CLIENTS) < 0 || chmod(path, 0660) < 0) {
        int saved = errno;
        close(fd);
        (void)unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

/* ---- JSON helpers (same protocol as audiod) ---- */

static const char *json_value(const char *json, const char *key)
{
    char needle[96];
    const char *p;
    size_t key_len;

    key_len = strlen(key);
    if (key_len + 3 > sizeof(needle))
        return NULL;
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + key_len + 2;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            ++q;
        if (*q == ':') {
            ++q;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                ++q;
            return q;
        }
        p += key_len + 2;
    }
    return NULL;
}

static int json_long(const char *json, const char *key, long *value)
{
    const char *p = json_value(json, key);
    char *end;
    long parsed;

    if (!p)
        return -1;
    errno = 0;
    parsed = strtol(p, &end, 10);
    if (p == end || errno == ERANGE)
        return -1;
    if (*end != '\0' && *end != ',' && *end != '}' && *end != ']' &&
        *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
        return -1;
    *value = parsed;
    return 0;
}

static int json_string(const char *json, const char *key,
                       char *out, size_t out_size)
{
    const char *p = json_value(json, key);
    size_t used = 0;

    if (!p || *p != '"' || out_size == 0)
        return -1;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p || used + 1 >= out_size)
                return -1;
            switch (*p) {
            case '"': case '\\': case '/': out[used++] = *p; break;
            case 'n': out[used++] = '\n'; break;
            case 'r': out[used++] = '\r'; break;
            case 't': out[used++] = '\t'; break;
            default: return -1;
            }
        } else {
            if ((unsigned char)*p < 0x20 || used + 1 >= out_size)
                return -1;
            out[used++] = *p;
        }
        ++p;
    }
    if (*p != '"')
        return -1;
    out[used] = '\0';
    return 0;
}

static int parse_request(char *message, unsigned long *id,
                         char *command, size_t command_size)
{
    long version;
    long request_id;

    *id = 0;
    if (json_long(message, "v", &version) < 0 ||
        version != LE_ADAPTER_PROTO_VERSION ||
        json_long(message, "id", &request_id) < 0 || request_id < 0 ||
        json_string(message, "cmd", command, command_size) < 0)
        return -1;
    *id = (unsigned long)request_id;
    return 0;
}

static int response_ok(char *out, size_t out_size, unsigned long id,
                       const char *data)
{
    int n = snprintf(out, out_size,
                     "{\"v\":%d,\"id\":%lu,\"ok\":true,\"data\":%s}\n",
                     LE_ADAPTER_PROTO_VERSION, id, data);
    return n >= 0 && (size_t)n < out_size ? n : -1;
}

static int response_error(char *out, size_t out_size, unsigned long id,
                          const char *error)
{
    int n = snprintf(out, out_size,
                     "{\"v\":%d,\"id\":%lu,\"ok\":false,\"error\":\"%s\"}\n",
                     LE_ADAPTER_PROTO_VERSION, id, error);
    return n >= 0 && (size_t)n < out_size ? n : -1;
}

/* ---- Resampling: mono native-rate → stereo 48 kHz ---- */

/*
 * Linear-interpolation resampler from src_rate mono to LE_TTS_BUS_RATE
 * stereo.  Allocates and returns a buffer of interleaved stereo S16_LE.
 * Caller frees with free().
 */
static int16_t *resample_to_bus(const int16_t *src, size_t src_frames,
                                int src_rate, size_t *out_frames)
{
    double ratio;
    size_t dst_frames;
    int16_t *dst;
    size_t i;

    if (!src || src_frames == 0 || src_rate <= 0) {
        *out_frames = 0;
        return NULL;
    }
    ratio = (double)LE_TTS_BUS_RATE / (double)src_rate;
    dst_frames = (size_t)((double)src_frames * ratio) + 1;
    dst = (int16_t *)malloc(dst_frames * LE_TTS_BUS_CHANNELS * sizeof(int16_t));
    if (!dst) {
        *out_frames = 0;
        return NULL;
    }
    for (i = 0; i < dst_frames; ++i) {
        double src_pos = (double)i / ratio;
        size_t idx = (size_t)src_pos;
        double frac = src_pos - (double)idx;
        int16_t sample;

        if (idx + 1 < src_frames) {
            double a = (double)src[idx];
            double b = (double)src[idx + 1];
            sample = (int16_t)(a + frac * (b - a));
        } else {
            sample = src[idx < src_frames ? idx : src_frames - 1];
        }
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
    }
    *out_frames = dst_frames;
    return dst;
}

/* ---- Write PCM to the announcement bus FIFO ---- */

static int write_pcm_fd(int fd, const int16_t *pcm, size_t frames)
{
    size_t total_bytes = frames * LE_TTS_BUS_CHANNELS * sizeof(int16_t);
    size_t sent = 0;

    while (sent < total_bytes) {
        ssize_t n = write(fd, (const char *)pcm + sent, total_bytes - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            int rc;
            do { rc = poll(&pfd, 1, 2000); } while (rc < 0 && errno == EINTR);
            if (rc > 0)
                continue;
        }
        le_log_perr("ttsd: write to announcement bus failed");
        return -1;
    }
    return 0;
}

static int write_pcm_to_bus(const int16_t *pcm, size_t frames,
                            const char *request_id)
{
    const char *bus_path = announcement_bus_path();
    int fd;
    int rc;

    fd = open(bus_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        le_log_perr("ttsd: open announcement bus %s", bus_path);
        return -1;
    }
    mark_first_pcm(request_id);
    rc = write_pcm_fd(fd, pcm, frames);
    close(fd);
    return rc;
}

static int synthesize_bus_pcm(struct tts_engine *engine, const char *text,
                              int16_t **samples_out, size_t *frames_out)
{
    short *mono = NULL;
    size_t mono_frames = 0;
    int16_t *stereo = NULL;
    size_t stereo_frames = 0;
    int src_rate;
    int rc;

    *samples_out = NULL;
    *frames_out = 0;
    rc = tts_engine_synthesize(engine, text, &mono, &mono_frames);
    if (rc < 0 || !mono || mono_frames == 0) {
        le_log_error("ttsd: synthesis failed for text (%zu chars)",
                     strlen(text));
        return -1;
    }
    src_rate = tts_engine_sample_rate(engine);
    if (src_rate == LE_TTS_BUS_RATE) {
        /* No resampling needed — duplicate mono to stereo in place. */
        size_t i;
        stereo = (int16_t *)malloc(mono_frames * 2 * sizeof(int16_t));
        if (!stereo) {
            tts_engine_free_samples(mono);
            return -1;
        }
        for (i = 0; i < mono_frames; ++i) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        stereo_frames = mono_frames;
    } else {
        stereo = resample_to_bus(mono, mono_frames, src_rate, &stereo_frames);
        if (!stereo || stereo_frames == 0) {
            tts_engine_free_samples(mono);
            return -1;
        }
    }
    tts_engine_free_samples(mono);

    *samples_out = stereo;
    *frames_out = stereo_frames;
    return 0;
}

static double monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0.0;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static const char *first_pcm_file_path(void)
{
    const char *path = getenv("LE_TTS_FIRST_PCM_FILE");

    return path && path[0] ? path : LE_TTS_FIRST_PCM_FILE;
}

static int valid_request_id(const char *request_id)
{
    const unsigned char *position =
        (const unsigned char *)request_id;

    if (!request_id || !request_id[0])
        return 1;
    while (*position) {
        if (!((*position >= 'a' && *position <= 'z') ||
              (*position >= 'A' && *position <= 'Z') ||
              (*position >= '0' && *position <= '9') ||
              *position == '-' || *position == '_'))
            return 0;
        ++position;
    }
    return 1;
}

static void mark_first_pcm(const char *request_id)
{
    const char *path = first_pcm_file_path();
    char record[128];
    int length;
    int fd;

    if (!request_id || !request_id[0])
        return;
    length = snprintf(
        record, sizeof(record), "%s %.0f\n",
        request_id, monotonic_milliseconds());
    if (length <= 0 || length >= (int)sizeof(record))
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, record, (size_t)length) != length)
        le_log_warn("ttsd: unable to write first PCM marker");
    close(fd);
}

struct cpu_boost_state {
    char governor[LE_TTS_MAX_CPUS][32];
    unsigned char changed[LE_TTS_MAX_CPUS];
};

static int read_text_file(const char *path, char *value, size_t capacity)
{
    ssize_t count;
    int fd;

    if (capacity < 2)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    do {
        count = read(fd, value, capacity - 1);
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0)
        return -1;
    value[count] = '\0';
    while (count > 0 &&
           (value[count - 1] == '\n' || value[count - 1] == '\r' ||
            value[count - 1] == ' ' || value[count - 1] == '\t'))
        value[--count] = '\0';
    return count > 0 ? 0 : -1;
}

static int write_text_file(const char *path, const char *value)
{
    char buffer[64];
    size_t sent = 0;
    size_t length;
    int fd;
    int n;

    n = snprintf(buffer, sizeof(buffer), "%s\n", value);
    if (n <= 0 || (size_t)n >= sizeof(buffer))
        return -1;
    length = (size_t)n;
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (sent < length) {
        ssize_t written = write(fd, buffer + sent, length - sent);
        if (written > 0)
            sent += (size_t)written;
        else if (written < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);
    return sent == length ? 0 : -1;
}

static void cpu_boost_begin(struct cpu_boost_state *state)
{
    const char *enabled = getenv("LE_TTS_CPU_BOOST");
    unsigned int changed = 0;
    unsigned int cpu;

    memset(state, 0, sizeof(*state));
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    for (cpu = 0; cpu < LE_TTS_MAX_CPUS; ++cpu) {
        char path[128];

        (void)snprintf(
            path, sizeof(path),
            "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", cpu);
        if (read_text_file(path, state->governor[cpu],
                           sizeof(state->governor[cpu])) < 0)
            continue;
        if (strcmp(state->governor[cpu], "performance") == 0)
            continue;
        if (write_text_file(path, "performance") == 0) {
            state->changed[cpu] = 1;
            ++changed;
        }
    }
    if (changed > 0)
        le_log_info("ttsd: CPU boost enabled on %u core%s",
                    changed, changed == 1 ? "" : "s");
}

static void cpu_boost_end(struct cpu_boost_state *state)
{
    unsigned int restored = 0;
    unsigned int cpu;

    for (cpu = 0; cpu < LE_TTS_MAX_CPUS; ++cpu) {
        char path[128];

        if (!state->changed[cpu])
            continue;
        (void)snprintf(
            path, sizeof(path),
            "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", cpu);
        if (write_text_file(path, state->governor[cpu]) == 0)
            ++restored;
    }
    if (restored > 0)
        le_log_info("ttsd: CPU boost restored on %u core%s",
                    restored, restored == 1 ? "" : "s");
}

static void realtime_deadline(struct timespec *deadline, long milliseconds)
{
    long nanoseconds;

    (void)clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += milliseconds / 1000;
    nanoseconds = deadline->tv_nsec + (milliseconds % 1000) * 1000000L;
    deadline->tv_sec += nanoseconds / 1000000000L;
    deadline->tv_nsec = nanoseconds % 1000000000L;
}

static int stream_queue_init(struct pcm_stream_queue *queue,
                             const char *request_id)
{
    memset(queue, 0, sizeof(*queue));
    if (request_id)
        snprintf(queue->request_id, sizeof(queue->request_id),
                 "%s", request_id);
    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&queue->ready, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }
    return 0;
}

static void stream_queue_fail(struct pcm_stream_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->failed = 1;
    pthread_cond_broadcast(&queue->ready);
    pthread_mutex_unlock(&queue->mutex);
}

static int stream_queue_push(struct pcm_stream_queue *queue,
                             int16_t *samples, size_t frames)
{
    struct pcm_stream_chunk *chunk;

    chunk = (struct pcm_stream_chunk *)calloc(1, sizeof(*chunk));
    if (!chunk)
        return -1;
    chunk->samples = samples;
    chunk->frames = frames;

    pthread_mutex_lock(&queue->mutex);
    if (queue->closed || queue->failed) {
        pthread_mutex_unlock(&queue->mutex);
        free(chunk);
        return -1;
    }
    if (queue->tail)
        queue->tail->next = chunk;
    else
        queue->head = chunk;
    queue->tail = chunk;
    pthread_cond_signal(&queue->ready);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static void stream_queue_close(struct pcm_stream_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    pthread_cond_broadcast(&queue->ready);
    pthread_mutex_unlock(&queue->mutex);
}

static int stream_queue_failed(struct pcm_stream_queue *queue)
{
    int failed;

    pthread_mutex_lock(&queue->mutex);
    failed = queue->failed;
    pthread_mutex_unlock(&queue->mutex);
    return failed;
}

static void stream_queue_destroy(struct pcm_stream_queue *queue)
{
    struct pcm_stream_chunk *chunk;

    while ((chunk = queue->head) != NULL) {
        queue->head = chunk->next;
        free(chunk->samples);
        free(chunk);
    }
    pthread_cond_destroy(&queue->ready);
    pthread_mutex_destroy(&queue->mutex);
}

static void *stream_writer(void *opaque)
{
    struct pcm_stream_queue *queue = (struct pcm_stream_queue *)opaque;
    const char *bus_path = announcement_bus_path();
    static const int16_t silence[
        LE_TTS_STREAM_GAP_FRAMES * LE_TTS_BUS_CHANNELS] = {0};
    struct pcm_stream_chunk *chunk;
    unsigned int consecutive_gap_periods = 0;
    unsigned int max_gap_periods =
        (LE_TTS_STREAM_MAX_GAP_MS + LE_TTS_STREAM_GAP_WAIT_MS - 1) /
        LE_TTS_STREAM_GAP_WAIT_MS;
    int fd = -1;
    int started = 0;

    for (;;) {
        struct timespec deadline;
        int timed_out = 0;
        int closed;

        pthread_mutex_lock(&queue->mutex);
        while (!queue->head && !queue->closed && !queue->failed) {
            int wait_rc;

            if (!started) {
                wait_rc = pthread_cond_wait(&queue->ready, &queue->mutex);
            } else {
                realtime_deadline(&deadline, LE_TTS_STREAM_GAP_WAIT_MS);
                wait_rc = pthread_cond_timedwait(&queue->ready, &queue->mutex,
                                                 &deadline);
                if (wait_rc == ETIMEDOUT) {
                    timed_out = 1;
                    break;
                }
            }
            if (wait_rc != 0) {
                queue->failed = 1;
                break;
            }
        }
        chunk = queue->head;
        if (chunk) {
            queue->head = chunk->next;
            if (!queue->head)
                queue->tail = NULL;
        }
        closed = queue->closed;
        pthread_mutex_unlock(&queue->mutex);

        if (chunk) {
            if (fd < 0) {
                fd = open(bus_path, O_WRONLY | O_CLOEXEC);
                if (fd < 0) {
                    le_log_perr("ttsd: open announcement bus %s",
                                bus_path);
                    free(chunk->samples);
                    free(chunk);
                    stream_queue_fail(queue);
                    break;
                }
            }
            if (!started) {
                le_log_info("ttsd: streaming first PCM chunk to bus");
                mark_first_pcm(queue->request_id);
            }
            if (consecutive_gap_periods > 0) {
                double gap_ms =
                    (double)consecutive_gap_periods *
                    LE_TTS_STREAM_GAP_FRAMES * 1000.0 / LE_TTS_BUS_RATE;
                le_log_info("ttsd: streaming phrase-boundary pause %.1f ms",
                            gap_ms);
            }
            consecutive_gap_periods = 0;
            started = 1;
            if (write_pcm_fd(fd, chunk->samples, chunk->frames) < 0) {
                free(chunk->samples);
                free(chunk);
                stream_queue_fail(queue);
                break;
            }
            free(chunk->samples);
            free(chunk);
            continue;
        }
        if (closed || stream_queue_failed(queue))
            break;
        if (!started || !timed_out)
            continue;

        if (consecutive_gap_periods >= max_gap_periods) {
            le_log_error("ttsd: streaming gap exceeded %d ms",
                         LE_TTS_STREAM_MAX_GAP_MS);
            stream_queue_fail(queue);
            break;
        }
        if (write_pcm_fd(fd, silence, LE_TTS_STREAM_GAP_FRAMES) < 0) {
            stream_queue_fail(queue);
            break;
        }
        ++consecutive_gap_periods;
        pthread_mutex_lock(&queue->mutex);
        ++queue->inserted_gap_periods;
        pthread_mutex_unlock(&queue->mutex);
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

static void free_stream_phrases(char **phrases, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i)
        free(phrases[i]);
}

static size_t split_stream_phrases(const char *text, char **phrases,
                                   size_t capacity)
{
    static const char prefix[] = "now playing";
    const char *split = NULL;
    const char *p;
    size_t prefix_len = sizeof(prefix) - 1;
    size_t text_len = strlen(text);

    if (capacity == 0)
        return 0;
    memset(phrases, 0, capacity * sizeof(*phrases));

    if (text_len > prefix_len + 1 &&
        strncasecmp(text, prefix, prefix_len) == 0 &&
        (text[prefix_len] == ' ' || text[prefix_len] == '\t')) {
        split = text + prefix_len;
    } else {
        for (p = text + 8; *p; ++p) {
            if ((*p == ',' || *p == ';' || *p == ':') &&
                p[1] != '\0') {
                split = p + 1;
                break;
            }
        }
    }

    if (split && capacity >= 2) {
        const char *rest = split;
        size_t first_len;

        while (*rest == ' ' || *rest == '\t')
            ++rest;
        first_len = (size_t)(split - text);
        while (first_len > 0 &&
               (text[first_len - 1] == ' ' || text[first_len - 1] == '\t'))
            --first_len;
        if (first_len > 0 && *rest) {
            phrases[0] = strndup(text, first_len);
            phrases[1] = strdup(rest);
            if (phrases[0] && phrases[1])
                return 2;
            free_stream_phrases(phrases, 2);
            phrases[0] = NULL;
            phrases[1] = NULL;
        }
    }

    phrases[0] = strdup(text);
    return phrases[0] ? 1 : 0;
}

static int synthesize_and_stream(struct tts_engine *engine, const char *text,
                                 const char *request_id)
{
    struct pcm_stream_queue queue;
    char *phrases[LE_TTS_STREAM_MAX_PHRASES];
    pthread_t writer;
    size_t phrase_count;
    size_t i;
    int writer_started = 0;
    int result = -1;
    double request_ms = monotonic_milliseconds();

    phrase_count = split_stream_phrases(
        text, phrases, sizeof(phrases) / sizeof(phrases[0]));
    if (phrase_count == 0)
        return -1;
    if (stream_queue_init(&queue, request_id) < 0)
        goto out_phrases;
    if (pthread_create(&writer, NULL, stream_writer, &queue) != 0)
        goto out_queue;
    writer_started = 1;

    le_log_info("ttsd: phrase streaming enabled (%zu phrase%s)",
                phrase_count, phrase_count == 1 ? "" : "s");
    for (i = 0; i < phrase_count; ++i) {
        int16_t *samples = NULL;
        size_t frames = 0;
        double started_ms = monotonic_milliseconds();
        double finished_ms;

        if (stream_queue_failed(&queue))
            break;
        if (synthesize_bus_pcm(engine, phrases[i], &samples, &frames) < 0)
            break;
        finished_ms = monotonic_milliseconds();
        le_log_info(
            "ttsd: streaming phrase %zu/%zu synthesized in %.1f ms "
            "(%.2f s audio, request +%.1f ms)",
            i + 1, phrase_count, finished_ms - started_ms,
            (double)frames / LE_TTS_BUS_RATE, finished_ms - request_ms);
        if (stream_queue_push(&queue, samples, frames) < 0) {
            free(samples);
            break;
        }
    }

    if (i == phrase_count && !stream_queue_failed(&queue))
        result = 0;
    stream_queue_close(&queue);
    if (writer_started)
        pthread_join(writer, NULL);
    if (stream_queue_failed(&queue))
        result = -1;
    le_log_info("ttsd: phrase streaming complete (gap periods=%u, result=%d)",
                queue.inserted_gap_periods, result);

out_queue:
    stream_queue_destroy(&queue);
out_phrases:
    free_stream_phrases(phrases, phrase_count);
    return result;
}

/* ---- Synthesize + write (runs in forked child) ---- */

static int synthesize_and_play(struct tts_engine *engine, const char *text,
                               const char *request_id)
{
    struct cpu_boost_state boost;
    int16_t *stereo = NULL;
    size_t stereo_frames = 0;
    int rc;
    const char *streaming = getenv("LE_TTS_STREAMING");

    cpu_boost_begin(&boost);
    if (streaming && !strcmp(streaming, "1")) {
        rc = synthesize_and_stream(engine, text, request_id);
        cpu_boost_end(&boost);
        return rc;
    }

    if (synthesize_bus_pcm(engine, text, &stereo, &stereo_frames) < 0) {
        cpu_boost_end(&boost);
        return -1;
    }
    le_log_info("ttsd: playing %zu frames (%.2f s) on announcement bus",
                stereo_frames, (double)stereo_frames / LE_TTS_BUS_RATE);
    rc = write_pcm_to_bus(stereo, stereo_frames, request_id);
    free(stereo);
    cpu_boost_end(&boost);
    return rc;
}

/* ---- Child management ---- */

static void kill_active_child(void)
{
    pid_t child = g_active_child;
    int status;
    unsigned int attempt;

    if (child <= 0)
        return;

    /* ONNX inference may not promptly handle SIGTERM.  Do not leave a
     * second model-sized child behind when a request is cancelled. */
    (void)kill(child, SIGKILL);
    for (attempt = 0; attempt < 50; ++attempt) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD))
            break;
        if (waited < 0 && errno != EINTR)
            break;
        usleep(10000);
    }
    g_active_child = -1;
}

static void reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == g_active_child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
                le_log_warn("ttsd: speech child exited with code %d",
                            WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                le_log_info("ttsd: speech child killed by signal %d",
                            WTERMSIG(status));
            g_active_child = -1;
        }
    }
    g_child_exited = 0;
}

static int start_speech(struct tts_engine *engine, const char *text,
                        const char *request_id)
{
    pid_t pid;

    /* A forked ONNX child can duplicate a model-sized address space on
     * low-memory devices.  Keep this opt-in: in-process synthesis blocks
     * the command loop, but avoids the parent/child peak during profiling
     * and for deployments where memory is tighter than responsiveness. */
    if (getenv("LE_TTS_IN_PROCESS") &&
        !strcmp(getenv("LE_TTS_IN_PROCESS"), "1")) {
        int rc = synthesize_and_play(engine, text, request_id);
        if (rc < 0) {
            le_log_error("ttsd: in-process synthesis failed for text (%zu chars)",
                         strlen(text));
            return -1;
        }
        le_log_info("ttsd: in-process speech completed (%zu chars)",
                    strlen(text));
        return 0;
    }

    /* Kill any in-progress utterance. */
    kill_active_child();
    reap_children();

    pid = fork();
    if (pid < 0) {
        le_log_perr("ttsd: fork for speech failed");
        return -1;
    }
    if (pid == 0) {
        /* Child: synthesize and write to bus, then exit. */
        int rc = synthesize_and_play(engine, text, request_id);
        _exit(rc == 0 ? 0 : 1);
    }
    g_active_child = pid;
    le_log_info("ttsd: speech started (pid=%d, %zu chars)", pid, strlen(text));
    return 0;
}

/* ---- Client I/O ---- */

static int in_process_synthesis_enabled(void)
{
    const char *value = getenv("LE_TTS_IN_PROCESS");
    return value && !strcmp(value, "1");
}

static int queue_in_process_speech(const char *text,
                                   const char *request_id)
{
    size_t length;

    if (!text || g_pending_speech)
        return -1;
    length = strlen(text);
    if (length >= sizeof(g_pending_text))
        return -1;
    memcpy(g_pending_text, text, length + 1);
    snprintf(g_pending_request_id, sizeof(g_pending_request_id),
             "%s", request_id ? request_id : "");
    g_pending_speech = 1;
    return 0;
}

static int queue_output(struct client *client, const char *message, size_t length)
{
    size_t pending = client->output_used - client->output_sent;
    if (length > sizeof(client->output) - pending)
        return -1;
    if (client->output_sent != 0 && pending != 0)
        memmove(client->output, client->output + client->output_sent, pending);
    client->output_sent = 0;
    memcpy(client->output + pending, message, length);
    client->output_used = pending + length;
    return 0;
}

static int handle_request(struct tts_engine *engine, char *message,
                          char *response, size_t response_size)
{
    char command[64];
    char text[LE_TTS_MAX_TEXT];
    unsigned long id;

    if (parse_request(message, &id, command, sizeof(command)) < 0)
        return response_error(response, response_size, 0, "malformed request");
    le_log_debug("ttsd: cmd=\"%s\" id=%lu", command, id);

    if (!strcmp(command, "status")) {
        char data[128];
        (void)snprintf(data, sizeof(data),
                       "{\"speaking\":%s,\"engine\":\"%s\"}",
                       (g_active_child > 0 || g_pending_speech)
                           ? "true" : "false",
                       LE_TTSD_ENGINE_NAME);
        return response_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "speak")) {
        char request_id[LE_TTS_REQUEST_ID_MAX] = "";

        if (json_string(message, "text", text, sizeof(text)) < 0)
            return response_error(response, response_size, id,
                                  "missing or invalid text field");
        if (text[0] == '\0')
            return response_error(response, response_size, id,
                                  "text must not be empty");
        if (json_value(message, "request_id") &&
            (json_string(message, "request_id", request_id,
                         sizeof(request_id)) < 0 ||
             !valid_request_id(request_id)))
            return response_error(response, response_size, id,
                                  "invalid request_id");
        if (in_process_synthesis_enabled()
                ? queue_in_process_speech(text, request_id) < 0
                : start_speech(engine, text, request_id) < 0)
            return response_error(response, response_size, id,
                                  "failed to start speech");
        return response_ok(response, response_size, id, "{\"speaking\":true}");
    }
    if (!strcmp(command, "stop_speech")) {
        kill_active_child();
        return response_ok(response, response_size, id, "{\"speaking\":false}");
    }
    return response_error(response, response_size, id, "unknown command");
}

static int process_client_input(struct tts_engine *engine, struct client *client)
{
    char response[LE_ADAPTER_MSG_MAX];
    char *newline;

    for (;;) {
        newline = memchr(client->input, '\n', client->input_used);
        if (!newline)
            break;
        {
            size_t line_length = (size_t)(newline - client->input);
            size_t consumed = line_length + 1;
            int response_length;
            if (line_length >= LE_ADAPTER_MSG_MAX)
                return -1;
            client->input[line_length] = '\0';
            response_length = handle_request(engine, client->input,
                                             response, sizeof(response));
            if (response_length < 0 ||
                queue_output(client, response, (size_t)response_length) < 0)
                return -1;
            memmove(client->input, client->input + consumed,
                    client->input_used - consumed);
            client->input_used -= consumed;
        }
    }
    if (client->input_used == sizeof(client->input)) {
        client->input_used = 0;
        return -1;
    }
    return 0;
}

static int read_client(struct tts_engine *engine, struct client *client)
{
    for (;;) {
        ssize_t n;
        if (client->input_used == sizeof(client->input))
            return -1;
        n = read(client->fd, client->input + client->input_used,
                 sizeof(client->input) - client->input_used);
        if (n > 0) {
            client->input_used += (size_t)n;
            if (process_client_input(engine, client) < 0)
                return -1;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static int flush_client(struct client *client)
{
    while (client->output_sent < client->output_used) {
        ssize_t n = write(client->fd, client->output + client->output_sent,
                          client->output_used - client->output_sent);
        if (n > 0) {
            client->output_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    client->output_used = 0;
    client->output_sent = 0;
    return 0;
}

static void close_client(struct client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static int accept_client(int listen_fd, struct client clients[LE_TTS_MAX_CLIENTS])
{
    int fd;
    int i;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return 0;
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);
    for (i = 0; i < LE_TTS_MAX_CLIENTS; ++i) {
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            return 1;
        }
    }
    close(fd);
    return 0;
}

/* ---- Main ---- */

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--socket PATH] [--model-dir DIR] [--voice NAME] "
            "[--foreground] [--verbose] [--debug] [--quiet]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *socket_path = LE_TTS_DEFAULT_SOCKET;
    const char *model_dir = NULL;
    const char *voice = "zipvoice";
    int foreground = 0;
    int listen_fd;
    int i;
    struct client clients[LE_TTS_MAX_CLIENTS];
    struct tts_engine *engine;
    struct sigaction action;
    struct sigaction child_action;

    memset(clients, 0, sizeof(clients));
    for (i = 0; i < LE_TTS_MAX_CLIENTS; ++i)
        clients[i].fd = -1;

    le_log_init("ttsd", argc, argv);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--model-dir") && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (!strcmp(argv[i], "--voice") && i + 1 < argc) {
            voice = argv[++i];
        } else if (!strcmp(argv[i], "--foreground")) {
            foreground = 1;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--syslog")) {
            /* handled by le_log_init */
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    (void)foreground;
    le_log_info("ttsd: starting (socket=%s, model=%s, voice=%s)",
                socket_path, model_dir ? model_dir : "(mock)", voice);

    /* Initialize the TTS engine. */
    engine = tts_engine_init(model_dir, voice);
    if (!engine) {
        le_log_error("ttsd: failed to initialize TTS engine");
        return EXIT_FAILURE;
    }
    le_log_info("ttsd: TTS engine ready (native rate=%d Hz)",
                tts_engine_sample_rate(engine));

    /* Signal setup. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_stop;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    memset(&child_action, 0, sizeof(child_action));
    child_action.sa_handler = signal_child;
    child_action.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&child_action.sa_mask);
    (void)sigaction(SIGCHLD, &child_action, NULL);

    listen_fd = listen_socket(socket_path);
    if (listen_fd < 0) {
        le_log_perr("ttsd: listen socket %s", socket_path);
        tts_engine_destroy(engine);
        return EXIT_FAILURE;
    }
    le_log_info("ttsd: listening on %s", socket_path);

    while (!g_stop) {
        struct pollfd pollfds[1 + LE_TTS_MAX_CLIENTS];
        int poll_to_client[1 + LE_TTS_MAX_CLIENTS];
        nfds_t nfds = 1;
        int poll_result;

        if (g_child_exited)
            reap_children();

        pollfds[0].fd = listen_fd;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;
        poll_to_client[0] = -1;
        for (i = 0; i < LE_TTS_MAX_CLIENTS; ++i) {
            if (clients[i].fd < 0)
                continue;
            pollfds[nfds].fd = clients[i].fd;
            pollfds[nfds].events = POLLIN;
            if (clients[i].output_used > clients[i].output_sent)
                pollfds[nfds].events |= POLLOUT;
            pollfds[nfds].revents = 0;
            poll_to_client[nfds] = i;
            ++nfds;
        }

        poll_result = poll(pollfds, nfds, -1);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pollfds[0].revents & POLLIN)
            (void)accept_client(listen_fd, clients);
        for (i = 1; i < (int)nfds; ++i) {
            int client_index = poll_to_client[i];
            short revents = pollfds[i].revents;
            if (client_index < 0 || clients[client_index].fd < 0)
                continue;
            if (revents & (POLLERR | POLLNVAL | POLLHUP)) {
                close_client(&clients[client_index]);
                continue;
            }
            if ((revents & POLLIN) &&
                read_client(engine, &clients[client_index]) < 0) {
                close_client(&clients[client_index]);
                continue;
            }
            if ((revents & POLLOUT) &&
                flush_client(&clients[client_index]) < 0)
                close_client(&clients[client_index]);
        }
        if (g_pending_speech) {
            int output_pending = 0;
            char text[LE_TTS_MAX_TEXT];
            char request_id[LE_TTS_REQUEST_ID_MAX];

            /*
             * Low-memory deployments synthesize in this process so the
             * loaded model is not duplicated by fork.  Flush the accepted
             * response first: callers receive a queued acknowledgement
             * immediately while this event loop then performs synthesis.
             */
            for (i = 0; i < LE_TTS_MAX_CLIENTS; ++i) {
                if (clients[i].fd >= 0 &&
                    clients[i].output_used > clients[i].output_sent) {
                    output_pending = 1;
                    break;
                }
            }
            if (!output_pending) {
                memcpy(text, g_pending_text, sizeof(text));
                memcpy(request_id, g_pending_request_id,
                       sizeof(request_id));
                g_pending_text[0] = '\0';
                g_pending_request_id[0] = '\0';
                g_pending_speech = 0;
                if (start_speech(engine, text, request_id) < 0)
                    le_log_error("ttsd: queued in-process speech failed");
            }
        }
    }

    le_log_info("ttsd: shutting down");
    kill_active_child();
    reap_children();
    for (i = 0; i < LE_TTS_MAX_CLIENTS; ++i)
        close_client(&clients[i]);
    close(listen_fd);
    (void)unlink(socket_path);
    tts_engine_destroy(engine);
    return EXIT_SUCCESS;
}
