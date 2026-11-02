#define _POSIX_C_SOURCE 200809L
/* Owner event sounds (#410): WAV validation/conversion and the file store. */
#include "sound_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static unsigned char wav[LE_SOUND_UPLOAD_MAX_BYTES];
static int16_t pcm[LE_EVENT_SOUND_MAX_SAMPLES];

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xFFFFu)); put16(p + 2, (unsigned)(v >> 16)); }

/* A canonical WAV of `frames` frames; every sample of channel c is base+c. */
static size_t make_wav(unsigned format, unsigned channels, unsigned long rate,
                       unsigned bits, unsigned long frames, int base, int extra_chunk)
{
    size_t at = 12, i;
    unsigned long data_bytes = frames * channels * (bits / 8);

    memcpy(wav, "RIFF", 4);
    memcpy(wav + 8, "WAVE", 4);
    if (extra_chunk) { /* An odd-sized chunk before fmt must be skipped with its pad byte. */
        memcpy(wav + at, "LIST", 4); put32(wav + at + 4, 3); memcpy(wav + at + 8, "abc", 3); wav[at + 11] = 0; at += 12;
    }
    memcpy(wav + at, "fmt ", 4); put32(wav + at + 4, 16);
    put16(wav + at + 8, format); put16(wav + at + 10, channels);
    put32(wav + at + 12, rate); put32(wav + at + 16, rate * channels * (bits / 8));
    put16(wav + at + 20, channels * (bits / 8)); put16(wav + at + 22, bits);
    at += 24;
    memcpy(wav + at, "data", 4); put32(wav + at + 4, data_bytes); at += 8;
    for (i = 0; i < frames * channels; ++i) {
        put16(wav + at, (unsigned)(uint16_t)(int16_t)(base + (int)(i % channels)));
        at += 2;
    }
    put32(wav + 4, (unsigned long)(at - 8));
    return at;
}

static off_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) ? -1 : st.st_size;
}

int main(void)
{
    size_t n, samples;
    char dir[] = "/tmp/le-sound-store-XXXXXX", path[256], json[2048];
    int i;

    /* Mono 48 kHz is copied sample for sample. */
    n = make_wav(1, 1, 48000, 16, 4800, -1234, 0);
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_OK);
    CHECK(samples == 4800 && pcm[0] == -1234 && pcm[4799] == -1234);

    /* Stereo is averaged; 24 kHz doubles in length; extra chunks are skipped. */
    n = make_wav(1, 2, 24000, 16, 2400, 1000, 1);
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_OK);
    CHECK(samples == 4800 && pcm[0] == 1000 && pcm[4799] == 1000);

    /* 44.1 kHz resamples to the bus rate. */
    n = make_wav(1, 1, 44100, 16, 44100, 7, 0);
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_OK);
    CHECK(samples == 48000 && pcm[123] == 7);

    /* WAVE_FORMAT_EXTENSIBLE carrying PCM is accepted. */
    n = make_wav(1, 1, 48000, 16, 480, 5, 0);
    {
        static unsigned char ext[sizeof(wav)];
        size_t m = 12;
        memcpy(ext, wav, 12);
        memcpy(ext + m, "fmt ", 4); put32(ext + m + 4, 40);
        memcpy(ext + m + 8, wav + 20, 16); put16(ext + m + 8, 0xFFFEu);
        memset(ext + m + 24, 0, 24); put16(ext + m + 24, 22); put16(ext + m + 32, 1);
        m += 48;
        memcpy(ext + m, wav + 36, n - 36); m += n - 36;
        CHECK(le_sound_wav_convert(ext, m, 1000, pcm, &samples) == LE_SOUND_OK && samples == 480);
        put16(ext + 12 + 32, 3); /* extensible float */
        CHECK(le_sound_wav_convert(ext, m, 1000, pcm, &samples) == LE_SOUND_UNSUPPORTED);
    }

    /* Invalid files are rejected, never truncated into something playable. */
    CHECK(le_sound_wav_convert((const unsigned char *)"hello", 5, 1000, pcm, &samples) == LE_SOUND_NOT_WAV);
    n = make_wav(1, 1, 48000, 16, 480, 0, 0);
    memcpy(wav, "RIFX", 4);
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_NOT_WAV);
    n = make_wav(3, 1, 48000, 16, 480, 0, 0);          /* IEEE float */
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_UNSUPPORTED);
    n = make_wav(1, 1, 48000, 8, 480, 0, 0);           /* 8-bit */
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_UNSUPPORTED);
    n = make_wav(1, 3, 48000, 16, 480, 0, 0);          /* three channels */
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_UNSUPPORTED);
    n = make_wav(1, 1, 96000, 16, 480, 0, 0);          /* above the bus rate */
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_UNSUPPORTED);
    n = make_wav(1, 1, 48000, 16, 0, 0, 0);            /* no audio */
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_EMPTY);
    n = make_wav(1, 1, 48000, 16, 480, 0, 0);
    CHECK(le_sound_wav_convert(wav, 30, 1000, pcm, &samples) == LE_SOUND_NOT_WAV); /* truncated fmt */
    CHECK(le_sound_wav_convert(wav, 36, 1000, pcm, &samples) == LE_SOUND_NOT_WAV); /* no data chunk */
    /* A data size past the received bytes is clamped to what arrived. */
    CHECK(le_sound_wav_convert(wav, n - 100, 1000, pcm, &samples) == LE_SOUND_OK && samples == 430);
    /* Each event's own limit applies, judged before conversion. */
    n = make_wav(1, 1, 48000, 16, 48001, 0, 0);
    CHECK(le_sound_wav_convert(wav, n, 1000, pcm, &samples) == LE_SOUND_TOO_LONG && samples == 0);
    CHECK(le_sound_wav_convert(wav, n, 2000, pcm, &samples) == LE_SOUND_OK && samples == 48001);
    n = make_wav(1, 1, 8000, 16, 40001, 0, 0);
    CHECK(le_sound_wav_convert(wav, n, 5000, pcm, &samples) == LE_SOUND_TOO_LONG);
    puts("sound store: WAV validation and conversion: ok");

    /* The store: write selects, select toggles by rename, delete clears. */
    CHECK(mkdtemp(dir) != NULL);
    for (i = 0; i < 100; ++i) pcm[i] = (int16_t)(i - 50);
    CHECK(le_sound_store_write(dir, le_event_sound_index("timer"), pcm, 100) == 0);
    snprintf(path, sizeof(path), "%s/sound-timer.raw", dir);
    CHECK(file_size(path) == 200);
    {
        struct stat st;
        FILE *f = fopen(path, "rb");
        unsigned char b[4];
        CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(f && fread(b, 1, 4, f) == 4);
        fclose(f);
        CHECK(b[0] == (unsigned char)(uint16_t)-50 && b[1] == 0xFF && b[2] == (unsigned char)(uint16_t)-49);
    }
    CHECK(le_sound_store_json(dir, json, sizeof(json)) == 0);
    CHECK(strstr(json, "{\"id\":\"timer\",\"name\":\"Timer and alarm ring\",\"selected\":\"custom\",\"uploaded\":true") != NULL);
    CHECK(strstr(json, "{\"id\":\"wake\",\"name\":\"Listening chime\",\"selected\":\"default\",\"uploaded\":false") != NULL);
    CHECK(le_sound_store_select(dir, 2, 0) == 0);
    CHECK(file_size(path) < 0 && le_sound_store_uploaded(dir, 2));
    CHECK(le_sound_store_json(dir, json, sizeof(json)) == 0);
    CHECK(strstr(json, "\"id\":\"timer\",\"name\":\"Timer and alarm ring\",\"selected\":\"default\",\"uploaded\":true") != NULL);
    CHECK(le_sound_store_select(dir, 2, 1) == 0 && file_size(path) == 200);
    CHECK(le_sound_store_select(dir, 0, 1) == 1);          /* nothing uploaded for wake */
    /* Over-long data for an event is refused by the store as well. */
    CHECK(le_sound_store_write(dir, 0, pcm, 48001) == -1);
    CHECK(le_sound_store_write(dir, 2, pcm, 0) == -1);
    CHECK(le_sound_store_write(dir, 7, pcm, 10) == -1);
    /* A new upload replaces a kept one. */
    CHECK(le_sound_store_select(dir, 2, 0) == 0);
    CHECK(le_sound_store_write(dir, 2, pcm, 10) == 0 && file_size(path) == 20);
    snprintf(path, sizeof(path), "%s/sound-timer.raw.saved", dir);
    CHECK(file_size(path) < 0);
    CHECK(le_sound_store_delete(dir, 2) == 0 && !le_sound_store_uploaded(dir, 2));
    CHECK(le_sound_store_delete(dir, 2) == 0);
    rmdir(dir);
    puts("sound store: atomic write, select, keep and delete: ok");
    return 0;
}
