/*
 * Tests for the HLS playlist parser and URL resolver.
 *
 * The two BBC playlists are copied verbatim from SPEC.md section 1 (captured
 * from lsn.lv and the akamaized media playlist on 2026-09-30); they are text
 * only, no audio. Everything here is offline.
 */
#include "adapter/radio_hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

/* (1) lsn.lv master: CRLF, absolute variant, no trailing newline. */
static const char BBC_MASTER[] =
    "#EXTM3U\r\n"
    "#EXT-X-VERSION:3\r\n"
    "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=101760,CODECS=\"mp4a.40.5\"\r\n"
    "http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/"
    "bbc_radio_two.isml/bbc_radio_two-audio%3d96000.norewind.m3u8";

#define BBC_MASTER_BASE \
    "http://lsn.lv/bbcradio.m3u8?station=bbc_radio_two&bitrate=96000"

#define BBC_MEDIA_BASE \
    "http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/" \
    "bbc_radio_two.isml/bbc_radio_two-audio%3d96000.norewind.m3u8"

#define BBC_MEDIA_DIR \
    "http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/" \
    "bbc_radio_two.isml/"

/* (2) akamaized media playlist: LF, relative segments, no trailing newline. */
static const char BBC_MEDIA[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
    "#EXT-X-MEDIA-SEQUENCE:279810706\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    "#EXT-X-TARGETDURATION:6\n"
    "#USP-X-TIMESTAMP-MAP:MPEGTS=6614164896,LOCAL=2026-09-30T17:15:12Z\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-09-30T17:15:12Z\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_two-audio=96000-279810706.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_two-audio=96000-279810707.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_two-audio=96000-279810708.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_two-audio=96000-279810709.ts\n"
    "#EXTINF:6.4, no desc\n"
    "bbc_radio_two-audio=96000-279810710.ts";

/* (3) multi-variant master with relative variant URIs. */
static const char BBC_VARIANTS[] =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
    "\n"
    "# variants\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=56000,AVERAGE-BANDWIDTH=51000,CODECS=\"mp4a.40.5\"\n"
    "bbc_radio_two-audio=48000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=112000,AVERAGE-BANDWIDTH=102000,CODECS=\"mp4a.40.5\"\n"
    "bbc_radio_two-audio=96000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=150000,AVERAGE-BANDWIDTH=136000,CODECS=\"mp4a.40.2\"\n"
    "bbc_radio_two-audio=128000.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=374000,AVERAGE-BANDWIDTH=340000,CODECS=\"mp4a.40.2\"\n"
    "bbc_radio_two-audio=320000.m3u8";

#define BBC_VARIANTS_BASE \
    "http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/bbc_radio_two/" \
    "bbc_radio_two.isml/bbc_radio_two.m3u8"

#define SEG_DIR BBC_MEDIA_DIR

static int parse(const char *text, const char *base, struct le_hls_playlist *out)
{
    return le_hls_parse(text, strlen(text), base, out);
}

static int test_bbc_master(void)
{
    struct le_hls_playlist p;

    CHECK(parse(BBC_MASTER, BBC_MASTER_BASE, &p) == 0);
    CHECK(p.is_master == 1);
    CHECK(p.count == 0);
    CHECK(!strcmp(p.variant, BBC_MEDIA_BASE));
    return 0;
}

static int test_bbc_media(void)
{
    struct le_hls_playlist p;
    int i;

    CHECK(parse(BBC_MEDIA, BBC_MEDIA_BASE, &p) == 0);
    CHECK(p.is_master == 0);
    CHECK(p.media_sequence == 279810706);
    CHECK(p.target_duration == 6);
    CHECK(p.endlist == 0);
    CHECK(p.count == 5);
    for (i = 0; i < p.count; ++i)
        CHECK(p.seq[i] == 279810706 + i);
    CHECK(!strcmp(p.uri[0],
        SEG_DIR "bbc_radio_two-audio=96000-279810706.ts"));
    CHECK(!strcmp(p.uri[4],
        SEG_DIR "bbc_radio_two-audio=96000-279810710.ts"));
    return 0;
}

static int test_bbc_variants(void)
{
    struct le_hls_playlist p;

    CHECK(parse(BBC_VARIANTS, BBC_VARIANTS_BASE, &p) == 0);
    CHECK(p.is_master == 1);
    CHECK(!strcmp(p.variant,
        "http://as-hls-ww-live.akamaized.net/pool_74208725/live/ww/"
        "bbc_radio_two/bbc_radio_two.isml/bbc_radio_two-audio=96000.m3u8"));
    return 0;
}

static int test_resolve(void)
{
    char out[LE_HLS_URL_MAX];

    /* Absolute URLs are kept verbatim. */
    CHECK(le_hls_resolve("http://h/a/b.m3u8", "https://x/y/z.ts",
                         out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "https://x/y/z.ts"));
    CHECK(le_hls_resolve("http://h/a/b.m3u8",
                         "http://x/y/z.ts?q=1", out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://x/y/z.ts?q=1"));

    /* Relative: base query stripped, up to and including the last '/'. */
    CHECK(le_hls_resolve("http://h/a/b.m3u8?token=1", "c.ts",
                         out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://h/a/c.ts"));
    CHECK(le_hls_resolve("http://h/a/b.m3u8", "c.ts", out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://h/a/c.ts"));

    /* '/absolute' keeps the base scheme and authority. */
    CHECK(le_hls_resolve("http://h/a/b.m3u8?token=1", "/x/y.ts",
                         out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://h/x/y.ts"));
    CHECK(le_hls_resolve("https://h:8080/a/b.m3u8", "/c.ts",
                         out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "https://h:8080/c.ts"));
    CHECK(le_hls_resolve("http://h:8080/a/b.m3u8", "d.ts",
                         out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://h:8080/a/d.ts"));

    /* No path in the base still lands under the authority. */
    CHECK(le_hls_resolve("http://h", "s.ts", out, sizeof(out)) == 0);
    CHECK(!strcmp(out, "http://h/s.ts"));

    /* Too small an output buffer is refused, not truncated. */
    CHECK(le_hls_resolve("http://h/a/b.m3u8", "c.ts", out, 8) == -1);

    /* Path over the radiod limit -> refused so split_url can never fail. */
    {
        char hugeref[LE_HLS_URL_MAX];

        memset(hugeref, 'p', sizeof(hugeref) - 1);
        hugeref[sizeof(hugeref) - 1] = '\0';
        CHECK(le_hls_resolve("http://h/", hugeref, out, sizeof(out)) == -1);
    }

    /* Host over the radiod limit -> refused. */
    {
        char host[LE_HLS_URL_MAX];

        memcpy(host, "http://", 7);
        memset(host + 7, 'a', 300);
        memcpy(host + 307, "/x.ts", 6);
        CHECK(le_hls_resolve("http://h/a/b.m3u8", host, out, sizeof(out)) == -1);
    }

    /* Port over the radiod limit -> refused. */
    CHECK(le_hls_resolve("http://h/a/b.m3u8",
                         "http://h:12345678901234567890/x.ts",
                         out, sizeof(out)) == -1);
    return 0;
}

static int test_refusals(void)
{
    struct le_hls_playlist p;

    CHECK(le_hls_parse("", 0, "http://h/p.m3u8", &p) == -1);
    CHECK(parse("hello\n", "http://h/p.m3u8", &p) == -1);
    CHECK(parse("#EXTM3U\n#EXTINF:1,\na.ts\n", "http://h/p.m3u8", &p) == 0);
    CHECK(parse("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n"
                "#EXTINF:1,\na.ts\n", "http://h/p.m3u8", &p) == -1);
    CHECK(parse("#EXTM3U\n#EXT-X-MAP:URI=\"i.mp4\"\n#EXTINF:1,\na.ts\n",
                "http://h/p.m3u8", &p) == -1);
    CHECK(parse("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:1,\na.ts\n",
                "http://h/p.m3u8", &p) == 0);
    CHECK(p.count == 1);
    CHECK(!strcmp(p.uri[0], "http://h/a.ts"));
    /* A master with no variant we can use is a refusal. */
    CHECK(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=96000,"
                "CODECS=\"avc1.42E01E\"\nv.m3u8\n", "http://h/p.m3u8", &p) == -1);
    CHECK(parse("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"mp4a.40.5\"\n"
                "v.m3u8\n", "http://h/p.m3u8", &p) == -1);
    return 0;
}

static int test_variant_choice(void)
{
    struct le_hls_playlist p;

    /* AVERAGE-BANDWIDTH must not be mistaken for BANDWIDTH (substring trap):
       the 64000 variant wins even though its AVERAGE-BANDWIDTH is huge. */
    CHECK(parse("#EXTM3U\n"
                "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=1000,BANDWIDTH=200000,"
                "CODECS=\"mp4a.40.5\"\na.m3u8\n"
                "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=999999,BANDWIDTH=64000,"
                "CODECS=\"mp4a.40.5\"\nb.m3u8\n",
                "http://h/x/y.m3u8", &p) == 0);
    CHECK(!strcmp(p.variant, "http://h/x/b.m3u8"));

    /* No variant at or below 128000 -> the lowest one is chosen. */
    CHECK(parse("#EXTM3U\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=150000,CODECS=\"mp4a.40.2\"\nhi.m3u8\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=374000,CODECS=\"mp4a.40.2\"\nbig.m3u8\n",
                "http://h/x/y.m3u8", &p) == 0);
    CHECK(!strcmp(p.variant, "http://h/x/hi.m3u8"));

    /* Acceptable CODECS list (all entries allowed) is eligible. */
    CHECK(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=96000,"
                "CODECS=\"mp4a.40.2,mp4a.40.5\"\nx.m3u8\n",
                "http://h/p.m3u8", &p) == 0);
    CHECK(!strcmp(p.variant, "http://h/x.m3u8"));

    /* A single overlong variant URI leaves nothing usable -> refusal. */
    {
        char text[1024];
        char ref[600];
        size_t i;

        for (i = 0; i < sizeof(ref) - 1; ++i)
            ref[i] = 'q';
        ref[sizeof(ref) - 1] = '\0';
        snprintf(text, sizeof(text),
                 "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=96000,"
                 "CODECS=\"mp4a.40.5\"\n%s\n", ref);
        CHECK(le_hls_parse(text, strlen(text), "http://h/", &p) == -1);
    }
    return 0;
}

static int test_segment_retention(void)
{
    char text[2048];
    struct le_hls_playlist p;
    size_t used = 0;
    int i;

    used += (size_t)snprintf(text + used, sizeof(text) - used,
                             "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:100\n");
    for (i = 0; i < 20; ++i)
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "#EXTINF:1.0,\nseg%d.ts\n", i);
    CHECK(parse(text, "http://h/d/p.m3u8", &p) == 0);
    CHECK(p.count == LE_HLS_MAX_SEGMENTS);
    CHECK(p.seq[0] == 104);
    CHECK(p.seq[15] == 119);
    CHECK(!strcmp(p.uri[0], "http://h/d/seg4.ts"));
    CHECK(!strcmp(p.uri[15], "http://h/d/seg19.ts"));

    /* An overlong segment URI is skipped, but later sequence indices keep
       their true playlist position. */
    {
        char ref[600];
        size_t i;

        for (i = 0; i < sizeof(ref) - 1; ++i)
            ref[i] = 'z';
        ref[sizeof(ref) - 1] = '\0';
        snprintf(text, sizeof(text),
                 "#EXTM3U\n#EXTINF:1,\na.ts\n#EXTINF:1,\n%s\n"
                 "#EXTINF:1,\nb.ts\n", ref);
        CHECK(le_hls_parse(text, strlen(text), "http://h/d/p.m3u8", &p) == 0);
        CHECK(p.count == 2);
        CHECK(p.seq[0] == 0);
        CHECK(p.seq[1] == 2);
        CHECK(!strcmp(p.uri[0], "http://h/d/a.ts"));
        CHECK(!strcmp(p.uri[1], "http://h/d/b.ts"));
    }

    /* CRLF media playlist, and a leading blank line before #EXTM3U. */
    CHECK(parse("\r\n#EXTM3U\r\n#EXTINF:1,\r\ns.ts\r\n",
                "http://h/x/p.m3u8", &p) == 0);
    CHECK(p.count == 1);
    CHECK(!strcmp(p.uri[0], "http://h/x/s.ts"));

    /* #EXT-X-ENDLIST is recorded. */
    CHECK(parse("#EXTM3U\n#EXTINF:1,\ns.ts\n#EXT-X-ENDLIST\n",
                "http://h/p.m3u8", &p) == 0);
    CHECK(p.endlist == 1);
    return 0;
}

/* The parsed body is raw network bytes, so it must survive without a NUL. */
static int test_unterminated(void)
{
    struct le_hls_playlist p;
    size_t m = strlen(BBC_MEDIA);
    char *raw = malloc(m);   /* exact size, no terminator */

    CHECK(raw != NULL);
    memcpy(raw, BBC_MEDIA, m);
    CHECK(le_hls_parse(raw, m, BBC_MEDIA_BASE, &p) == 0);
    CHECK(p.count == 5);
    CHECK(p.media_sequence == 279810706);
    CHECK(p.seq[4] == 279810710);
    free(raw);
    return 0;
}

/* Review regressions: control bytes, query-only base, //host, big sequence. */
static int test_review_regressions(void)
{
    char url[LE_HLS_URL_MAX];
    struct le_hls_playlist pl;
    static const char big[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:2147483648\n"
        "#EXTINF:6,\na.ts\n#EXTINF:6,\nb.ts\n";

    CHECK(le_hls_resolve("http://h/d/l.m3u8", "http://h/s.ts\rX: y", url,
                         sizeof(url)) == -1);
    CHECK(le_hls_resolve("http://h/d/l.m3u8", "s.ts\nX: y", url,
                         sizeof(url)) == -1);
    CHECK(le_hls_resolve("http://h/d/l.m3u8", "a b.ts", url,
                         sizeof(url)) == -1);
    CHECK(le_hls_resolve("http://h?token=s", "/seg.ts", url,
                         sizeof(url)) == 0);
    CHECK(strcmp(url, "http://h/seg.ts") == 0);
    CHECK(le_hls_resolve("https://h/d/l.m3u8", "//cdn.x/s.ts", url,
                         sizeof(url)) == 0);
    CHECK(strcmp(url, "https://cdn.x/s.ts") == 0);
    CHECK(le_hls_parse(big, sizeof(big) - 1, "http://h/l.m3u8", &pl) == 0);
    CHECK(pl.media_sequence == 2147483648LL);
    CHECK(pl.seq[0] == 2147483648LL && pl.seq[1] == 2147483649LL);
    return 0;
}

int main(void)
{
    CHECK(test_bbc_master() == 0);
    CHECK(test_bbc_media() == 0);
    CHECK(test_bbc_variants() == 0);
    CHECK(test_resolve() == 0);
    CHECK(test_refusals() == 0);
    CHECK(test_variant_choice() == 0);
    CHECK(test_segment_retention() == 0);
    CHECK(test_unterminated() == 0);
    CHECK(test_review_regressions() == 0);

    printf("test_radio_hls OK\n");
    return 0;
}
