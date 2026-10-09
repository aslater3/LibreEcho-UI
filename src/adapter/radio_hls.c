/*
 * HLS playlist parsing and URL resolution for the internet radio player.
 *
 * Pure: no sockets, no I/O, no allocation. The BBC playlists this has to read
 * are in SPEC.md section 1; the rules are in section 3d. Every scan is
 * length-bounded because the caller hands over a raw fetched body that is not
 * NUL-terminated.
 */
#include "radio_hls.h"

#include <stdio.h>
#include <string.h>

#define LE_HLS_HOST_MAX 256      /* radiod.c HOST_MAX */
#define LE_HLS_PORT_MAX 16       /* radiod.c port buffer */
#define LE_HLS_PATH_MAX 384      /* radiod.c PATH_MAX_LEN */
#define LE_HLS_BANDWIDTH_MAX 128000   /* highest audio variant we start on */

static int has_prefix(const char *s, size_t n, const char *prefix)
{
    size_t l = strlen(prefix);

    return n >= l && !memcmp(s, prefix, l);
}

static int ci_eq(const char *s, size_t n, const char *want)
{
    size_t l = strlen(want), i;

    if (l != n)
        return 0;
    for (i = 0; i < l; ++i) {
        char c = s[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != want[i])
            return 0;
    }
    return 1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * A URL is usable only if radiod's split_url would accept it. split_url fails
 * on an empty or >= HOST_MAX host, an empty or >= 16 port, and a path that
 * snprintf cannot fit in PATH_MAX_LEN. Mirroring that here means a resolved
 * URI can never make the fetcher fail.
 */
static int url_ok(const char *u)
{
    size_t scheme = 0, host_len, path_len;
    const char *rest, *slash, *colon;

    if (!strncmp(u, "http://", 7))
        scheme = 7;
    else if (!strncmp(u, "https://", 8))
        scheme = 8;
    if (!scheme)
        return -1;
    for (rest = u; *rest; ++rest)       /* no CR/LF/space/control bytes */
        if ((unsigned char)*rest <= 0x20 || (unsigned char)*rest == 0x7f)
            return -1;
    rest = u + scheme;
    slash = strchr(rest, '/');
    colon = memchr(rest, ':', slash ? (size_t)(slash - rest) : strlen(rest));
    host_len = colon ? (size_t)(colon - rest)
                     : (slash ? (size_t)(slash - rest) : strlen(rest));
    if (!host_len || host_len >= LE_HLS_HOST_MAX)
        return -1;
    if (colon) {
        size_t n = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);

        if (!n || n >= LE_HLS_PORT_MAX)
            return -1;
    }
    path_len = slash ? strlen(slash) : 1;
    if (path_len >= LE_HLS_PATH_MAX)
        return -1;
    return 0;
}

static int url_set(const char *u, char *out, size_t size)
{
    size_t n = strlen(u);

    if (n >= size || url_ok(u) < 0)
        return -1;
    memcpy(out, u, n + 1);
    return 0;
}

int le_hls_resolve(const char *base, const char *ref, char *out, size_t size)
{
    char tmp[LE_HLS_URL_MAX];
    size_t scheme, auth_len, cut, last, i;
    int built, folder;

    if (!ref || !out || !size)
        return -1;
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8))
        return url_set(ref, out, size);
    if (!base)
        return -1;
    if (!strncmp(base, "http://", 7))
        scheme = 7;
    else if (!strncmp(base, "https://", 8))
        scheme = 8;
    else
        return -1;
    if (ref[0] == '/' && ref[1] == '/') {      /* network-path reference */
        built = snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(scheme - 2), base,
                         ref);
    } else if (ref[0] == '/') {
        /* the authority ends at '/', '?' or '#': a query-only base is fine */
        auth_len = scheme + strcspn(base + scheme, "/?#");
        built = snprintf(tmp, sizeof(tmp), "%.*s%s", (int)auth_len, base, ref);
    } else {
        cut = strcspn(base, "?#");             /* base query is dropped */
        last = cut;
        folder = 0;
        for (i = cut; i > scheme; --i) {
            if (base[i - 1] == '/') {
                last = i;
                folder = 1;
                break;
            }
        }
        built = snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)last, base,
                         folder ? "" : "/", ref);
    }
    if (built < 0 || built >= (int)sizeof(tmp))
        return -1;
    return url_set(tmp, out, size);
}

/*
 * Walk a comma-separated attribute list, honouring double quotes (CODECS
 * values contain commas). name/value point into [s, s+n) and are trimmed;
 * returns 0 once the list is exhausted.
 */
static int next_attr(const char *s, size_t n, size_t *pos,
                     const char **name, size_t *name_len,
                     const char **value, size_t *value_len)
{
    size_t start, i, eq = 0, end;
    int quoted = 0;

    if (*pos >= n)
        return 0;
    start = *pos;
    for (i = start; i < n; ++i) {
        if (s[i] == '"')
            quoted = !quoted;
        else if (s[i] == ',' && !quoted)
            break;
        else if (s[i] == '=' && !quoted && !eq)
            eq = i;
    }
    end = i;
    *pos = i < n ? i + 1 : n;
    if (!eq) {
        *name = s + start;
        *name_len = end - start;
        *value = NULL;
        *value_len = 0;
    } else {
        *name = s + start;
        *name_len = eq - start;
        *value = s + eq + 1;
        *value_len = end - eq - 1;
    }
    while (*name_len && (*name)[0] == ' ') {
        ++*name;
        --*name_len;
    }
    while (*name_len && (*name)[*name_len - 1] == ' ')
        --*name_len;
    if (*value) {
        while (*value_len && (*value)[0] == ' ') {
            ++*value;
            --*value_len;
        }
        while (*value_len && (*value)[*value_len - 1] == ' ')
            --*value_len;
        if (*value_len >= 2 && (*value)[0] == '"' &&
            (*value)[*value_len - 1] == '"') {
            ++*value;
            *value_len -= 2;
        }
    }
    return 1;
}

static int attr_is(const char *name, size_t len, const char *want)
{
    return len == strlen(want) && !memcmp(name, want, len);
}

static int digits(const char *s, size_t n, long long *out)
{
    long long v = 0;
    size_t i;

    if (!n)
        return -1;
    for (i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        if (v > (2000000000000LL - (s[i] - '0')) / 10)
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

static int codec_ok(const char *s, size_t n)
{
    static const char *const allowed[] = {
        "mp4a.40.2", "mp4a.40.5", "mp4a.40.34", "mp3"
    };
    size_t k;

    for (k = 0; k < sizeof(allowed) / sizeof(allowed[0]); ++k) {
        size_t l = strlen(allowed[k]), i;

        if (l != n)
            continue;
        for (i = 0; i < n; ++i) {
            char c = s[i];

            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != allowed[k][i])
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

static int codecs_ok(const char *v, size_t n)
{
    size_t i = 0;

    while (i < n) {
        size_t start = i, len;

        while (i < n && v[i] != ',')
            ++i;
        len = i - start;
        while (len && v[start] == ' ') {
            ++start;
            --len;
        }
        while (len && v[start + len - 1] == ' ')
            --len;
        if (!codec_ok(v + start, len))
            return 0;
        ++i;
    }
    return 1;
}

static void append_segment(struct le_hls_playlist *out, long long seq,
                           const char *uri)
{
    if (out->count < LE_HLS_MAX_SEGMENTS) {
        out->seq[out->count] = seq;
        copy_str(out->uri[out->count], LE_HLS_URL_MAX, uri);
        ++out->count;
        return;
    }
    memmove(out->uri[0], out->uri[1],
            sizeof(out->uri[0]) * (LE_HLS_MAX_SEGMENTS - 1));
    memmove(out->seq, out->seq + 1,
            sizeof(out->seq[0]) * (LE_HLS_MAX_SEGMENTS - 1));
    out->seq[LE_HLS_MAX_SEGMENTS - 1] = seq;
    copy_str(out->uri[LE_HLS_MAX_SEGMENTS - 1], LE_HLS_URL_MAX, uri);
}

int le_hls_parse(const char *text, size_t n, const char *base_url,
                 struct le_hls_playlist *out)
{
    char ref[LE_HLS_URL_MAX], url[LE_HLS_URL_MAX], low_uri[LE_HLS_URL_MAX];
    long low_bw = 0, best_le_bw = 0, seg_index = 0;
    int have_le = 0, have_low = 0;
    int pending_stream = 0, pending_ok = 0, pending_bw = 0, pending_extinf = 0;
    int seen_header = 0;
    size_t pos = 0;

    if (!text || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    while (pos < n) {
        size_t end = pos, len;
        const char *line;

        while (end < n && text[end] != '\n')
            ++end;
        len = end - pos;
        if (len && text[pos + len - 1] == '\r')
            --len;
        line = text + pos;
        pos = end + 1;

        if (!seen_header) {
            if (!len)
                continue;              /* blank lines before the header */
            if (len != 7 || memcmp(line, "#EXTM3U", 7))
                return -1;
            seen_header = 1;
            continue;
        }
        if (!len)
            continue;

        if (line[0] == '#') {
            if (has_prefix(line, len, "#EXTINF:")) {
                pending_extinf = 1;
            } else if (has_prefix(line, len, "#EXT-X-STREAM-INF:")) {
                const char *attrs = line + sizeof("#EXT-X-STREAM-INF:") - 1;
                size_t alen = len - (sizeof("#EXT-X-STREAM-INF:") - 1);
                size_t apos = 0, nl, vl;
                const char *a, *v;
                long long bw = 0;
                int have_bw = 0, cs_ok = 1;

                out->is_master = 1;
                pending_extinf = 0;
                while (next_attr(attrs, alen, &apos, &a, &nl, &v, &vl)) {
                    if (attr_is(a, nl, "BANDWIDTH")) {
                        if (digits(v, vl, &bw) == 0)
                            have_bw = 1;
                    } else if (attr_is(a, nl, "CODECS")) {
                        cs_ok = codecs_ok(v, vl);
                    }
                }
                pending_stream = 1;
                pending_ok = have_bw && cs_ok;
                pending_bw = have_bw ? (bw > 2000000000LL ? 2000000000 : (int)bw) : 0;
            } else if (has_prefix(line, len, "#EXT-X-MEDIA-SEQUENCE:")) {
                long long v;

                if (digits(line + sizeof("#EXT-X-MEDIA-SEQUENCE:") - 1,
                           len - (sizeof("#EXT-X-MEDIA-SEQUENCE:") - 1),
                           &v) == 0)
                    out->media_sequence = v;
            } else if (has_prefix(line, len, "#EXT-X-TARGETDURATION:")) {
                long long v;

                if (digits(line + sizeof("#EXT-X-TARGETDURATION:") - 1,
                           len - (sizeof("#EXT-X-TARGETDURATION:") - 1),
                           &v) == 0)
                    out->target_duration = (int)v;
            } else if (len == sizeof("#EXT-X-ENDLIST") - 1 &&
                       !memcmp(line, "#EXT-X-ENDLIST", len)) {
                out->endlist = 1;
            } else if (has_prefix(line, len, "#EXT-X-KEY:")) {
                const char *attrs = line + sizeof("#EXT-X-KEY:") - 1;
                size_t alen = len - (sizeof("#EXT-X-KEY:") - 1);
                size_t apos = 0, nl, vl;
                const char *a, *v;
                int unencrypted = 0;

                while (next_attr(attrs, alen, &apos, &a, &nl, &v, &vl)) {
                    if (attr_is(a, nl, "METHOD"))
                        unencrypted = ci_eq(v, vl, "none");
                }
                if (!unencrypted)
                    return -1;         /* encrypted: refuse clearly */
            } else if (has_prefix(line, len, "#EXT-X-MAP")) {
                return -1;             /* fMP4: refuse clearly */
            }
            continue;
        }

        /* A URI line: the pending STREAM-INF variant or EXTINF segment. */
        if (len >= sizeof(ref)) {
            if (pending_stream)
                pending_stream = 0;
            else if (pending_extinf) {
                pending_extinf = 0;
                ++seg_index;           /* still occupies a playlist slot */
            }
            continue;
        }
        memcpy(ref, line, len);
        ref[len] = '\0';
        if (pending_stream) {
            pending_stream = 0;
            if (pending_ok &&
                le_hls_resolve(base_url, ref, url, sizeof(url)) == 0) {
                if (pending_bw <= LE_HLS_BANDWIDTH_MAX) {
                    if (!have_le || pending_bw > best_le_bw) {
                        best_le_bw = pending_bw;
                        have_le = 1;
                        copy_str(out->variant, sizeof(out->variant), url);
                    }
                } else if (!have_low || pending_bw < low_bw) {
                    low_bw = pending_bw;
                    have_low = 1;
                    copy_str(low_uri, sizeof(low_uri), url);
                }
            }
            continue;
        }
        if (pending_extinf) {
            long long seq = out->media_sequence + seg_index;

            pending_extinf = 0;
            ++seg_index;
            if (le_hls_resolve(base_url, ref, url, sizeof(url)) == 0)
                append_segment(out, seq, url);
        }
    }

    if (!seen_header)
        return -1;
    if (out->is_master) {
        if (!have_le && !have_low)
            return -1;                 /* no usable variant */
        if (!have_le)
            copy_str(out->variant, sizeof(out->variant), low_uri);
        out->count = 0;
    }
    return 0;
}
