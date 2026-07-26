/*
 * Mock TTS engine.
 *
 * Implements the tts_engine C ABI without any neural model.  Synthesis
 * produces a short, pleasant two-tone "chirp" (S16_LE mono at the bus
 * rate) so the entire announce pipeline — daemon, adapter protocol,
 * announcement bus FIFO, audio_engine ducking, and the green LED pulse —
 * can be exercised end to end before the real ZipVoice/sherpa-onnx
 * backend is wired in.
 *
 * The chirp length scales with text length so tests can observe that
 * different utterances behave differently, but it is bounded so a long
 * string never blocks the bus for long.
 */
#include "tts_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct tts_engine {
    int sample_rate;
};

struct tts_engine *tts_engine_init(const char *model_dir, const char *voice)
{
    struct tts_engine *engine;

    (void)model_dir;
    (void)voice;
    engine = (struct tts_engine *)calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    engine->sample_rate = LE_TTS_BUS_RATE;
    return engine;
}

int tts_engine_sample_rate(const struct tts_engine *engine)
{
    return engine ? engine->sample_rate : LE_TTS_BUS_RATE;
}

int tts_engine_synthesize(struct tts_engine *engine, const char *text,
                          short **out, size_t *n_out)
{
    size_t text_len;
    double seconds;
    size_t total;
    size_t split;
    short *samples;
    size_t i;
    const double rate = (double)(engine ? engine->sample_rate : LE_TTS_BUS_RATE);
    const double freq_low = 523.25;   /* C5  */
    const double freq_high = 783.99;  /* G5  */
    const double amplitude = 0.35;

    if (!out || !n_out)
        return -1;
    *out = NULL;
    *n_out = 0;
    if (!text)
        text = "";

    /* 60 ms per character, clamped to [0.3 s, 2.0 s]. */
    text_len = strlen(text);
    seconds = 0.06 * (double)text_len;
    if (seconds < 0.3)
        seconds = 0.3;
    if (seconds > 2.0)
        seconds = 2.0;

    total = (size_t)(seconds * rate);
    if (total == 0)
        total = 1;
    samples = (short *)malloc(total * sizeof(short));
    if (!samples)
        return -1;

    split = total / 2;
    for (i = 0; i < total; ++i) {
        double t = (double)i / rate;
        double freq = (i < split) ? freq_low : freq_high;
        /* 10 ms attack/decay envelope to avoid clicks. */
        double env = 1.0;
        double fade = 0.01 * rate;
        if ((double)i < fade)
            env = (double)i / fade;
        else if ((double)(total - i) < fade)
            env = (double)(total - i) / fade;
        {
            double v = amplitude * env * sin(2.0 * M_PI * freq * t);
            double scaled = v * 32767.0;
            if (scaled > 32767.0) scaled = 32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            samples[i] = (short)scaled;
        }
    }

    *out = samples;
    *n_out = total;
    return 0;
}

void tts_engine_free_samples(short *samples)
{
    free(samples);
}

void tts_engine_destroy(struct tts_engine *engine)
{
    free(engine);
}
