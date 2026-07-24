#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define main airplayd_program_main
#include "../src/adapter/airplayd.c"
#undef main

static void init_ctx(struct airplay_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->listener = -1;
    ctx->metadata_fd = -1;
}

static void feed_fragmented(struct airplay_ctx *ctx, const char *text,
                            size_t fragment)
{
    size_t length = strlen(text);
    size_t offset = 0;

    while (offset < length) {
        size_t count = length - offset;
        if (count > fragment)
            count = fragment;
        metadata_parser_feed(ctx, text + offset, count);
        offset += count;
    }
}

static void append_status(const struct airplay_ctx *ctx,
                          char *buffer, size_t size)
{
    size_t used = 0;

    buffer[0] = '\0';
    assert(json_append_raw(buffer, size, &used, "{\"base\":true") == 0);
    assert(metadata_append_status(ctx, buffer, size, &used) == 0);
    assert(json_append_raw(buffer, size, &used, "}") == 0);
}

static void test_fragmented_base64_and_json(void)
{
    static const char begin[] =
        "<item><type>73736e63</type><code>70626567</code>"
        "<length>0</length></item>";
    static const char title[] =
        "<item><type>636f7265</type><code>6d696e6d</code>"
        "<length>17</length><data encoding=\"base64\">"
        "TGlicmVFY2hvICJMaXZlIgo=</data></item>";
    static const char artist[] =
        "<item><type>core</type><code>asar</code>"
        "<length>11</length><data encoding='base64'>"
        "VGVzdCBBcnRpc3Q=</data></item>";
    static const char album[] =
        "<item><type>636f7265</type><code>6173616c</code>"
        "<length>9</length><data encoding=\"base64\">\n"
        "VGVzdCBEaXNj\n</data></item>";
    struct airplay_ctx ctx;
    char status[2048];

    init_ctx(&ctx);
    feed_fragmented(&ctx, begin, 1);
    feed_fragmented(&ctx, title, 2);
    feed_fragmented(&ctx, artist, 3);
    feed_fragmented(&ctx, album, 5);
    assert(ctx.playing == 1);
    assert(!strcmp(ctx.title, "LibreEcho \"Live\"\n"));
    assert(!strcmp(ctx.artist, "Test Artist"));
    assert(!strcmp(ctx.album, "Test Disc"));
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"playing\""));
    assert(strstr(status, "\"source\":\"airplay\""));
    assert(strstr(status, "\"title\":\"LibreEcho \\\"Live\\\"\\u000a\""));
    assert(strstr(status, "\"artist\":\"Test Artist\""));
    assert(strstr(status, "\"album\":\"Test Disc\""));
}

static void test_missing_metadata_and_session_clear(void)
{
    static const char begin[] =
        "<item><type>ssnc</type><code>pbeg</code>"
        "<length>0</length></item>";
    static const char end[] =
        "<item><type>ssnc</type><code>pend</code>"
        "<length>0</length></item>";
    struct airplay_ctx ctx;
    char status[512];

    init_ctx(&ctx);
    strcpy(ctx.title, "stale title");
    metadata_parser_feed(&ctx, begin, strlen(begin));
    assert(ctx.playing == 1);
    assert(ctx.title[0] == '\0');
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"playing\""));
    assert(strstr(status, "\"source\":\"airplay\""));
    assert(!strstr(status, "\"title\""));
    strcpy(ctx.artist, "stale artist");
    metadata_parser_feed(&ctx, end, strlen(end));
    assert(ctx.playing == 0);
    assert(ctx.artist[0] == '\0');
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"stopped\""));
    assert(!strstr(status, "\"source\""));
    assert(!strstr(status, "\"artist\""));
}

static void test_malformed_input_recovers(void)
{
    static const char malformed[] =
        "garbage<item><type>core</type><code>minm</code>"
        "<length>5</length><data encoding=\"base64\">"
        "!!!!</data></item>";
    static const char valid[] =
        "<item><type>core</type><code>minm</code>"
        "<length>5</length><data encoding=\"base64\">"
        "QWZ0ZXI=</data></item>";
    static const char wrong_length[] =
        "<item><type>core</type><code>asar</code>"
        "<length>6</length><data encoding=\"base64\">"
        "U2hvcnQ=</data></item>";
    struct airplay_ctx ctx;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, malformed, strlen(malformed));
    assert(ctx.title[0] == '\0');
    metadata_parser_feed(&ctx, wrong_length, strlen(wrong_length));
    assert(ctx.artist[0] == '\0');
    feed_fragmented(&ctx, valid, 4);
    assert(!strcmp(ctx.title, "After"));
}

static void test_oversized_item_recovers(void)
{
    static const char valid[] =
        "<item><type>core</type><code>asal</code>"
        "<length>2</length><data encoding=\"base64\">"
        "T0s=</data></item>";
    struct airplay_ctx ctx;
    char chunk[257];
    size_t remaining = AIRPLAY_METADATA_ITEM_MAX + 128;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, "<item>", 6);
    memset(chunk, 'x', sizeof(chunk));
    while (remaining > 0) {
        size_t count = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        metadata_parser_feed(&ctx, chunk, count);
        remaining -= count;
    }
    metadata_parser_feed(&ctx, "</item>", 7);
    assert(ctx.metadata_parser.used == 0);
    feed_fragmented(&ctx, valid, 7);
    assert(!strcmp(ctx.album, "OK"));
}

static void test_oversized_field_is_ignored(void)
{
    static const char oversized[] =
        "<item><type>core</type><code>minm</code>"
        "<length>193</length><data encoding=\"base64\">"
        "QQ==</data></item>";
    static const char invalid_utf8[] =
        "<item><type>core</type><code>asar</code>"
        "<length>2</length><data encoding=\"base64\">"
        "wyg=</data></item>";
    struct airplay_ctx ctx;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, oversized, strlen(oversized));
    metadata_parser_feed(&ctx, invalid_utf8, strlen(invalid_utf8));
    assert(ctx.title[0] == '\0');
    assert(ctx.artist[0] == '\0');
}

static void test_fifo_is_nonblocking(void)
{
    static const char begin[] =
        "<item><type>ssnc</type><code>pbeg</code>"
        "<length>0</length></item>";
    struct airplay_ctx ctx;
    struct stat status;
    char directory[] = "/tmp/libreecho-airplay-metadata.XXXXXX";
    int flags;

    init_ctx(&ctx);
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(ctx.metadata_path, sizeof(ctx.metadata_path),
                    "%s/metadata", directory) > 0);
    assert(metadata_fifo_open(&ctx) == 0);
    assert(lstat(ctx.metadata_path, &status) == 0);
    assert(S_ISFIFO(status.st_mode));
    flags = fcntl(ctx.metadata_fd, F_GETFL);
    assert(flags >= 0);
    assert((flags & O_NONBLOCK) != 0);
    assert(write(ctx.metadata_fd, begin, sizeof(begin) - 1) ==
           (ssize_t)(sizeof(begin) - 1));
    metadata_fifo_drain(&ctx);
    assert(ctx.playing == 1);
    metadata_fifo_close(&ctx);
    assert(unlink(ctx.metadata_path) == 0);
    assert(rmdir(directory) == 0);
}

int main(void)
{
    test_fragmented_base64_and_json();
    test_missing_metadata_and_session_clear();
    test_malformed_input_recovers();
    test_oversized_item_recovers();
    test_oversized_field_is_ignored();
    test_fifo_is_nonblocking();
    puts("AirPlay metadata ingestion: ok");
    return 0;
}
