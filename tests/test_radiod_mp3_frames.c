/*
 * Regression: a server that delivers MP3 body bytes in writes smaller than
 * one frame must still produce audio. The reviewed loop consumed the whole
 * input buffer on every zero-sample decode, so a stream that paced small
 * writes had every read discarded before its frame completed and produced no
 * audio across all retries. mp3_take_frame is now the single decode path and
 * only removes bytes that were decoded or definitively rejected.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#define main radiod_program_main
#include "../src/adapter/radiod.c"
#undef main

#include "radiod_mp3_fixture.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/*
 * Mirror play_stream's loop against the fixture with a fixed delivery size.
 * Returns the total PCM samples decoded; stops when the source is exhausted
 * and the buffered tail can no longer complete a frame.
 */
static long feed_fixture(size_t chunk)
{
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    unsigned char in[IN_BUFFER];
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t filled = 0, cursor = 0;
    long total = 0;
    int iterations = 0;

    memset(&info, 0, sizeof(info));
    mp3dec_init(&decoder);
    for (;;) {
        int samples = mp3_take_frame(&decoder, in, &filled, sizeof(in), pcm,
                                     &info);

        if (++iterations > 1000000) {
            fprintf(stderr, "decode loop did not terminate\n");
            return -1;
        }
        if (samples > 0) {
            total += samples;
            continue;
        }
        if (samples == -2)
            break;                        /* no frame and no room to grow */
        if (samples == 0)
            continue;                     /* rejected frame dropped */
        if (cursor >= sizeof(mp3_fixture))
            break;                        /* source exhausted */
        {
            size_t take = sizeof(mp3_fixture) - cursor;
            size_t room = sizeof(in) - filled;

            if (take > chunk)
                take = chunk;
            if (take > room)
                take = room;
            if (!take)
                break;
            memcpy(in + filled, mp3_fixture + cursor, take);
            cursor += take;
            filled += take;
        }
    }
    return total;
}

/* A stream with no frame sync must terminate, not spin, and yield no audio. */
static int junk_terminates(void)
{
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    unsigned char in[IN_BUFFER];
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t filled = 0;
    int iterations = 0;

    memset(&info, 0, sizeof(info));
    mp3dec_init(&decoder);
    for (;;) {
        int samples = mp3_take_frame(&decoder, in, &filled, sizeof(in), pcm,
                                     &info);

        if (++iterations > 100000)
            return 0;
        if (samples > 0)
            return 0;                     /* junk must never decode */
        if (samples == -2)
            return filled > 0;            /* fixed buffer filled with junk */
        if (samples == 0)
            continue;
        /* -1: keep feeding the fixed buffer one chunk at a time */
        if (sizeof(in) - filled >= 1024)
            memset(in + filled, 0x5a, 1024), filled += 1024;
        else
            return 0;
    }
}

int main(void)
{
    long whole, small;

    whole = feed_fixture(sizeof(mp3_fixture));
    small = feed_fixture(16);
    printf("samples: whole=%ld 16-byte-chunks=%ld\n", whole, small);
    CHECK(whole > 0);
    CHECK(small > 0);
    CHECK(small == whole);
    CHECK(junk_terminates() == 1);
    puts("radiod mp3 frame accumulation: sub-frame writes still decode: ok");
    return 0;
}
