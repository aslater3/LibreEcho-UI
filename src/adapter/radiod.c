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
 *   - ICY (Shoutcast) stream metadata is read when the station sends it.
 *     Stations that do not send it have no track title and none is invented.
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
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "adapter.h"
#include "../log.h"
#include "radio_resample.h"

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
#define HEADER_MAX 8192
/*
 * ICY (Shoutcast) metadata. A station that answers "Icy-MetaData: 1" sends an
 * icy-metaint header and then interleaves the audio: every icy-metaint audio
 * bytes there is one length byte counting 16-byte units, followed by that many
 * bytes of text such as StreamTitle='Artist - Track';.
 *
 * The block has to come out of the byte stream before the audio reaches the
 * decoder whether or not anyone reads it, so parsing it is the same work as
 * skipping it. Everything here is fixed size: the length byte caps a block at
 * 255 * 16 bytes, and the text kept out of it is clamped to TITLE_MAX.
 */
#define ICY_META_UNIT 16
#define ICY_META_MAX (255 * ICY_META_UNIT)
#define TITLE_MAX 192                  /* matches LE_MEDIA_TEXT in backend.h */

static volatile sig_atomic_t running = 1;
static pid_t player_pid = -1;
static char playing_url[URL_MAX];
/*
 * What the stream said it is playing. The player runs in a forked child, so
 * the child parses and the parent answers "status"; a pipe carries one short
 * line per change. The write end is non-blocking on purpose -- a parent that
 * is not reading must never stall the audio path, so a dropped title is the
 * correct failure.
 */
static char playing_title[TITLE_MAX];
static char playing_station[TITLE_MAX];
static int meta_fd = -1;               /* parent: read end of that pipe */
static int meta_out = -1;              /* child: write end of that pipe */

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
 * Titles arrive as arbitrary network bytes. Control characters are not legal
 * inside a JSON string and invalid UTF-8 makes the whole status response
 * unreadable to the browser, so only printable ASCII and well-formed UTF-8
 * sequences survive. A station that still labels itself in Latin-1 loses its
 * accented characters rather than corrupting the response; at this size that
 * is the honest trade, not a transcoding table.
 */
static size_t utf8_length(const unsigned char *p, size_t available)
{
    size_t need, i;

    if (p[0] < 0xc2 || p[0] > 0xf4)
        return 0;
    need = p[0] < 0xe0 ? 2 : p[0] < 0xf0 ? 3 : 4;
    if (need > available)
        return 0;
    for (i = 1; i < need; ++i)
        if ((p[i] & 0xc0) != 0x80)
            return 0;
    if (need == 3 && p[0] == 0xe0 && p[1] < 0xa0)
        return 0;                            /* overlong */
    if (need == 4 && p[0] == 0xf0 && p[1] < 0x90)
        return 0;                            /* overlong */
    if (need == 4 && p[0] == 0xf4 && p[1] > 0x8f)
        return 0;                            /* above U+10FFFF */
    return need;
}

static void sanitise_text(char *out, size_t size, const char *in, size_t length)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0, used = 0, n;

    out[0] = '\0';
    while (i < length && used + 1 < size) {
        if (p[i] >= 0x20 && p[i] < 0x7f) { out[used++] = (char)p[i++]; continue; }
        if (p[i] < 0x80) { ++i; continue; }  /* control byte: dropped */
        n = utf8_length(p + i, length - i);
        if (!n || used + n + 1 > size) { ++i; continue; }
        memcpy(out + used, p + i, n);
        used += n;
        i += n;
    }
    while (used && (out[used - 1] == ' ' || out[used - 1] == '\t'))
        --used;
    out[used] = '\0';
}

/* Child side: one line per change, dropped rather than blocking. */
static void publish_metadata(char kind, const char *value)
{
    char line[TITLE_MAX + 3];
    ssize_t written;
    int n;

    if (meta_out < 0)
        return;
    n = snprintf(line, sizeof(line), "%c%s\n", kind, value);
    if (n < 0 || (size_t)n >= sizeof(line))
        return;
    written = write(meta_out, line, (size_t)n);
    (void)written;
}

/*
 * Pull the track title out of one metadata block. An empty StreamTitle is a
 * real answer -- the station is saying it no longer knows -- so it is
 * published as an empty title rather than leaving the last one on screen.
 */
static void metadata_block(const char *text, size_t length)
{
    static const char key[] = "StreamTitle='";
    const size_t key_len = sizeof(key) - 1;
    char title[TITLE_MAX];
    size_t i, start;

    for (i = 0; i + key_len <= length; ++i) {
        if (memcmp(text + i, key, key_len))
            continue;
        start = i + key_len;
        for (i = start; i < length; ++i)
            if (text[i] == '\'' && (i + 1 >= length || text[i + 1] == ';'))
                break;
        sanitise_text(title, sizeof(title), text + start, i - start);
        publish_metadata('T', title);
        return;
    }
}

/*
 * The stream reader. Audio comes out of icy_read; metadata blocks never do.
 * The block state is kept across calls because a block can straddle two reads
 * and the reader must not block waiting for the rest of one.
 */
struct icy_stream {
    int fd;
    int metaint;                    /* 0 when the server sends no metadata */
    int until_meta;                 /* audio bytes left before the next block */
    int meta_need;                  /* -1 = length byte pending, else remaining */
    size_t meta_used;
    char meta_text[ICY_META_MAX];
    unsigned char raw[NET_CHUNK];
    size_t raw_used, raw_pos;
    /*
     * A live stream has no Content-Length; a file served over HTTP does.
     * Keeping both lets the player tell "the station dropped" from "the file
     * finished", which decide opposite things about reconnecting.
     */
    long content_length;            /* 0 when the server declares none */
    long body_read;
};

static struct icy_stream stream;

/* Case-insensitive header lookup over the NUL-terminated header block. */
static int header_value(const char *headers, const char *name,
                        char *out, size_t size)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    out[0] = '\0';
    while (*line) {
        const char *end = strstr(line, "\r\n");
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (len > name_len && !strncasecmp(line, name, name_len) &&
            line[name_len] == ':') {
            const char *value = line + name_len + 1;
            size_t n;

            while (*value == ' ' || *value == '\t')
                ++value;
            n = len - (size_t)(value - line);
            while (n && (value[n - 1] == ' ' || value[n - 1] == '\t'))
                --n;
            if (n >= size)
                n = size - 1;
            memcpy(out, value, n);
            out[n] = '\0';
            return 0;
        }
        if (!end)
            break;
        line = end + 2;
    }
    return -1;
}

/*
 * Send the request and consume headers up to the blank line. Icy-MetaData is
 * asked for now; a station that ignores it sends no icy-metaint and the reader
 * passes every byte straight through, exactly as before.
 */
static int icy_open(struct icy_stream *st, int fd, const char *host,
                    const char *path, char *station, size_t station_size)
{
    char request[HOST_MAX + PATH_MAX_LEN + 128];
    char headers[HEADER_MAX], value[64], name[TITLE_MAX * 2];
    size_t used = 0, offset, leftover;
    long metaint;
    int n;

    memset(st, 0, sizeof(*st));
    st->fd = fd;
    st->meta_need = -1;
    station[0] = '\0';
    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\n"
                 "User-Agent: LibreEcho/1.0\r\nIcy-MetaData: 1\r\n"
                 "Connection: close\r\n\r\n", path, host);
    if (n < 0 || (size_t)n >= sizeof(request))
        return -1;
    if (write_all(fd, request, (size_t)n) < 0)
        return -1;

    for (;;) {
        ssize_t got;
        const char *blank;

        if (used + NET_CHUNK >= sizeof(headers))
            return -1;                       /* headers absurdly large */
        got = read(fd, headers + used, NET_CHUNK);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        used += (size_t)got;
        headers[used] = '\0';
        blank = strstr(headers, "\r\n\r\n");
        if (!blank)
            continue;
        offset = (size_t)(blank - headers) + 4;
        leftover = used - offset;
        if (leftover > sizeof(st->raw))
            return -1;                       /* cannot happen: one read's worth */
        memcpy(st->raw, headers + offset, leftover);
        st->raw_used = leftover;
        headers[offset - 2] = '\0';          /* keep the header lines only */
        break;
    }

    if (!header_value(headers, "icy-metaint", value, sizeof(value))) {
        char *end;

        metaint = strtol(value, &end, 10);
        /*
         * A metaint that is not plausible would desynchronise the audio for
         * the whole session, so an implausible one turns metadata off rather
         * than corrupting playback.
         */
        if (end != value && metaint > 0 && metaint <= 1024L * 1024L)
            st->metaint = (int)metaint;
    }
    if (!header_value(headers, "content-length", value, sizeof(value))) {
        char *end;
        long declared = strtol(value, &end, 10);

        if (end != value && declared > 0)
            st->content_length = declared;
    }
    st->body_read = 0;
    st->until_meta = st->metaint;
    if (!header_value(headers, "icy-name", name, sizeof(name)))
        sanitise_text(station, station_size, name, strlen(name));
    return 0;
}

static void icy_consume_meta(struct icy_stream *st)
{
    size_t available = st->raw_used - st->raw_pos, take;

    if (st->meta_need < 0) {
        st->meta_need = (int)st->raw[st->raw_pos++] * ICY_META_UNIT;
        st->meta_used = 0;
        if (st->meta_need)
            return;
    } else {
        take = available < (size_t)st->meta_need ? available
                                                : (size_t)st->meta_need;
        if (st->meta_used + take <= sizeof(st->meta_text)) {
            memcpy(st->meta_text + st->meta_used, st->raw + st->raw_pos, take);
            st->meta_used += take;
        }
        st->raw_pos += take;
        st->meta_need -= (int)take;
        if (st->meta_need)
            return;
        metadata_block(st->meta_text, st->meta_used);
    }
    st->meta_need = -1;
    st->until_meta = st->metaint;
}

/*
 * Fill dst with audio only. Returns what it has rather than waiting for a full
 * buffer: the decoder wants bytes promptly, and a live stream never ends.
 */
static int icy_read(struct icy_stream *st, unsigned char *dst, size_t want)
{
    size_t produced = 0, available;

    while (produced < want) {
        if (st->raw_pos == st->raw_used) {
            ssize_t got;

            if (produced)
                break;                       /* never block for a full buffer */
            got = read(st->fd, st->raw, sizeof(st->raw));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return -1;
            st->raw_used = (size_t)got;
            st->raw_pos = 0;
        }
        if (st->metaint && !st->until_meta) {
            icy_consume_meta(st);
            continue;
        }
        available = st->raw_used - st->raw_pos;
        if (st->metaint && available > (size_t)st->until_meta)
            available = (size_t)st->until_meta;
        if (available > want - produced)
            available = want - produced;
        memcpy(dst + produced, st->raw + st->raw_pos, available);
        st->raw_pos += available;
        produced += available;
        if (st->metaint)
            st->until_meta -= (int)available;
    }
    return (int)produced;
}

/*
 * Hand a decoded block to the resampler and push the result onto the bus.
 * The resampler state is per-stream and must persist across calls; see
 * radio_resample.c for why.
 */
#define RESAMPLE_MAX_OUT 8192

static struct le_radio_resampler resampler;

static int write_bus(int bus, const short *pcm, int frames, int channels,
                     int rate)
{
    static int16_t out[RESAMPLE_MAX_OUT * LE_RADIO_RESAMPLE_CHANNELS];
    int produced = le_radio_resample(&resampler, pcm, frames, channels, rate,
                                     out, RESAMPLE_MAX_OUT);

    if (produced <= 0)
        return 0;
    return write_all(bus, out, (size_t)produced *
                     LE_RADIO_RESAMPLE_CHANNELS * sizeof(int16_t));
}

static int play_stream(const char *url, const char *bus_path, long *played,
                       int *complete);

/*
 * A stream ending is not a reason to stop playing. Servers rotate, senders
 * close connections and networks hiccup; play_stream returning simply meant
 * the player exited, so the first ordinary drop ended playback silently and
 * the UI showed "not playing" with nothing to explain it.
 *
 * Retry with a widening delay, but only forgive a failure that followed real
 * audio. A URL that has never played anything is a broken station rather than
 * a dropped stream, and retrying it forever would keep reporting playback
 * while the room stayed silent. SIGTERM is back to its default action in this
 * child, so a stop still ends it immediately, including mid-delay.
 */
#define RECONNECT_TRIES 5
#define RECONNECT_DELAY_MS 1000
#define RECONNECT_DELAY_MAX_MS 8000

static void delay_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int play_with_reconnect(const char *url, const char *bus_path)
{
    unsigned delay = RECONNECT_DELAY_MS;
    int barren = 0;

    for (;;) {
        long played = 0;
        int complete = 0;
        int rc = play_stream(url, bus_path, &played, &complete);

        if (complete) {
            le_log_info("radiod: %s played to its end", url);
            return rc;
        }
        if (played > 0) {                     /* a real drop, not a bad URL */
            barren = 0;
            delay = RECONNECT_DELAY_MS;
            le_log_info("radiod: stream ended after %ld frames, reconnecting",
                        played);
        } else if (++barren >= RECONNECT_TRIES) {
            le_log_warn("radiod: no audio from %s in %d attempts, giving up",
                        url, barren);
            return rc;
        } else {
            le_log_warn("radiod: no audio from %s, attempt %d of %d",
                        url, barren, RECONNECT_TRIES);
        }
        delay_ms(delay);
        if (played <= 0 && delay < RECONNECT_DELAY_MAX_MS)
            delay *= 2;
    }
}

static int play_stream(const char *url, const char *bus_path, long *played,
                       int *complete)
{
    char host[HOST_MAX], port[16], path[PATH_MAX_LEN], station[TITLE_MAX];
    unsigned char in[IN_BUFFER];
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    size_t filled = 0;
    int net = -1, bus = -1, rc = -1;

    /*
     * A local file is played by the same decoder as a stream: open it instead
     * of a socket and hand icy_read a plain fd with no metadata interleaving.
     * The caller has already checked the path is inside the USB mount, so this
     * refuses anything that is not an absolute path and lets open() enforce
     * the rest.
     */
    if (url && url[0] == '/') {
        net = open(url, O_RDONLY | O_CLOEXEC);
        if (net < 0)
            return -1;
        memset(&stream, 0, sizeof(stream));
        stream.fd = net;
        stream.meta_need = -1;
        /* A file has a known length, so it finishes rather than dropping. */
        stream.content_length = (long)lseek(net, 0, SEEK_END);
        if (stream.content_length < 0)
            stream.content_length = 0;
        if (lseek(net, 0, SEEK_SET) < 0)
            goto done;
        stream.body_read = 0;
        publish_metadata('N', url);
    } else {
        if (split_url(url, host, sizeof(host), port, sizeof(port),
                      path, sizeof(path)) < 0)
            return -1;
        net = connect_stream(host, port);
        if (net < 0)
            return -1;
        if (icy_open(&stream, net, host, path, station, sizeof(station)) < 0)
            goto done;
        if (station[0])
            publish_metadata('N', station);
    }
    bus = open(bus_path, O_WRONLY | O_CLOEXEC);
    if (bus < 0)
        goto done;
    mp3dec_init(&decoder);
    le_radio_resample_reset(&resampler);
    for (;;) {
        int samples;

        if (filled < NET_CHUNK) {
            int got = icy_read(&stream, in + filled, sizeof(in) - filled);

            if (got <= 0)
                break;                        /* stream ended */
            stream.body_read += got;
            filled += (size_t)got;
        }
        samples = mp3dec_decode_frame(&decoder, in, (int)filled, pcm, &info);
        if (info.frame_bytes <= 0)
            break;                            /* no sync and no progress */
        memmove(in, in + info.frame_bytes, filled - (size_t)info.frame_bytes);
        filled -= (size_t)info.frame_bytes;
        if (samples > 0) {
            if (write_bus(bus, pcm, samples, info.channels, info.hz) < 0)
                break;                        /* the bus went away */
            if (played)
                ++*played;
        }
    }
    /*
     * A server that declared a length and delivered it served a file, and a
     * file that reached its end is finished -- reconnecting would replay it
     * for ever. Live streams declare no length, so they are unaffected. The
     * metaint guard keeps this to the plain case: interleaved metadata counts
     * towards Content-Length but not towards the audio bytes counted here.
     */
    if (complete && stream.metaint == 0 && stream.content_length > 0 &&
        stream.body_read >= stream.content_length)
        *complete = 1;
    rc = 0;
done:
    if (bus >= 0)
        close(bus);
    if (net >= 0)
        close(net);
    return rc;
}

/*
 * One line at a time out of the player's pipe. The parent never blocks on it:
 * the pipe is non-blocking at both ends, so a player producing nothing simply
 * leaves the last title in place.
 */
static void apply_metadata_line(const char *line, size_t length)
{
    char text[TITLE_MAX];
    size_t n;

    if (!length)
        return;
    n = length - 1;
    if (n >= sizeof(text))
        n = sizeof(text) - 1;
    memcpy(text, line + 1, n);
    text[n] = '\0';
    if (line[0] == 'T')
        snprintf(playing_title, sizeof(playing_title), "%s", text);
    else if (line[0] == 'N')
        snprintf(playing_station, sizeof(playing_station), "%s", text);
}

static void drain_metadata(void)
{
    static char pending[TITLE_MAX + 2];
    static size_t pending_used;
    char buffer[256];
    ssize_t got;
    size_t i;

    if (meta_fd < 0) {
        pending_used = 0;
        return;
    }
    for (;;) {
        got = read(meta_fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return;                          /* nothing pending, or EOF */
        for (i = 0; i < (size_t)got; ++i) {
            if (buffer[i] == '\n') {
                apply_metadata_line(pending, pending_used);
                pending_used = 0;
            } else if (pending_used < sizeof(pending)) {
                pending[pending_used++] = buffer[i];
            }
        }
    }
}

static void forget_player(void)
{
    player_pid = -1;
    playing_url[0] = '\0';
    playing_title[0] = '\0';
    playing_station[0] = '\0';
    if (meta_fd >= 0)
        close(meta_fd);
    meta_fd = -1;
}

static void stop_player(void)
{
    if (player_pid <= 0)
        return;
    kill(player_pid, SIGTERM);
    while (waitpid(player_pid, NULL, 0) < 0 && errno == EINTR)
        ;
    forget_player();
}

static void reap_player(void)
{
    pid_t done;

    drain_metadata();
    if (player_pid <= 0)
        return;
    do {
        done = waitpid(player_pid, NULL, WNOHANG);
    } while (done < 0 && errno == EINTR);
    if (done == player_pid || (done < 0 && errno == ECHILD))
        forget_player();
}

static int start_player(const char *url, const char *bus_path)
{
    int fds[2];
    pid_t child;

    stop_player();
    if (pipe(fds) < 0)
        return -1;
    child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (child == 0) {
        close(fds[0]);
        meta_out = fds[1];
        (void)fcntl(meta_out, F_SETFL, O_NONBLOCK);
        signal(SIGTERM, SIG_DFL);
        _exit(play_with_reconnect(url, bus_path) < 0 ? 1 : 0);
    }
    close(fds[1]);
    meta_fd = fds[0];
    (void)fcntl(meta_fd, F_SETFL, O_NONBLOCK);
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

/* Quote what goes into the status document; sanitise_text already removed the
   control characters, so only the two JSON escapes are left to do. */
static void escape_json(char *out, size_t size, const char *in)
{
    size_t i, j;

    for (i = 0, j = 0; in[i] && j + 2 < size; ++i) {
        if (in[i] == '"' || in[i] == '\\')
            out[j++] = '\\';
        out[j++] = in[i];
    }
    out[j] = '\0';
}

static int handle(char *message, char *response, size_t response_size,
                  const char *bus_path)
{
    char command[64], url[URL_MAX];
    unsigned long id;
    char *args = NULL;

    if (le_adapter_parse_request(message, command, sizeof(command),
                                 &args, &id) < 0)
        return le_adapter_respond_err(response, response_size, 0,
                                      "malformed request");
    reap_player();
    if (!strcmp(command, "status")) {
        char data[URL_MAX * 2 + TITLE_MAX * 4 + 96];
        char escaped_url[URL_MAX * 2], escaped_title[TITLE_MAX * 2];
        char escaped_station[TITLE_MAX * 2];

        escape_json(escaped_url, sizeof(escaped_url), playing_url);
        escape_json(escaped_title, sizeof(escaped_title), playing_title);
        escape_json(escaped_station, sizeof(escaped_station), playing_station);
        snprintf(data, sizeof(data),
                 "{\"playing\":%s,\"url\":\"%s\",\"title\":\"%s\","
                 "\"station\":\"%s\"}",
                 player_pid > 0 ? "true" : "false", escaped_url,
                 escaped_title, escaped_station);
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
        if (url[0] != '/' && strncmp(url, "http://", 7))
            return le_adapter_respond_err(response, response_size, id,
                                          "url must be http://");
        if (start_player(url, bus_path) < 0)
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
    const char *bus_path = BUS_PATH;
    int listen_fd, i;

    /* --bus names the mixer sink. It is overridable so the stream reader can
       be exercised without the audio engine, not so playback can be aimed
       somewhere else on a real device. */
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            socket_path = argv[++i];
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc)
            bus_path = argv[++i];
    }

    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = le_adapter_listen(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "radiod: cannot listen on %s\n", socket_path);
        return 1;
    }
    le_log_info("radiod: ready socket=%s (http, mp3, icy metadata)",
                socket_path);
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
            if (handle(message, response, sizeof(response), bus_path) > 0)
                (void)write_all(client, response, strlen(response));
        }
        close(client);
    }
    stop_player();
    close(listen_fd);
    return 0;
}
