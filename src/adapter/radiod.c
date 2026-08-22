/*
 * libreecho-radiod -- play an internet radio stream onto the media bus.
 *
 * Fetches an HTTP stream, decodes MP3 with the vendored minimp3, resamples to
 * the 48 kHz stereo bus and writes it to media.pcm, which the shared audio
 * engine already mixes and ducks. Nothing else on the image can decode a
 * compressed stream, which is why this exists.
 *
 * Scope of this version, stated plainly rather than discovered later:
 *   - http:// only. https:// needs a TLS library (~300 KB trimmed) and is a
 *     separate, larger decision; requests for it are refused with a clear
 *     message rather than silently failing to connect.
 *   - MP3 only. AAC-only stations will not play.
 *
 * Playback runs in a forked child so a stalled or hostile server cannot block
 * the control socket, and so "stop" is a signal rather than cooperative
 * shutdown.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "adapter.h"
#include "../log.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "../../third-party/minimp3/minimp3.h"

#define BUS_PATH "/run/libreecho-audio/media.pcm"
#define BUS_RATE 48000
#define BUS_CHANNELS 2
#define NET_CHUNK 4096
#define IN_BUFFER (NET_CHUNK * 8)
#define URL_MAX 512
#define HOST_MAX 256
#define PATH_MAX_LEN 384

static volatile sig_atomic_t running = 1;
static pid_t player_pid = -1;
static char playing_url[URL_MAX];

static void stop_signal(int signum) { (void)signum; running = 0; }

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

/*
 * Split http://host[:port]/path. Rejects anything else, including https, so
 * the caller gets a reason instead of a connection that never succeeds.
 */
static int split_url(const char *url, char *host, size_t host_size,
                     char *port, size_t port_size,
                     char *path, size_t path_size)
{
    const char *rest, *colon, *slash;
    size_t host_len;

    if (!url || strncmp(url, "http://", 7))
        return -1;
    rest = url + 7;
    slash = strchr(rest, '/');
    colon = memchr(rest, ':', slash ? (size_t)(slash - rest) : strlen(rest));
    host_len = colon ? (size_t)(colon - rest)
                     : (slash ? (size_t)(slash - rest) : strlen(rest));
    if (!host_len || host_len >= host_size)
        return -1;
    memcpy(host, rest, host_len);
    host[host_len] = '\0';
    if (colon) {
        size_t n = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (!n || n >= port_size)
            return -1;
        memcpy(port, colon + 1, n);
        port[n] = '\0';
    } else {
        snprintf(port, port_size, "80");
    }
    if (snprintf(path, path_size, "%s", slash ? slash : "/")
            >= (int)path_size)
        return -1;
    return 0;
}

static int connect_stream(const char *host, const char *port)
{
    struct addrinfo hints, *list = NULL, *ai;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list) != 0)
        return -1;
    for (ai = list; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}

/*
 * Send the request and consume headers up to the blank line, returning any
 * body bytes already read. Icy-MetaData is deliberately not requested: the
 * metadata is interleaved into the audio and would have to be stripped, and
 * nothing here displays it yet.
 */
static int send_request_skip_headers(int fd, const char *host,
                                     const char *path,
                                     unsigned char *spill, size_t spill_size,
                                     size_t *spill_used)
{
    char request[HOST_MAX + PATH_MAX_LEN + 128];
    unsigned char buffer[NET_CHUNK];
    size_t used = 0;
    int n;

    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\n"
                 "User-Agent: LibreEcho/1.0\r\nIcy-MetaData: 0\r\n"
                 "Connection: close\r\n\r\n", path, host);
    if (n < 0 || (size_t)n >= sizeof(request))
        return -1;
    if (write_all(fd, request, (size_t)n) < 0)
        return -1;

    *spill_used = 0;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        unsigned char *blank;

        if (got <= 0)
            return -1;
        if (used + (size_t)got > spill_size)
            return -1;                       /* headers absurdly large */
        memcpy(spill + used, buffer, (size_t)got);
        used += (size_t)got;
        blank = (unsigned char *)memmem(spill, used, "\r\n\r\n", 4);
        if (blank) {
            size_t offset = (size_t)(blank - spill) + 4;
            *spill_used = used - offset;
            memmove(spill, spill + offset, *spill_used);
            return 0;
        }
    }
}

/* Linear resample + channel fold onto the 48 kHz stereo bus. */
static int write_bus(int bus, const short *pcm, int frames, int channels,
                     int rate)
{
    static int16_t out[8192 * BUS_CHANNELS];
    double ratio = (double)BUS_RATE / (double)rate;
    int out_frames = (int)(frames * ratio), i;

    if (out_frames <= 0)
        return 0;
    if (out_frames > 8192)
        out_frames = 8192;
    for (i = 0; i < out_frames; ++i) {
        double position = (double)i / ratio;
        int index = (int)position;
        double fraction = position - index;
        int a = index < frames ? index : frames - 1;
        int b = index + 1 < frames ? index + 1 : frames - 1;
        int left, right;

        if (channels >= 2) {
            left = (int)(pcm[a * 2] + (pcm[b * 2] - pcm[a * 2]) * fraction);
            right = (int)(pcm[a * 2 + 1] +
                          (pcm[b * 2 + 1] - pcm[a * 2 + 1]) * fraction);
        } else {
            left = right = (int)(pcm[a] + (pcm[b] - pcm[a]) * fraction);
        }
        out[i * 2] = (int16_t)(left > 32767 ? 32767 : left < -32768 ? -32768 : left);
        out[i * 2 + 1] = (int16_t)(right > 32767 ? 32767 : right < -32768 ? -32768 : right);
    }
    return write_all(bus, out, (size_t)out_frames * BUS_CHANNELS * sizeof(int16_t));
}

static int play_stream(const char *url)
{
    char host[HOST_MAX], port[16], path[PATH_MAX_LEN];
    unsigned char in[IN_BUFFER];
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    size_t filled = 0;
    int net = -1, bus = -1, rc = -1;

    if (split_url(url, host, sizeof(host), port, sizeof(port),
                  path, sizeof(path)) < 0)
        return -1;
    net = connect_stream(host, port);
    if (net < 0)
        return -1;
    if (send_request_skip_headers(net, host, path, in, sizeof(in), &filled) < 0)
        goto done;
    bus = open(BUS_PATH, O_WRONLY | O_CLOEXEC);
    if (bus < 0)
        goto done;
    mp3dec_init(&decoder);
    for (;;) {
        int samples;

        if (filled < NET_CHUNK) {
            ssize_t got = read(net, in + filled, sizeof(in) - filled);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;                        /* stream ended */
            filled += (size_t)got;
        }
        samples = mp3dec_decode_frame(&decoder, in, (int)filled, pcm, &info);
        if (info.frame_bytes <= 0)
            break;                            /* no sync and no progress */
        memmove(in, in + info.frame_bytes, filled - (size_t)info.frame_bytes);
        filled -= (size_t)info.frame_bytes;
        if (samples > 0 &&
            write_bus(bus, pcm, samples, info.channels, info.hz) < 0)
            break;                            /* the bus went away */
    }
    rc = 0;
done:
    if (bus >= 0)
        close(bus);
    if (net >= 0)
        close(net);
    return rc;
}

static void stop_player(void)
{
    if (player_pid <= 0)
        return;
    kill(player_pid, SIGTERM);
    while (waitpid(player_pid, NULL, 0) < 0 && errno == EINTR)
        ;
    player_pid = -1;
    playing_url[0] = '\0';
}

static void reap_player(void)
{
    pid_t done;

    if (player_pid <= 0)
        return;
    do {
        done = waitpid(player_pid, NULL, WNOHANG);
    } while (done < 0 && errno == EINTR);
    if (done == player_pid || (done < 0 && errno == ECHILD)) {
        player_pid = -1;
        playing_url[0] = '\0';
    }
}

static int start_player(const char *url)
{
    pid_t child;

    stop_player();
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        _exit(play_stream(url) < 0 ? 1 : 0);
    }
    player_pid = child;
    snprintf(playing_url, sizeof(playing_url), "%s", url);
    return 0;
}

static int json_string_field(const char *msg, const char *key,
                             char *out, size_t size)
{
    const char *p = strstr(msg, key);
    const char *q;
    size_t n;

    if (!p)
        return -1;
    p = strchr(p + strlen(key), '"');
    if (!p)
        return -1;
    q = strchr(++p, '"');
    if (!q)
        return -1;
    n = (size_t)(q - p);
    if (n >= size)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int handle(char *message, char *response, size_t response_size)
{
    char command[64], url[URL_MAX], escaped[URL_MAX * 2];
    unsigned long id;
    char *args = NULL;
    size_t i, j;

    if (le_adapter_parse_request(message, command, sizeof(command),
                                 &args, &id) < 0)
        return le_adapter_respond_err(response, response_size, 0,
                                      "malformed request");
    reap_player();
    if (!strcmp(command, "status")) {
        char data[URL_MAX * 2 + 64];
        for (i = 0, j = 0; playing_url[i] && j + 2 < sizeof(escaped); ++i) {
            if (playing_url[i] == '"' || playing_url[i] == '\\')
                escaped[j++] = '\\';
            escaped[j++] = playing_url[i];
        }
        escaped[j] = '\0';
        snprintf(data, sizeof(data), "{\"playing\":%s,\"url\":\"%s\"}",
                 player_pid > 0 ? "true" : "false", escaped);
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "play")) {
        if (json_string_field(args ? args : message, "\"url\"",
                              url, sizeof(url)) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "url is required");
        if (!strncmp(url, "https://", 8))
            return le_adapter_respond_err(response, response_size, id,
                "https streams need a TLS library that is not on this image; "
                "use an http:// URL for this station");
        if (strncmp(url, "http://", 7))
            return le_adapter_respond_err(response, response_size, id,
                                          "url must be http://");
        if (start_player(url) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "playback could not start");
        le_log_info("radiod: playing %s", url);
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "stop")) {
        stop_player();
        le_log_info("radiod: stopped");
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    return le_adapter_respond_err(response, response_size, id,
                                  "unknown command");
}

int main(int argc, char **argv)
{
    const char *socket_path = "/run/libreecho/radio.sock";
    int listen_fd, i;

    for (i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            socket_path = argv[++i];

    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = le_adapter_listen(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "radiod: cannot listen on %s\n", socket_path);
        return 1;
    }
    le_log_info("radiod: ready socket=%s (http, mp3)", socket_path);
    while (running) {
        char message[LE_ADAPTER_MSG_MAX], response[LE_ADAPTER_MSG_MAX];
        ssize_t n;
        int client = le_adapter_accept(listen_fd);

        if (client < 0) {
            reap_player();
            continue;
        }
        n = read(client, message, sizeof(message) - 1);
        if (n > 0) {
            message[n] = '\0';
            if (handle(message, response, sizeof(response)) > 0)
                (void)write_all(client, response, strlen(response));
        }
        close(client);
    }
    stop_player();
    close(listen_fd);
    return 0;
}
