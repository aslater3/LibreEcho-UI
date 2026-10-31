#include "voice_ns.h"

#include <math.h>
#include <string.h>

/* Per-frame (8 ms) constants. */
#define NS_SMOOTHING 0.7f        /* power smoothing before minimum tracking */
/* The floor climbs at most 3 dB a second (x2 per 125 frames), so a louder
   room is learnt in seconds while speech barely lifts it. */
#define NS_MINIMUM_RISE 1.00556f
#define NS_NOISE_BIAS 2.0f       /* minimum of smoothed power -> mean noise */
#define NS_DD_ALPHA 0.98f        /* decision-directed a-priori SNR */
#define NS_MIN_SNR 1e-3f
#define NS_POWER_EPSILON 1e-3f

static void fft(const struct le_voice_ns *ns, float *re, float *im)
{
    unsigned int size, i;

    for (i = 0; i < LE_VOICE_NS_FFT; ++i) {
        unsigned int j = ns->bit_reverse[i];

        if (j > i) {
            float t = re[i];

            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (size = 2; size <= LE_VOICE_NS_FFT; size <<= 1) {
        unsigned int half = size / 2U, step = LE_VOICE_NS_FFT / size;
        unsigned int start;

        for (start = 0; start < LE_VOICE_NS_FFT; start += size) {
            unsigned int k;

            for (k = 0; k < half; ++k) {
                float wr = ns->twiddle_re[k * step];
                float wi = ns->twiddle_im[k * step];
                unsigned int a = start + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;

                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

static void process_block(struct le_voice_ns *ns)
{
    float re[LE_VOICE_NS_FFT], im[LE_VOICE_NS_FFT];
    unsigned int i;

    memmove(ns->frame, ns->frame + LE_VOICE_NS_HOP,
            (LE_VOICE_NS_FFT - LE_VOICE_NS_HOP) * sizeof(ns->frame[0]));
    for (i = 0; i < LE_VOICE_NS_HOP; ++i)
        ns->frame[LE_VOICE_NS_FFT - LE_VOICE_NS_HOP + i] =
            (float)ns->input[i];
    for (i = 0; i < LE_VOICE_NS_FFT; ++i) {
        re[i] = ns->frame[i] * ns->window[i];
        im[i] = 0.0f;
    }
    fft(ns, re, im);
    for (i = 0; i < LE_VOICE_NS_BINS; ++i) {
        float power = re[i] * re[i] + im[i] * im[i];
        float noise, posterior, prior, gain;

        if (!ns->frames) {
            ns->smoothed[i] = power;
            ns->minimum[i] = power;
            ns->previous[i] = 1.0f;
        } else {
            ns->smoothed[i] = NS_SMOOTHING * ns->smoothed[i] +
                              (1.0f - NS_SMOOTHING) * power;
            if (ns->smoothed[i] < ns->minimum[i])
                ns->minimum[i] = ns->smoothed[i];
            else if (ns->smoothed[i] > ns->minimum[i] * NS_MINIMUM_RISE)
                ns->minimum[i] = ns->minimum[i] * NS_MINIMUM_RISE +
                                 NS_POWER_EPSILON;
            else
                ns->minimum[i] = ns->smoothed[i];
        }
        noise = NS_NOISE_BIAS * ns->minimum[i] + NS_POWER_EPSILON;
        posterior = power / noise;
        prior = NS_DD_ALPHA * ns->previous[i] +
                (1.0f - NS_DD_ALPHA) *
                    (posterior > 1.0f ? posterior - 1.0f : 0.0f);
        if (prior < NS_MIN_SNR)
            prior = NS_MIN_SNR;
        gain = prior / (1.0f + prior);
        if (gain < ns->floor_gain)
            gain = ns->floor_gain;
        ns->previous[i] = gain * gain * posterior;
        re[i] *= gain;
        im[i] *= gain;
        if (i && i < LE_VOICE_NS_FFT / 2U) {
            re[LE_VOICE_NS_FFT - i] *= gain;
            im[LE_VOICE_NS_FFT - i] *= gain;
        }
    }
    ++ns->frames;
    /* Inverse transform as the conjugate of a forward transform. */
    for (i = 0; i < LE_VOICE_NS_FFT; ++i)
        im[i] = -im[i];
    fft(ns, re, im);
    for (i = 0; i < LE_VOICE_NS_FFT; ++i)
        ns->overlap[i] += re[i] * (1.0f / (float)LE_VOICE_NS_FFT) *
                          ns->window[i];
    for (i = 0; i < LE_VOICE_NS_HOP; ++i) {
        float value = ns->overlap[i];

        value += value < 0.0f ? -0.5f : 0.5f;
        if (value > 32767.0f)
            value = 32767.0f;
        if (value < -32768.0f)
            value = -32768.0f;
        ns->output[i] = (int16_t)value;
    }
    memmove(ns->overlap, ns->overlap + LE_VOICE_NS_HOP,
            (LE_VOICE_NS_FFT - LE_VOICE_NS_HOP) * sizeof(ns->overlap[0]));
    memset(ns->overlap + LE_VOICE_NS_FFT - LE_VOICE_NS_HOP, 0,
           LE_VOICE_NS_HOP * sizeof(ns->overlap[0]));
}

void le_voice_ns_reset(struct le_voice_ns *ns)
{
    if (!ns)
        return;
    memset(ns->frame, 0, sizeof(ns->frame));
    memset(ns->overlap, 0, sizeof(ns->overlap));
    memset(ns->smoothed, 0, sizeof(ns->smoothed));
    memset(ns->minimum, 0, sizeof(ns->minimum));
    memset(ns->previous, 0, sizeof(ns->previous));
    memset(ns->input, 0, sizeof(ns->input));
    memset(ns->output, 0, sizeof(ns->output));
    ns->position = 0;
    ns->frames = 0;
}

int le_voice_ns_init(struct le_voice_ns *ns, int attenuation_db)
{
    const double pi = 3.14159265358979323846;
    unsigned int i, bits = 0;

    if (!ns || attenuation_db < LE_VOICE_NS_MIN_ATTENUATION_DB ||
        attenuation_db > LE_VOICE_NS_MAX_ATTENUATION_DB)
        return -1;
    memset(ns, 0, sizeof(*ns));
    while ((1U << bits) < LE_VOICE_NS_FFT)
        ++bits;
    for (i = 0; i < LE_VOICE_NS_FFT; ++i) {
        unsigned int j = 0, b;

        for (b = 0; b < bits; ++b)
            if (i & (1U << b))
                j |= 1U << (bits - 1U - b);
        ns->bit_reverse[i] = (unsigned short)j;
        /* Periodic sqrt-Hann: its square overlap-adds to one at 50 %. */
        ns->window[i] = (float)sqrt(
            0.5 - 0.5 * cos(2.0 * pi * (double)i / (double)LE_VOICE_NS_FFT));
    }
    for (i = 0; i < LE_VOICE_NS_FFT / 2U; ++i) {
        ns->twiddle_re[i] =
            (float)cos(-2.0 * pi * (double)i / (double)LE_VOICE_NS_FFT);
        ns->twiddle_im[i] =
            (float)sin(-2.0 * pi * (double)i / (double)LE_VOICE_NS_FFT);
    }
    ns->floor_gain = (float)pow(10.0, -(double)attenuation_db / 20.0);
    le_voice_ns_reset(ns);
    return 0;
}

void le_voice_ns_process(struct le_voice_ns *ns, int16_t *samples,
                         size_t count)
{
    size_t i;

    if (!ns || !samples)
        return;
    for (i = 0; i < count; ++i) {
        int16_t value = samples[i];

        samples[i] = ns->output[ns->position];
        ns->input[ns->position] = value;
        if (++ns->position == LE_VOICE_NS_HOP) {
            process_block(ns);
            ns->position = 0;
        }
    }
}
