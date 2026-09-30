/*
 * Nursery / sleep-source synthesis for audiod.
 *
 * This unit is deliberately self-contained and header-only: the audio daemon
 * build owns an explicit source list (Makefile AUDIOD_SOURCES), and a new
 * translation unit would need that list edited. Keeping the synthesis here as
 * static functions means audiod.c gains the feature with no build-system
 * change, and the tests compile the same code without linking anything.
 *
 * What it generates, all procedurally and with no bundled sample:
 *
 *   - white / pink / brown noise, the colours shipped for issue #54;
   *   - a low-frequency heartbeat with a soft double pulse and a smooth,
 *     always-zero-at-the-edges envelope so beats cannot click;
 *   - an optional low-level pink/brown bed under the heartbeat.
 *
 * Determinism: the noise feed is a seeded xorshift32 and the heartbeat is a
 * closed-form phase, so a fixed seed reproduces the stream byte for byte.
 * That is what lets the host tests assert envelope and cap behaviour without
 * a device.
 *
 * Safety: the output is clamped to +-LE_SLEEP_AMPLITUDE_CAP (8000 of 32767)
 * before it leaves this unit, independent of every caller-supplied value, and
 * the fades are bounded so a stop or a timer expiry cannot produce a step.
 *
 * The generators produce interleaved S16 stereo at LE_SLEEP_RATE, which is the
 * media bus rate, so nothing is resampled.
 */
#ifndef LIBREECHO_SLEEP_GENERATOR_H
#define LIBREECHO_SLEEP_GENERATOR_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Source selection. NONE means "no generator"; init refuses it. */
#define LE_SLEEP_SOURCE_NONE 0
#define LE_SLEEP_SOURCE_WHITE 1
#define LE_SLEEP_SOURCE_PINK 2
#define LE_SLEEP_SOURCE_BROWN 3
#define LE_SLEEP_SOURCE_HEARTBEAT 4

/* Optional bed under the heartbeat. NONE is the default. */
#define LE_SLEEP_BED_NONE 0
#define LE_SLEEP_BED_PINK 1
#define LE_SLEEP_BED_BROWN 2

/* Tempo range for the heartbeat. Below 40 BPM it stops reading as a pulse;
   above 100 it stops being restful. Both ends are hard clamps, not errors. */
#define LE_SLEEP_TEMPO_MIN 40
#define LE_SLEEP_TEMPO_MAX 100
#define LE_SLEEP_TEMPO_DEFAULT 60

/* Output ceiling in S16 units (of 32767) and the level scale that reaches it. */
#define LE_SLEEP_AMPLITUDE_CAP 8000
#define LE_SLEEP_LEVEL_MAX 100

/* Fades, in frames at LE_SLEEP_RATE (48 kHz). */
#define LE_SLEEP_FADE_IN_FRAMES 24000   /* 0.5 s ramp at the start */
#define LE_SLEEP_FADE_OUT_FRAMES 14400  /* 0.3 s worst-case stop ramp */
#define LE_SLEEP_MAX_FADE_SECONDS 3600  /* one hour: keeps the product bounded */

#define LE_SLEEP_RATE 48000
#define LE_SLEEP_CHANNELS 2
#define LE_SLEEP_BED_GAIN 0.3

struct le_sleep_gen {
    int source;
    int bed;
    int tempo_bpm;
    int level;
    double amplitude;            /* <= LE_SLEEP_AMPLITUDE_CAP */
    long total_frames;           /* -1 while unlimited */
    long fade_out_frames;        /* last-N-frames fade, 0 when none */
    long frames_done;
    int active;
    int stopping;
    long stop_left;              /* frames left of the stop ramp */
    uint32_t rng;
    /* Programme and bed filter state, kept separate so a bed cannot colour
       the main noise and vice versa. */
    double brown, pink_a, pink_b, pink_c;
    double bed_brown, bed_pink_a, bed_pink_b, bed_pink_c;
    double beat_frames;          /* frames per beat at tempo_bpm */
};

static inline uint32_t le_sleep_random(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* One normalised (-1..1) noise sample of the requested colour. */
static inline double le_sleep_noise(struct le_sleep_gen *g, int colour)
{
    double white = (double)(int32_t)le_sleep_random(&g->rng) / 2147483648.0;

    if (colour == LE_SLEEP_SOURCE_BROWN) {
        /* Leaky integrator: it cannot wander to the rails. */
        g->brown = g->brown + white * 0.05;
        if (g->brown > 1.0) g->brown = 1.0;
        if (g->brown < -1.0) g->brown = -1.0;
        g->brown *= 0.995;
        return g->brown * 6.0;
    }
    if (colour == LE_SLEEP_SOURCE_PINK) {
        g->pink_a = 0.99765 * g->pink_a + white * 0.0990460;
        g->pink_b = 0.96300 * g->pink_b + white * 0.2965164;
        g->pink_c = 0.57000 * g->pink_c + white * 1.0526913;
        return (g->pink_a + g->pink_b + g->pink_c + white * 0.1848) * 0.25;
    }
    return white;
}

static inline double le_sleep_bed_noise(struct le_sleep_gen *g)
{
    double white = (double)(int32_t)le_sleep_random(&g->rng) / 2147483648.0;

    if (g->bed == LE_SLEEP_BED_BROWN) {
        g->bed_brown = g->bed_brown + white * 0.05;
        if (g->bed_brown > 1.0) g->bed_brown = 1.0;
        if (g->bed_brown < -1.0) g->bed_brown = -1.0;
        g->bed_brown *= 0.995;
        return g->bed_brown * 6.0;
    }
    g->bed_pink_a = 0.99765 * g->bed_pink_a + white * 0.0990460;
    g->bed_pink_b = 0.96300 * g->bed_pink_b + white * 0.2965164;
    g->bed_pink_c = 0.57000 * g->bed_pink_c + white * 1.0526913;
    return (g->bed_pink_a + g->bed_pink_b + g->bed_pink_c + white * 0.1848) * 0.25;
}

/*
 * Soft double pulse. "lub" starts at the beat and "dub" a third of a beat
 * later; each is a low sine under a sin^2 window, so the envelope is exactly
 * zero at both edges and the pulse cannot click. The window is applied to the
 * carrier rather than the sample history, so consecutive beats stay
 * phase-continuous with silence between them.
 */
static inline double le_sleep_heartbeat(const struct le_sleep_gen *g)
{
    /* Seconds within the current beat. */
    double t = (double)g->beat_frames > 0.0
        ? (double)(g->frames_done % (long)(g->beat_frames > 1.0
                                           ? g->beat_frames : 1.0))
          / (double)LE_SLEEP_RATE : 0.0;
    const double lub_start = 0.0, lub_end = 0.10, lub_hz = 55.0;
    const double dub_start = 0.30, dub_end = 0.385, dub_hz = 48.0;
    double value = 0.0;

    if (t >= lub_start && t < lub_end) {
        double u = (t - lub_start) / (lub_end - lub_start);

        value += sin(3.14159265358979323846 * u) *
                 sin(3.14159265358979323846 * u) *
                 sin(2.0 * 3.14159265358979323846 * lub_hz * t);
    }
    if (t >= dub_start && t < dub_end) {
        double u = (t - dub_start) / (dub_end - dub_start);

        value += 0.7 * sin(3.14159265358979323846 * u) *
                 sin(3.14159265358979323846 * u) *
                 sin(2.0 * 3.14159265358979323846 * dub_hz * t);
    }
    return value;
}

/* Gain applied to one absolute frame position. Stop always wins over end. */
static inline double le_sleep_gain(const struct le_sleep_gen *g, long pos)
{
    double gain = 1.0;

    if (g->stopping)
        return g->stop_left <= 0 ? 0.0
             : (double)g->stop_left / (double)LE_SLEEP_FADE_OUT_FRAMES;
    if (pos < LE_SLEEP_FADE_IN_FRAMES)
        gain = (double)pos / (double)LE_SLEEP_FADE_IN_FRAMES;
    if (g->fade_out_frames > 0 && g->total_frames > 0) {
        long left = g->total_frames - pos;

        if (left < g->fade_out_frames) {
            double f = (double)left / (double)g->fade_out_frames;

            if (f < gain)
                gain = f;
        }
    }
    if (gain < 0.0)
        gain = 0.0;
    if (gain > 1.0)
        gain = 1.0;
    return gain;
}

static inline int le_sleep_gen_active(const struct le_sleep_gen *g)
{
    return g && g->active;
}

/*
 * Normalise caller input, clamp tempo/level/fades, and arm the generator.
 * Returns 0 when a stream was configured, -1 when the request names no source
 * (everything is clamped rather than rejected, but NONE means nothing to do).
 */
static inline int le_sleep_gen_init(struct le_sleep_gen *g, int source, int bed,
                                    int tempo_bpm, int level, long seconds,
                                    long fade_seconds, uint32_t seed)
{
    if (!g)
        return -1;
    memset(g, 0, sizeof(*g));
    if (source <= LE_SLEEP_SOURCE_NONE || source > LE_SLEEP_SOURCE_HEARTBEAT)
        return -1;
    if (bed != LE_SLEEP_BED_PINK && bed != LE_SLEEP_BED_BROWN)
        bed = LE_SLEEP_BED_NONE;
    if (source != LE_SLEEP_SOURCE_HEARTBEAT)
        bed = LE_SLEEP_BED_NONE;             /* the bed only rides the pulse */
    if (tempo_bpm < LE_SLEEP_TEMPO_MIN)
        tempo_bpm = LE_SLEEP_TEMPO_MIN;
    if (tempo_bpm > LE_SLEEP_TEMPO_MAX)
        tempo_bpm = LE_SLEEP_TEMPO_MAX;
    if (level < 1)
        level = 1;
    if (level > LE_SLEEP_LEVEL_MAX)
        level = LE_SLEEP_LEVEL_MAX;
    if (seconds < 0)
        seconds = 0;
    if (fade_seconds < 0)
        fade_seconds = 0;
    if (fade_seconds > LE_SLEEP_MAX_FADE_SECONDS)
        fade_seconds = LE_SLEEP_MAX_FADE_SECONDS;

    g->source = source;
    g->bed = bed;
    g->tempo_bpm = tempo_bpm;
    g->level = level;
    g->amplitude = LE_SLEEP_AMPLITUDE_CAP * (double)level
                 / (double)LE_SLEEP_LEVEL_MAX;
    g->total_frames = seconds > 0 ? seconds * (long)LE_SLEEP_RATE : -1;
    g->fade_out_frames = fade_seconds * (long)LE_SLEEP_RATE;
    if (g->total_frames > 0 && g->fade_out_frames > g->total_frames)
        g->fade_out_frames = g->total_frames;
    g->rng = seed ? seed : 0x1234567u;
    g->beat_frames = (double)LE_SLEEP_RATE * 60.0 / (double)tempo_bpm;
    g->active = 1;
    return 0;
}

/* Arm a bounded, click-free stop: the next fills ramp to silence. */
static inline void le_sleep_gen_request_stop(struct le_sleep_gen *g)
{
    if (!g->stopping) {
        g->stopping = 1;
        g->stop_left = LE_SLEEP_FADE_OUT_FRAMES;
    }
}

/*
 * Render up to `frames` interleaved stereo frames. Returns the number written:
 * short of `frames` only on the final call, 0 once the stream has finished.
 * Every sample is bounded to +-LE_SLEEP_AMPLITUDE_CAP here.
 */
static inline size_t le_sleep_gen_fill(struct le_sleep_gen *g, int16_t *out,
                                       size_t frames)
{
    size_t i;

    if (!g || !g->active)
        return 0;
    for (i = 0; i < frames; ++i) {
        long pos = g->frames_done;
        double value, gain;

        if (g->stopping && g->stop_left <= 0)
            return i;
        if (g->total_frames > 0 && pos >= g->total_frames)
            return i;
        if (g->source == LE_SLEEP_SOURCE_HEARTBEAT) {
            value = le_sleep_heartbeat(g);
            if (g->bed != LE_SLEEP_BED_NONE)
                value += le_sleep_bed_noise(g) * LE_SLEEP_BED_GAIN;
        } else {
            value = le_sleep_noise(g, g->source);
        }
        if (value > 1.0)
            value = 1.0;
        if (value < -1.0)
            value = -1.0;
        gain = le_sleep_gain(g, pos);
        value *= gain;
        out[i * LE_SLEEP_CHANNELS] = (int16_t)(value * g->amplitude);
        out[i * LE_SLEEP_CHANNELS + 1] = (int16_t)(value * g->amplitude);
        g->frames_done = pos + 1;
        if (g->stopping)
            --g->stop_left;
    }
    return frames;
}

/* Whole frames left before the natural end, or -1 when unlimited. */
static inline long le_sleep_gen_remaining(const struct le_sleep_gen *g)
{
    if (!g || g->total_frames < 0)
        return -1;
    return g->total_frames > g->frames_done
         ? g->total_frames - g->frames_done : 0;
}

#endif /* LIBREECHO_SLEEP_GENERATOR_H */
