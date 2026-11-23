/*
 * FLAC decode for radiod: src/adapter/radio_flac.c over the vendored dr_flac.
 *
 * tests/fixtures/radio/flac-tone.flac carries a Vorbis comment and 8 KB of
 * padding ahead of the audio, so the forward-only seek path is exercised.
 * Its decoded PCM is compared against ffmpeg's decode of the same file
 * (frame count and FNV-1a hash recorded in tests/fixtures/radio/README.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio_flac.h"

#define EXPECTED_FRAMES 17640
#define EXPECTED_HASH 0xb1d327a5u

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #cond);                                       \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

struct feed {
    const unsigned char *data;
    size_t length, pos;
    size_t chunk;                     /* deliver at most this many per read */
    long fail_at;                     /* -1, or byte offset that errors */
};

static long feed_read(void *ctx, unsigned char *dst, size_t want)
{
    struct feed *f = ctx;
    size_t n = f->length - f->pos;

    if (f->fail_at >= 0 && f->pos >= (size_t)f->fail_at)
        return -1;
    if (n > want)
        n = want;
    if (n > f->chunk)
        n = f->chunk;
    if (f->fail_at >= 0 && f->pos + n > (size_t)f->fail_at)
        n = (size_t)f->fail_at - f->pos;
    memcpy(dst, f->data + f->pos, n);
    f->pos += n;
    return (long)n;
}

struct sink {
    unsigned hash;
    long long frames;
    int channels, rate, calls;
    long long stop_after;             /* refuse once this many frames arrive */
};

static int sink_take(void *ctx, const short *pcm, int frames, int channels,
                     int rate)
{
    struct sink *s = ctx;
    const unsigned char *bytes = (const unsigned char *)pcm;
    size_t i, n = (size_t)frames * (size_t)channels * sizeof(short);

    if (s->stop_after && s->frames >= s->stop_after)
        return -1;
    if (frames <= 0 || frames > LE_RADIO_FLAC_BLOCK)
        return -1;
    for (i = 0; i < n; ++i)
        s->hash = (s->hash ^ bytes[i]) * 16777619u;
    s->frames += frames;
    s->channels = channels;
    s->rate = rate;
    ++s->calls;
    return 0;
}

static unsigned char *load(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *length = (size_t)n;
    return data;
}

static int run(const unsigned char *data, size_t length, size_t chunk,
               long fail_at, struct sink *s, long long *frames)
{
    struct feed f;

    memset(&f, 0, sizeof(f));
    f.data = data;
    f.length = length;
    f.chunk = chunk;
    f.fail_at = fail_at;
    s->hash = 2166136261u;
    s->frames = 0;
    return le_radio_flac_play(feed_read, &f, sink_take, s, frames);
}

int main(int argc, char **argv)
{
    static const size_t chunks[] = {1, 7, 1000, 4096, 1u << 20};
    unsigned char *data;
    size_t length = 0, i;
    struct sink s;
    long long frames = 0;

    data = load(argc > 1 ? argv[1] : "tests/fixtures/radio/flac-tone.flac",
                &length);
    if (!data) {
        fprintf(stderr, "radio_flac: fixture missing\n");
        return 1;
    }
    CHECK(le_radio_flac_is_flac(data, length));
    CHECK(!le_radio_flac_is_flac((const unsigned char *)"ID3\x04", 4));

    /* Any delivery pattern yields ffmpeg's PCM exactly. */
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        memset(&s, 0, sizeof(s));
        CHECK(run(data, length, chunks[i], -1, &s, &frames) ==
              LE_RADIO_FLAC_OK);
        CHECK(frames == EXPECTED_FRAMES && s.frames == EXPECTED_FRAMES);
        CHECK(s.hash == EXPECTED_HASH);
        CHECK(s.channels == 2 && s.rate == 44100);
    }

    /* Cut short with a clean end: STREAMINFO's total exposes it. */
    memset(&s, 0, sizeof(s));
    CHECK(run(data, length - 3000, 4096, -1, &s, &frames) ==
          LE_RADIO_FLAC_SOURCE);
    CHECK(frames > 0 && frames < EXPECTED_FRAMES);

    /* A read error mid-stream is a source failure, not success. */
    memset(&s, 0, sizeof(s));
    CHECK(run(data, length, 4096, (long)(length * 3 / 4), &s, &frames) ==
          LE_RADIO_FLAC_SOURCE);

    /* The sink refusing stops decoding at once. */
    memset(&s, 0, sizeof(s));
    s.stop_after = 2000;
    CHECK(run(data, length, 4096, -1, &s, &frames) == LE_RADIO_FLAC_SINK);
    CHECK(frames < EXPECTED_FRAMES);

    /* Not FLAC at all, or the error arrives before the header. */
    memset(&s, 0, sizeof(s));
    CHECK(run((const unsigned char *)"ID3\x04garbage-garbage-garbage", 26,
              4096, -1, &s, &frames) == LE_RADIO_FLAC_NOT_FLAC);
    CHECK(s.calls == 0);
    memset(&s, 0, sizeof(s));
    CHECK(run(data, length, 4096, 10, &s, &frames) == LE_RADIO_FLAC_SOURCE);

    free(data);
    if (failures) {
        fprintf(stderr, "radio_flac: %d check(s) failed\n", failures);
        return 1;
    }
    puts("radio_flac: ok");
    return 0;
}
