/*
 * HLS playlist parsing and URL resolution for the internet radio player.
 *
 * Pure: no sockets, no I/O, no allocation. Split out of radiod.c so the
 * playlist rules (and the BBC playlists they have to survive) are testable
 * without a network or the rest of the player.
 *
 * Playlists arrive as raw network bytes and are NOT necessarily
 * NUL-terminated, so every scan is length-bounded.
 */
#ifndef LIBREECHO_RADIO_HLS_H
#define LIBREECHO_RADIO_HLS_H

#include <stddef.h>

/*
 * These limits mirror the constants radiod.c enforces when it splits a URL
 * before connecting (URL_MAX / HOST_MAX / PATH_MAX_LEN there). They are
 * duplicated on purpose: this header must not depend on radiod.c internals.
 * le_hls_resolve refuses anything that split_url would reject, so a resolved
 * URI can never fail there.
 */
#define LE_HLS_URL_MAX 512
#define LE_HLS_MAX_SEGMENTS 16

struct le_hls_playlist {
    int is_master;                     /* saw #EXT-X-STREAM-INF */
    char variant[LE_HLS_URL_MAX];      /* master: chosen variant (absolute) */
    long long media_sequence;
    int target_duration;
    int endlist;
    int count;
    long long seq[LE_HLS_MAX_SEGMENTS];
    char uri[LE_HLS_MAX_SEGMENTS][LE_HLS_URL_MAX]; /* resolved absolute */
};

/*
 * Parse a playlist body of n bytes. base_url is the URL the body was fetched
 * from and resolves relative URIs for both variants and segments. Returns 0 on
 * success, -1 on a refused playlist (missing #EXTM3U, encryption, fMP4, or a
 * master with no variant that fits). A master fills variant; a media playlist
 * fills media_sequence/target_duration/endlist/seq/uri/count, keeping only the
 * last LE_HLS_MAX_SEGMENTS segments.
 */
int le_hls_parse(const char *text, size_t n, const char *base_url,
                 struct le_hls_playlist *out);

/*
 * Resolve ref against base: an absolute http(s):// URL is kept, "/path" keeps
 * the base's scheme+authority, and anything else is joined to the base with
 * its query string stripped, up to and including the last '/'. Returns -1 if
 * ref does not fit in out or the result would exceed the radiod host/path
 * limits.
 */
int le_hls_resolve(const char *base, const char *ref, char *out, size_t size);

#endif
