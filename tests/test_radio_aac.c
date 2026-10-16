/*
 * radio_aac: the ADTS frame contract and tone fidelity of the Helix wrapper.
 *
 * The fixture is 2 s of a 1 kHz sine, 44.1 kHz stereo AAC-LC, as raw ADTS.
 * It is decoded whole and in 1/13/700-byte feeds; the incomplete-frame
 * contract says a server that delivers writes smaller than one frame must
 * still decode the same total number of samples, and a truncated trailing
 * frame must be retained (-1), never dropped.
 */
#define _POSIX_C_SOURCE 200809L

#include "adapter/radio_aac.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define IN_CAP 32768            /* radiod's IN_BUFFER, so the cap is the same */
#define PCM_FRAMES 200000       /* interleaved samples recorded for analysis */
#define CHANNELS 2
#define RATE 44100

static unsigned char fixture[1 << 20];
static size_t fixture_len;
static short decoded[PCM_FRAMES];
static long decoded_total;

struct run_result {
    long total;                 /* interleaved samples decoded */
    size_t retained;            /* bytes left in the buffer when the loop ended */
    int channels, rate;
    int last;                   /* final le_radio_aac_take_frame return */
};

static int load_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        perror(path);
        return -1;
    }
    fixture_len = fread(fixture, 1, sizeof(fixture), f);
    fclose(f);
    return fixture_len ? 0 : -1;
}

/*
 * Drive the decoder exactly as the player loop does: one take_frame call per
 * iteration, refilling the fixed input buffer from src in `chunk`-sized
 * pieces, and stopping only when the source is drained and the buffer can no
 * longer complete a frame.
 */
static struct run_result run(size_t chunk, const unsigned char *src, size_t n)
{
    unsigned char in[IN_CAP];
    short pcm[LE_RADIO_AAC_MAX_SAMPLES];
    struct run_result r;
    size_t filled = 0, cursor = 0;
    long total = 0;
    int channels = 0, rate = 0, iterations = 0;

    memset(&r, 0, sizeof(r));
    decoded_total = 0;
    if (le_radio_aac_open() != 0) {
        r.total = -1;
        return r;
    }
    for (;;) {
        int samples = le_radio_aac_take_frame(in, &filled, sizeof(in), pcm,
                                              &channels, &rate);

        r.last = samples;
        if (++iterations > 1000000) {
            fprintf(stderr, "decode loop did not terminate\n");
            break;
        }
        if (samples > 0) {
            total += samples;
            if (decoded_total + samples <= PCM_FRAMES)
                memcpy(decoded + decoded_total, pcm,
                       (size_t)samples * sizeof(short));
            decoded_total += samples;
            continue;
        }
        if (samples == -2)
            break;                 /* no frame and no room to grow */
        if (samples == 0)
            continue;              /* a rejected complete frame was dropped */
        if (cursor >= n)
            break;                 /* source exhausted, tail retained */
        {
            size_t take = n - cursor, room = sizeof(in) - filled;

            if (take > chunk)
                take = chunk;
            if (take > room)
                take = room;
            if (!take)
                break;
            memcpy(in + filled, src + cursor, take);
            cursor += take;
            filled += take;
        }
    }
    le_radio_aac_close();
    r.total = total;
    r.channels = channels;
    r.rate = rate;
    r.retained = filled;
    return r;
}

/* Zero crossings of channel 0 over one second around the middle of the clip. */
static int middle_crossings(void)
{
    long frames = decoded_total / 2;
    long start = frames / 2 - RATE / 2, i;
    int count = 0, prev = 0;

    if (start < 0)
        start = 0;
    for (i = 0; i < RATE && start + i < frames; ++i) {
        short s = decoded[(start + i) * 2];
        int sign = s > 0 ? 1 : (s < 0 ? -1 : 0);

        if (sign && prev && sign != prev)
            ++count;
        if (sign)
            prev = sign;
    }
    return count;
}

/* RMS of channel 0 over the same one second. */
static double middle_rms(void)
{
    long frames = decoded_total / 2;
    long start = frames / 2 - RATE / 2, i;
    double sum = 0.0;

    if (start < 0)
        start = 0;
    for (i = 0; i < RATE && start + i < frames; ++i) {
        double v = decoded[(start + i) * 2];

        sum += v * v;
    }
    return sqrt(sum / (double)RATE);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/fixtures/radio/aac-tone.adts";
    struct run_result whole, one, thirteen, sevenhundred, junk, trunc;
    long expect;
    int crossings;

    CHECK(load_fixture(path) == 0);
    CHECK(fixture_len > 0);

    /* Whole file at once: the reference total and the tone to check. */
    whole = run(fixture_len, fixture, fixture_len);
    CHECK(whole.total > 0);
    CHECK(whole.channels == CHANNELS);
    CHECK(whole.rate == RATE);

    crossings = middle_crossings();
    fprintf(stderr, "1 kHz tone: zero crossings/s=%d  rms=%.1f  samples=%ld\n",
            crossings, middle_rms(), whole.total);
    CHECK(crossings >= 1960 && crossings <= 2040);
    CHECK(middle_rms() > 1000.0);
    expect = whole.total;

    /* The incomplete-frame contract: sub-frame writes must not lose a frame. */
    one = run(1, fixture, fixture_len);
    thirteen = run(13, fixture, fixture_len);
    sevenhundred = run(700, fixture, fixture_len);
    CHECK(one.total == expect);
    CHECK(thirteen.total == expect);
    CHECK(sevenhundred.total == expect);
    CHECK(one.channels == CHANNELS && one.rate == RATE);
    CHECK(thirteen.channels == CHANNELS && thirteen.rate == RATE);
    CHECK(sevenhundred.channels == CHANNELS && sevenhundred.rate == RATE);

    /* A garbage prefix that contains no 0xFF must be skipped, not decoded. */
    {
        static unsigned char prefixed[64 + sizeof(fixture)];
        size_t n = 64 + fixture_len;

        memset(prefixed, 0x00, 64);
        memcpy(prefixed + 64, fixture, fixture_len);
        junk = run(n, prefixed, n);
        CHECK(junk.total == expect);
        CHECK(junk.channels == CHANNELS && junk.rate == RATE);
    }

    /* A truncated final frame is retained (-1), not discarded. */
    trunc = run(fixture_len, fixture, fixture_len - 40);
    CHECK(trunc.last == -1);
    CHECK(trunc.retained > 0);
    CHECK(trunc.total > 0 && trunc.total < expect);

    /* is_adts: ADTS sync + layer 0 is accepted, everything else is not. */
    CHECK(le_radio_aac_is_adts(fixture, fixture_len) == 1);
    {
        static const unsigned char id3[] = { 'I', 'D', '3', 0x04 };
        static const unsigned char mp3[] = { 0xFF, 0xFB, 0x90, 0x00 };
        static const unsigned char sync[] = { 0xFF, 0xF1, 0x50, 0x80 };

        CHECK(le_radio_aac_is_adts(id3, sizeof(id3)) == 0);
        CHECK(le_radio_aac_is_adts(mp3, sizeof(mp3)) == 0);   /* layer 1 */
        CHECK(le_radio_aac_is_adts(sync, sizeof(sync)) == 1);
        CHECK(le_radio_aac_is_adts(fixture, 1) == 0);         /* too short */
    }

    printf("test_radio_aac OK (%ld samples)\n", expect);
    return 0;
}
