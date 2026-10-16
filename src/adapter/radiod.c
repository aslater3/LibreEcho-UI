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
 *   - MP3, AAC (ADTS) and HLS. HLS is MPEG-TS carrying AAC-LC or HE-AAC v1
 *     (what BBC radio serves); fMP4, encrypted HLS and HE-AAC v2 are refused.
 *   - ICY (Shoutcast) stream metadata is read when the station sends it.
 *     Stations that do not send it have no track title and none is invented.
 *
 * Playback runs in a forked child so a stalled or hostile server cannot block
 * the control socket, and so "stop" is a signal rather than cooperative
 * shutdown.
 */
#include <ctype.h>
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
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "adapter.h"
#include "../json.h"
#include "../log.h"
#include "../tls.h"
#include "radio_aac.h"
#include "radio_hls.h"
#include "radio_opus.h"
#include "radio_resample.h"
#include "radio_ts.h"

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
static int player_paused;
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

static void install_stop_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

static int player_parent_guard(void)
{
#ifdef __linux__
    pid_t parent = getppid();

    /* SIGTERM stays pending in a SIGSTOP-paused child; SIGKILL cannot. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent)
        return -1;
#endif
    return 0;
}

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
                     char *path, size_t path_size, int *is_https)
{
    const char *rest, *colon, *slash;
    size_t host_len;
    int secure = 0;

    if (!url)
        return -1;
    if (!strncmp(url, "https://", 8)) {
        rest = url + 8;
        secure = 1;
    } else if (!strncmp(url, "http://", 7)) {
        rest = url + 7;
        secure = 0;
    } else {
        return -1;
    }
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
        snprintf(port, port_size, "%s", secure ? "443" : "80");
    }
    if (snprintf(path, path_size, "%s", slash ? slash : "/")
            >= (int)path_size)
        return -1;
    if (is_https)
        *is_https = secure;
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
    int playlist_type;              /* Content-Type says HLS playlist */
    struct le_tls *tls;             /* NULL for plain http */
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
static char redirect_url[URL_MAX];      /* set by icy_open on an HLS 3xx */

static int icy_open(struct icy_stream *st, int fd, const char *host,
                    const char *path, char *station, size_t station_size,
                    int secure, int hls)
{
    char request[HOST_MAX + PATH_MAX_LEN + 128];
    char headers[HEADER_MAX], value[64], name[TITLE_MAX * 2];
    size_t used = 0, offset, leftover;
    long metaint;
    int n;

    memset(st, 0, sizeof(*st));
    st->fd = fd;
    st->meta_need = -1;
    if (secure) {
        st->tls = le_tls_client_open(fd, host);
        if (!st->tls)
            return -1;
    }
    station[0] = '\0';
    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\n"
                 "User-Agent: LibreEcho/1.0\r\n%s"
                 "Connection: close\r\n\r\n", path, host,
                 hls ? "" : "Icy-MetaData: 1\r\n");
    if (n < 0 || (size_t)n >= sizeof(request))
        return -1;
    if (st->tls ? le_tls_write(st->tls, request, (size_t)n) != n
                : write_all(fd, request, (size_t)n) < 0)
        return -1;

    for (;;) {
        ssize_t got;
        const char *blank;

        if (used + NET_CHUNK >= sizeof(headers))
            return -1;                       /* headers absurdly large */
        got = st->tls ? le_tls_read(st->tls, headers + used, NET_CHUNK)
                      : read(fd, headers + used, NET_CHUNK);
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
        {
            int status = 200;
            char location[URL_MAX];
            if (!strncmp(headers, "HTTP/", 5) &&
                sscanf(headers, "%*[^ ] %d", &status) == 1) {
                if (hls && status >= 300 && status < 400) {
                    redirect_url[0] = '\0';
                    if (!header_value(headers, "location", location,
                                      sizeof(location)))
                        snprintf(redirect_url, sizeof(redirect_url), "%s",
                                 location);
                    return -2;           /* caller follows the redirect */
                }
                if (status >= 300 && status < 400) {
                    if (!header_value(headers, "location", location,
                                      sizeof(location)))
                        le_log_warn("radiod: redirect rejected: %s", location);
                    else
                        le_log_warn("radiod: redirect response rejected");
                    return -1;
                }
                if (status < 200 || status >= 300)
                    return -1;
            }
        }
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
    if (!header_value(headers, "content-type", value, sizeof(value))) {
        size_t k;

        for (k = 0; value[k]; ++k)
            value[k] = (char)tolower((unsigned char)value[k]);
        st->playlist_type = strstr(value, "mpegurl") != NULL;
    }
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
            got = st->tls ? le_tls_read(st->tls, st->raw, sizeof(st->raw))
                          : read(st->fd, st->raw, sizeof(st->raw));
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


/*
 * ---- HLS ---------------------------------------------------------------
 *
 * An HLS station is a playlist plus a rolling window of MPEG-TS segments. The
 * player child forks a fetcher that walks the playlist, downloads segments and
 * demuxes them to a plain ADTS byte stream on a private pipe; the player child
 * then decodes that pipe exactly as it decodes an Icecast stream. The pipe is
 * what absorbs the gaps between bursty segment downloads, so the shared audio
 * bus keeps the continuous-writer contract it has today.
 */
#define HLS_PLAYLIST_MAX (64 * 1024)
#define HLS_REDIRECTS 3
#define HLS_STALL_REFRESHES 6
#define HLS_SEGMENT_FAILS 5
#define HLS_PIPE_BYTES (256 * 1024)

static int hls_mode;                    /* player child is decoding an HLS pipe */
static void delay_ms(unsigned ms);

static void http_close(int fd)
{
    if (stream.tls) {
        le_tls_close(stream.tls);
        stream.tls = NULL;
    }
    if (fd >= 0)
        close(fd);
}

/*
 * GET url, following up to HLS_REDIRECTS redirects. On success the response
 * headers are consumed, the body is readable through icy_read(&stream) and the
 * socket is returned; cur receives the final URL (the base for relative refs).
 */
static int hls_http_open(const char *url, char *cur, size_t cur_size)
{
    char host[HOST_MAX], port[16], path[PATH_MAX_LEN], station[TITLE_MAX];
    char next[URL_MAX];
    int hop;

    snprintf(cur, cur_size, "%s", url);
    for (hop = 0; hop <= HLS_REDIRECTS; ++hop) {
        int secure = 0, net, rc;

        if (split_url(cur, host, sizeof(host), port, sizeof(port), path,
                      sizeof(path), &secure) < 0)
            return -1;
        net = connect_stream(host, port);
        if (net < 0)
            return -1;
        rc = icy_open(&stream, net, host, path, station, sizeof(station),
                      secure, 1);
        if (rc == 0)
            return net;
        http_close(net);
        if (rc != -2 || !redirect_url[0] ||
            le_hls_resolve(cur, redirect_url, next, sizeof(next)) < 0)
            return -1;
        snprintf(cur, cur_size, "%s", next);
    }
    return -1;
}

/* Read a whole (small) body into buf; returns its length or -1. */
static int hls_fetch_text(const char *url, char *cur, size_t cur_size,
                          char *buf, size_t cap)
{
    size_t used = 0;
    int net = hls_http_open(url, cur, cur_size);

    if (net < 0)
        return -1;
    for (;;) {
        int got;

        if (used == cap) {
            http_close(net);
            return -1;                   /* playlist larger than the cap */
        }
        got = icy_read(&stream, (unsigned char *)buf + used,
                       cap - used > NET_CHUNK ? NET_CHUNK : cap - used);
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    http_close(net);
    return (int)used;
}

/* Stream one segment through the demuxer into the pipe. 0 ok, -1 failure,
 * -2 the decoder side went away. */
static int hls_fetch_segment(const char *url, int out_fd, struct le_radio_ts *ts)
{
    static unsigned char in[NET_CHUNK];
    static unsigned char adts[NET_CHUNK + 2 * LE_RADIO_TS_PACKET];
    char cur[URL_MAX];
    int net = hls_http_open(url, cur, sizeof(cur));
    int rc = 0;

    if (net < 0)
        return -1;
    for (;;) {
        size_t pos = 0;
        int got = icy_read(&stream, in, sizeof(in));

        if (got < 0)
            break;
        if (got == 0)
            continue;
        while (pos < (size_t)got) {
            size_t taken = 0;
            int made = le_radio_ts_feed(ts, in + pos, (size_t)got - pos, adts,
                                        sizeof(adts), &taken);

            if (made < 0) {
                rc = -1;
                goto out;
            }
            if (made > 0 && write_all(out_fd, adts, (size_t)made) < 0) {
                rc = -2;
                goto out;
            }
            if (!taken)
                break;
            pos += taken;
        }
    }
out:
    http_close(net);
    return rc;
}

/* The fetcher process. Returns its exit status. */
static int hls_fetcher(const char *url, int out_fd)
{
    static char text[HLS_PLAYLIST_MAX];
    static struct le_hls_playlist pl;
    struct le_radio_ts ts;
    char media_url[URL_MAX], cur[URL_MAX];
    long long next_seq = -1;
    int stalled = 0, failures = 0, n;

    le_radio_ts_reset(&ts);
    snprintf(media_url, sizeof(media_url), "%s", url);
    n = hls_fetch_text(media_url, cur, sizeof(cur), text, sizeof(text));
    if (n < 0 || le_hls_parse(text, (size_t)n, cur, &pl) < 0)
        return 1;
    if (pl.is_master) {
        snprintf(media_url, sizeof(media_url), "%s", pl.variant);
        n = hls_fetch_text(media_url, cur, sizeof(cur), text, sizeof(text));
        if (n < 0 || le_hls_parse(text, (size_t)n, cur, &pl) < 0 ||
            pl.is_master)
            return 1;
    }
    snprintf(media_url, sizeof(media_url), "%s", cur);   /* after redirects */
    for (;;) {
        int i, fetched = 0;

        if (!pl.count)
            return 1;
        if (next_seq < 0) {                /* live edge minus ~3 segments */
            i = pl.count > 3 ? pl.count - 3 : 0;
            next_seq = pl.seq[i];
        } else if (next_seq < pl.seq[0]) {
            le_log_warn("radiod: hls fell behind, skipping to %lld",
                        (long long)pl.seq[0]);
            next_seq = pl.seq[0];
        }
        for (i = 0; i < pl.count; ++i) {
            int rc;

            if (pl.seq[i] < next_seq)
                continue;
            rc = hls_fetch_segment(pl.uri[i], out_fd, &ts);
            if (rc == -2)
                return 0;                  /* decoder gone */
            next_seq = pl.seq[i] + 1;
            if (rc < 0) {
                le_log_warn("radiod: hls segment %lld failed",
                            (long long)pl.seq[i]);
                if (++failures >= HLS_SEGMENT_FAILS)
                    return 1;
                continue;
            }
            failures = 0;
            ++fetched;
        }
        if (pl.endlist)
            return 0;                      /* finished VOD */
        if (fetched)
            stalled = 0;
        else if (++stalled >= HLS_STALL_REFRESHES)
            return 1;                      /* stream stopped moving */
        delay_ms(1000u * (unsigned)(pl.target_duration > 2
                                        ? pl.target_duration / 2 : 1));
        n = hls_fetch_text(media_url, cur, sizeof(cur), text, sizeof(text));
        if (n < 0 || le_hls_parse(text, (size_t)n, cur, &pl) < 0) {
            if (++failures >= HLS_SEGMENT_FAILS)
                return 1;
            continue;
        }
    }
}

static int is_hls_url(const char *url)
{
    size_t n = strcspn(url, "?#");

    return n >= 5 && !strncasecmp(url + n - 5, ".m3u8", 5);
}

static int play_stream(const char *url, const char *bus_path, long *played,
                       int *complete);
static int play_hls_stream(const char *url, const char *bus_path, long *played,
                           int *complete);

static int open_usb_file(const char *url)
{
    static const char root[] = "/run/libreecho/usb";
    char copy[URL_MAX], *save, *part;
    int dirfd, next;
    size_t root_len = sizeof(root) - 1;
    if (!url || strncmp(url, root, root_len) || url[root_len] != '/' ||
        strlen(url + root_len + 1) >= sizeof(copy))
        return -1;
    snprintf(copy, sizeof(copy), "%s", url + root_len + 1);
    dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0)
        return -1;
    part = strtok_r(copy, "/", &save);
    while (part) {
        char *next_part = strtok_r(NULL, "/", &save);
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(dirfd);
            return -1;
        }
        next = openat(dirfd, part, next_part ?
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW :
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            close(dirfd);
            return -1;
        }
        close(dirfd);
        if (!next_part)
            return next;
        dirfd = next;
        part = next_part;
    }
    close(dirfd);
    return -1;
}

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

/*
 * One decode attempt over the fixed input buffer. Returns the number of PCM
 * samples decoded; zero when the decoder rejected a complete frame (it is
 * then removed from the buffer); -1 when the buffered bytes are an
 * incomplete frame and more input is required (nothing is removed); -2 when
 * no frame can be assembled from what is buffered. Incomplete trailing data
 * must never be discarded: a server that delivers writes smaller than one
 * frame would otherwise lose every read before the frame completed and
 * produce no audio at all.
 *
 * The decoder state is snapshotted around the attempt: a failed probe clears
 * the library's bit reservoir internally, and the frame that finally
 * completes must still see the reservoir its main_data_begin refers to,
 * which is what the previous successful decode left behind.
 */
static int mp3_take_frame(mp3dec_t *decoder, unsigned char *in, size_t *filled,
                          size_t capacity, short *pcm,
                          mp3dec_frame_info_t *info)
{
    mp3dec_t snapshot = *decoder;
    int samples;
    size_t consumed;

    if (!*filled)
        return -1;
    samples = mp3dec_decode_frame(decoder, in, (int)*filled, pcm, info);
    if (samples > 0) {
        consumed = (size_t)info->frame_bytes;
        if (consumed > *filled)
            consumed = *filled;
        memmove(in, in + consumed, *filled - consumed);
        *filled -= consumed;
        return samples;
    }
    consumed = info->frame_bytes > 0 ? (size_t)info->frame_bytes : 0;
    if (consumed && consumed < *filled) {
        /* A complete frame the decoder rejected: drop it and carry on. */
        memmove(in, in + consumed, *filled - consumed);
        *filled -= consumed;
        return 0;
    }
    /*
     * Incomplete frame at the head: keep it and ask for more input. The
     * snapshot is restored so probing cannot damage the reservoir that the
     * completed frame will need.
     */
    *decoder = snapshot;
    return *filled >= capacity ? -2 : -1;
}

/*
 * Decode whatever arrives from the source onto the bus until it ends. The
 * source is the icy stream, or the read end of the HLS pipe when pipe_fd >= 0.
 * The codec is chosen once from the first bytes: an ADTS sync word means AAC,
 * anything else is handed to minimp3 exactly as before.
 */
static void pump_audio(int bus, int pipe_fd, long *played)
{
    unsigned char in[IN_BUFFER];
    short pcm[LE_RADIO_AAC_MAX_SAMPLES > MINIMP3_MAX_SAMPLES_PER_FRAME
                  ? LE_RADIO_AAC_MAX_SAMPLES : MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    size_t filled = 0;
    int codec = -1;                          /* -1 undecided, 0 mp3, 1 aac */

    mp3dec_init(&decoder);
    le_radio_resample_reset(&resampler);
    for (;;) {
        int samples, frames, channels = 2, rate = BUS_RATE;

        if (codec < 0 && filled >= 4) {
            codec = le_radio_aac_is_adts(in, filled);
            if (codec && le_radio_aac_open() < 0)
                break;                       /* decoder state unavailable */
        }
        if (codec < 0) {
            samples = -1;                    /* need bytes to choose */
        } else if (codec) {
            samples = le_radio_aac_take_frame(in, &filled, sizeof(in), pcm,
                                              &channels, &rate);
        } else {
            samples = mp3_take_frame(&decoder, in, &filled, sizeof(in), pcm,
                                     &info);
            channels = info.channels;
            rate = info.hz;
        }
        if (samples > 0) {
            frames = codec ? samples / (channels > 0 ? channels : 1) : samples;
            if (codec && rate == BUS_RATE && channels == BUS_CHANNELS) {
                if (write_all(bus, pcm, (size_t)samples * sizeof(short)) < 0)
                    break;                   /* the bus went away */
            } else {
                int off = 0, bad = 0;

                /* le_radio_resample takes at most 1152 frames per call. */
                while (off < frames && !bad) {
                    int chunk = frames - off > 1152 ? 1152 : frames - off;

                    bad = write_bus(bus, pcm + (size_t)off * (size_t)channels,
                                    chunk, channels, rate) < 0;
                    off += chunk;
                }
                if (bad)
                    break;
            }
            if (played)
                ++*played;
            continue;
        }
        if (samples == -2)
            break;                           /* no frame and no room to grow */
        if (samples == 0)
            continue;                        /* rejected frame dropped */
        {
            size_t want = sizeof(in) - filled;
            int got;

            if (want > NET_CHUNK)
                want = NET_CHUNK;
            if (pipe_fd >= 0) {
                ssize_t r;

                do
                    r = read(pipe_fd, in + filled, want);
                while (r < 0 && errno == EINTR);
                got = r < 0 ? -1 : (int)r;
            } else {
                got = icy_read(&stream, in + filled, want);
            }
            if (got <= 0)
                break;                       /* stream ended */
            if (pipe_fd < 0)
                stream.body_read += got;
            filled += (size_t)got;
        }
    }
    if (codec == 1)
        le_radio_aac_close();
}

static int play_hls_stream(const char *url, const char *bus_path, long *played,
                           int *complete)
{
    int fds[2], bus, status = 0;
    pid_t fetcher;

    if (pipe(fds) < 0)
        return -1;
#ifdef F_SETPIPE_SZ
    (void)fcntl(fds[1], F_SETPIPE_SZ, HLS_PIPE_BYTES);
#endif
    bus = open(bus_path, O_WRONLY | O_CLOEXEC);
    if (bus < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    fetcher = fork();
    if (fetcher < 0) {
        close(fds[0]);
        close(fds[1]);
        close(bus);
        return -1;
    }
    if (fetcher == 0) {
        close(fds[0]);
        close(bus);
        if (player_parent_guard() < 0)
            _exit(1);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        memset(&stream, 0, sizeof(stream));
        _exit(hls_fetcher(url, fds[1]));
    }
    close(fds[1]);
    memset(&stream, 0, sizeof(stream));
    hls_mode = 1;
    pump_audio(bus, fds[0], played);
    hls_mode = 0;
    close(fds[0]);                           /* EPIPE ends a live fetcher */
    kill(fetcher, SIGTERM);
    while (waitpid(fetcher, &status, 0) < 0 && errno == EINTR)
        ;
    close(bus);
    /* Only ENDLIST with every segment delivered exits 0: a finished VOD. */
    if (complete && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        played && *played > 0)
        *complete = 1;
    return 0;
}

static int play_stream(const char *url, const char *bus_path, long *played,
                       int *complete)
{
    char host[HOST_MAX], port[16], path[PATH_MAX_LEN], station[TITLE_MAX];
    int net = -1, bus = -1, rc = -1;
    int is_ogg = 0;

    /*
     * A local file is played by the same decoder as a stream: open it instead
     * of a socket and hand icy_read a plain fd with no metadata interleaving.
     * The caller has already checked the path is inside the USB mount, so this
     * refuses anything that is not an absolute path and lets open() enforce
     * the rest.
     */
    if (url && url[0] != '/' && !hls_mode && is_hls_url(url))
        return play_hls_stream(url, bus_path, played, complete);
    if (url && url[0] == '/') {
        unsigned char magic[4];
        ssize_t got;

        net = open_usb_file(url);
        if (net < 0)
            return -1;
        memset(&stream, 0, sizeof(stream));
        stream.fd = net;
        stream.tls = NULL;
        stream.meta_need = -1;
        /* A file has a known length, so it finishes rather than dropping. */
        stream.content_length = (long)lseek(net, 0, SEEK_END);
        if (stream.content_length < 0)
            stream.content_length = 0;
        if (lseek(net, 0, SEEK_SET) < 0)
            goto done;
        /*
         * Sniff the container before choosing a decoder: Ogg Opus and MP3 do
         * not share one. The four-byte Ogg capture pattern routes the file,
         * and libopusfile then confirms it really is Opus (OpusHead) rather
         * than, say, Ogg Vorbis.
         */
        got = read(net, magic, sizeof(magic));
        if (got < 0)
            goto done;
        is_ogg = le_radio_opus_is_ogg(magic, (size_t)got);
        if (lseek(net, 0, SEEK_SET) < 0)
            goto done;
        stream.body_read = 0;
        publish_metadata('N', url);
    } else {
        int secure = 0;

        if (split_url(url, host, sizeof(host), port, sizeof(port),
                      path, sizeof(path), &secure) < 0)
            return -1;
        net = connect_stream(host, port);
        if (net < 0)
            return -1;
        if (icy_open(&stream, net, host, path, station, sizeof(station),
                     secure, 0) < 0)
            goto done;
        if (stream.playlist_type) {
            /* An .m3u8 served from a URL that does not look like one. */
            if (stream.tls) {
                le_tls_close(stream.tls);
                stream.tls = NULL;
            }
            close(net);
            net = -1;
            return play_hls_stream(url, bus_path, played, complete);
        }
        if (secure)
            le_log_info("radiod: %s over TLS (peer not verified: no CA store "
                        "on this image)", host);
        if (station[0])
            publish_metadata('N', station);
    }
    bus = open(bus_path, O_WRONLY | O_CLOEXEC);
    if (bus < 0)
        goto done;
    if (is_ogg) {
        /*
         * Ogg Opus is natively 48 kHz stereo, exactly the media bus rate, so
         * the decoded block is written with no resampler. A local file is
         * finite: however the decode ends -- clean EOF, truncation, or no
         * decoder in this build -- it is complete, because reconnecting would
         * just replay the file for ever.
         */
#ifdef LE_RADIOD_ENABLE_OPUS
        int opus_rc = le_radio_opus_play_fd(bus, net, played, complete);

        if (opus_rc == LE_RADIO_OPUS_NOT_OPUS)
            le_log_warn("radiod: %s is Ogg but not a decodable Opus stream",
                        url);
        else if (opus_rc == LE_RADIO_OPUS_BUS)
            le_log_warn("radiod: media bus closed during Opus playback of %s",
                        url);
        rc = opus_rc == LE_RADIO_OPUS_OK ? 0 : -1;
#else
        le_log_warn("radiod: %s is an Ogg Opus file but this image was built "
                    "without an Opus decoder", url);
        rc = -1;
#endif
        if (complete)
            *complete = 1;
        goto done;
    }
    pump_audio(bus, -1, played);
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
    if (stream.tls) {
        le_tls_close(stream.tls);
        stream.tls = NULL;
    }
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
    player_paused = 0;
    playing_url[0] = '\0';
    playing_title[0] = '\0';
    playing_station[0] = '\0';
    if (meta_fd >= 0)
        close(meta_fd);
    meta_fd = -1;
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

/* A stopped worker retains decoder, resampler, socket/file offset and metadata.
   Live servers may disconnect during a long pause; their existing reconnect
   policy still applies on resume. Never re-open the stream merely to pause. */
#define PLAYER_WAIT_STEPS 50
#define PLAYER_WAIT_MS 4

static int set_player_paused(int paused)
{
    unsigned int i;
    int status;
    pid_t done;

    reap_player(); /* ECHILD clears stale IDs before any signal. */
    if (player_pid <= 0)
        return -1;
    if (player_paused == paused)
        return 0;
    if (kill(player_pid, paused ? SIGSTOP : SIGCONT) < 0)
        return -1;
    for (i = 0; i < PLAYER_WAIT_STEPS; ++i) {
        do {
            done = waitpid(player_pid, &status,
                           WNOHANG | WUNTRACED | WCONTINUED);
        } while (done < 0 && errno == EINTR);
        if (done < 0) {
            if (errno == ECHILD)
                forget_player();
            return -1;
        }
        if (done > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                forget_player();
                return -1;
            }
            if ((paused && WIFSTOPPED(status)) ||
                (!paused && WIFCONTINUED(status))) {
                player_paused = paused;
                return 0;
            }
        }
        delay_ms(PLAYER_WAIT_MS);
    }
    return -1;
}

static int stop_player(void)
{
    unsigned int i;

    reap_player();
    if (player_pid <= 0)
        return 0;
    if (kill(player_pid, SIGTERM) < 0 && errno != ESRCH)
        return -1;
    /* A paused process cannot deliver SIGTERM until continued. */
    if (player_paused && kill(player_pid, SIGCONT) < 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < PLAYER_WAIT_STEPS * 2; ++i) {
        reap_player();
        if (player_pid <= 0)
            return 0;
        if (i == PLAYER_WAIT_STEPS && kill(player_pid, SIGKILL) < 0 &&
            errno != ESRCH)
            return -1;
        delay_ms(PLAYER_WAIT_MS);
    }
    reap_player();
    return player_pid <= 0 ? 0 : -1;
}

static int start_player(const char *url, const char *bus_path)
{
    int fds[2];
    pid_t child;

    if (stop_player() < 0)
        return -1;
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
        if (player_parent_guard() < 0)
            _exit(1);
        meta_out = fds[1];
        (void)fcntl(meta_out, F_SETFL, O_NONBLOCK);
        signal(SIGTERM, SIG_DFL);
        if (!running)
            _exit(0);
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
    return json_get_string(msg, key, out, size) == 1 ? 0 : -1;
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

/*
 * Build the "status" payload.  This is its own function so the JSON contract
 * -- in particular the Opus capability field the HTTP layer gates on -- can be
 * tested without pulling in the whole streaming stack.
 */
static void build_status_json(char *data, size_t size)
{
    char escaped_url[URL_MAX * 2], escaped_title[TITLE_MAX * 2];
    char escaped_station[TITLE_MAX * 2];
    const char *opus_capability;

    escape_json(escaped_url, sizeof(escaped_url), playing_url);
    escape_json(escaped_title, sizeof(escaped_title), playing_title);
    escape_json(escaped_station, sizeof(escaped_station), playing_station);
    /*
     * Publish the Opus decode capability so the HTTP layer can gate an Opus
     * launch before it happens.  It is gated on the same build macro as the
     * decode path itself: a build without an Opus decoder reports false and
     * links no Opus objects at all.
     */
#ifdef LE_RADIOD_ENABLE_OPUS
    opus_capability = le_radio_opus_available() ? "true" : "false";
#else
    opus_capability = "false";
#endif
    snprintf(data, size,
             "{\"playing\":%s,\"paused\":%s,\"url\":\"%s\",\"title\":\"%s\","
             "\"station\":\"%s\",\"opus\":%s}",
             player_pid > 0 && !player_paused ? "true" : "false",
             player_pid > 0 && player_paused ? "true" : "false", escaped_url,
             escaped_title, escaped_station, opus_capability);
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

        build_status_json(data, sizeof(data));
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "play")) {
        if (json_string_field(args ? args : message, "url",
                              url, sizeof(url)) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "url is required");
        if (url[0] != '/' && strncmp(url, "http://", 7) &&
            strncmp(url, "https://", 8))
            return le_adapter_respond_err(response, response_size, id,
                                          "url must be http:// or https://");
        if (start_player(url, bus_path) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "playback could not start");
        le_log_info("radiod: playing %s", url);
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "pause") || !strcmp(command, "resume")) {
        if (set_player_paused(!strcmp(command, "pause")) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "playback control failed or no player");
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    if (!strcmp(command, "stop")) {
        if (stop_player() < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "playback could not stop");
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

    install_stop_handlers();
    signal(SIGPIPE, SIG_IGN);

    listen_fd = le_adapter_listen(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "radiod: cannot listen on %s\n", socket_path);
        return 1;
    }
    le_log_info("radiod: ready socket=%s (http, https, mp3, aac, hls, icy metadata)",
                socket_path);
    while (running) {
        char message[LE_ADAPTER_MSG_MAX], response[LE_ADAPTER_MSG_MAX];
        ssize_t n;
        int client = le_adapter_accept(listen_fd);

        if (client < 0) {
            reap_player();
            if (!running)
                break;
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
