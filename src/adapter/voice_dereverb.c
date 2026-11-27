#include "voice_dereverb.h"

#include <math.h>
#include <string.h>

#define DEREVERB_POWER_EPSILON 1e-3f
/* Never subtract the whole bin: the floor below takes over from here. */
#define DEREVERB_MAX_SUBTRACTION 0.95f

static void fft(const struct le_voice_dereverb *dereverb, float *re, float *im)
{
    unsigned int size, i;

    for (i = 0; i < LE_VOICE_DEREVERB_FFT; ++i) {
        unsigned int j = dereverb->bit_reverse[i];

        if (j > i) {
            float t = re[i];

            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (size = 2; size <= LE_VOICE_DEREVERB_FFT; size <<= 1) {
        unsigned int half = size / 2U, step = LE_VOICE_DEREVERB_FFT / size;
        unsigned int start;

        for (start = 0; start < LE_VOICE_DEREVERB_FFT; start += size) {
            unsigned int k;

            for (k = 0; k < half; ++k) {
                float wr = dereverb->twiddle_re[k * step];
                float wi = dereverb->twiddle_im[k * step];
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

static void process_block(struct le_voice_dereverb *dereverb)
{
    float power[LE_VOICE_DEREVERB_BINS];
    float re[LE_VOICE_DEREVERB_FFT], im[LE_VOICE_DEREVERB_FFT];
    const float *delayed = NULL;
    float delay_weight = 1.0f;
    unsigned int i;

    memmove(dereverb->frame, dereverb->frame + LE_VOICE_DEREVERB_HOP,
            (LE_VOICE_DEREVERB_FFT - LE_VOICE_DEREVERB_HOP) *
                sizeof(dereverb->frame[0]));
    for (i = 0; i < LE_VOICE_DEREVERB_HOP; ++i)
        dereverb->frame[LE_VOICE_DEREVERB_FFT -
                        LE_VOICE_DEREVERB_HOP + i] =
            (float)dereverb->input[i];
    for (i = 0; i < LE_VOICE_DEREVERB_FFT; ++i) {
        re[i] = dereverb->frame[i] * dereverb->window[i];
        im[i] = 0.0f;
    }
    fft(dereverb, re, im);
    for (i = 0; i < LE_VOICE_DEREVERB_BINS; ++i)
        power[i] = re[i] * re[i] + im[i] * im[i];

    if (dereverb->history_count >= LE_VOICE_DEREVERB_DELAY_FRAMES) {
        unsigned int index = (dereverb->history_position +
                              LE_VOICE_DEREVERB_HISTORY_FRAMES -
                              LE_VOICE_DEREVERB_DELAY_FRAMES) %
                             LE_VOICE_DEREVERB_HISTORY_FRAMES;

        delayed = dereverb->history[index];
        delay_weight = 1.0f;
        for (i = 0; i < LE_VOICE_DEREVERB_DELAY_FRAMES; ++i)
            delay_weight *= LE_VOICE_DEREVERB_DECAY;
    }
    for (i = 0; i < LE_VOICE_DEREVERB_BINS; ++i) {
        float observed = power[i];
        float gain;

        if (delayed) {
            dereverb->late[i] = LE_VOICE_DEREVERB_DECAY *
                                    dereverb->late[i] +
                                delay_weight * delayed[i];
        }
        if (dereverb->late[i] > observed * DEREVERB_MAX_SUBTRACTION)
            dereverb->late[i] = observed * DEREVERB_MAX_SUBTRACTION;
        gain = observed > DEREVERB_POWER_EPSILON
            ? sqrtf((observed - dereverb->late[i]) / observed)
            : 1.0f;
        if (gain < dereverb->floor_gain)
            gain = dereverb->floor_gain;
        re[i] *= gain;
        im[i] *= gain;
        if (i && i < LE_VOICE_DEREVERB_FFT / 2U) {
            re[LE_VOICE_DEREVERB_FFT - i] *= gain;
            im[LE_VOICE_DEREVERB_FFT - i] *= gain;
        }
    }
    memcpy(dereverb->history[dereverb->history_position], power,
           sizeof(power));
    if (++dereverb->history_position == LE_VOICE_DEREVERB_HISTORY_FRAMES)
        dereverb->history_position = 0;
    if (dereverb->history_count < LE_VOICE_DEREVERB_HISTORY_FRAMES)
        ++dereverb->history_count;
    ++dereverb->frames;

    /* Inverse transform as the conjugate of a forward transform. */
    for (i = 0; i < LE_VOICE_DEREVERB_FFT; ++i)
        im[i] = -im[i];
    fft(dereverb, re, im);
    for (i = 0; i < LE_VOICE_DEREVERB_FFT; ++i)
        dereverb->overlap[i] +=
            re[i] * (1.0f / (float)LE_VOICE_DEREVERB_FFT) *
            dereverb->window[i];
    for (i = 0; i < LE_VOICE_DEREVERB_HOP; ++i) {
        float value = dereverb->overlap[i];

        value += value < 0.0f ? -0.5f : 0.5f;
        if (value > 32767.0f)
            value = 32767.0f;
        if (value < -32768.0f)
            value = -32768.0f;
        dereverb->output[i] = (int16_t)value;
    }
    memmove(dereverb->overlap, dereverb->overlap + LE_VOICE_DEREVERB_HOP,
            (LE_VOICE_DEREVERB_FFT - LE_VOICE_DEREVERB_HOP) *
                sizeof(dereverb->overlap[0]));
    memset(dereverb->overlap + LE_VOICE_DEREVERB_FFT -
               LE_VOICE_DEREVERB_HOP,
           0, LE_VOICE_DEREVERB_HOP * sizeof(dereverb->overlap[0]));
}

void le_voice_dereverb_reset(struct le_voice_dereverb *dereverb)
{
    if (!dereverb)
        return;
    memset(dereverb->frame, 0, sizeof(dereverb->frame));
    memset(dereverb->overlap, 0, sizeof(dereverb->overlap));
    memset(dereverb->history, 0, sizeof(dereverb->history));
    memset(dereverb->late, 0, sizeof(dereverb->late));
    memset(dereverb->input, 0, sizeof(dereverb->input));
    memset(dereverb->output, 0, sizeof(dereverb->output));
    dereverb->position = 0;
    dereverb->history_position = 0;
    dereverb->history_count = 0;
    dereverb->frames = 0;
}

int le_voice_dereverb_init(struct le_voice_dereverb *dereverb,
                           int attenuation_db)
{
    const double pi = 3.14159265358979323846;
    unsigned int i, bits = 0;

    if (!dereverb ||
        attenuation_db < LE_VOICE_DEREVERB_MIN_ATTENUATION_DB ||
        attenuation_db > LE_VOICE_DEREVERB_MAX_ATTENUATION_DB)
        return -1;
    memset(dereverb, 0, sizeof(*dereverb));
    while ((1U << bits) < LE_VOICE_DEREVERB_FFT)
        ++bits;
    for (i = 0; i < LE_VOICE_DEREVERB_FFT; ++i) {
        unsigned int j = 0, b;

        for (b = 0; b < bits; ++b)
            if (i & (1U << b))
                j |= 1U << (bits - 1U - b);
        dereverb->bit_reverse[i] = (unsigned short)j;
        /* Periodic sqrt-Hann: its square overlap-adds to one at 50 %. */
        dereverb->window[i] = (float)sqrt(
            0.5 - 0.5 * cos(2.0 * pi * (double)i /
                            (double)LE_VOICE_DEREVERB_FFT));
    }
    for (i = 0; i < LE_VOICE_DEREVERB_FFT / 2U; ++i) {
        dereverb->twiddle_re[i] =
            (float)cos(-2.0 * pi * (double)i /
                       (double)LE_VOICE_DEREVERB_FFT);
        dereverb->twiddle_im[i] =
            (float)sin(-2.0 * pi * (double)i /
                       (double)LE_VOICE_DEREVERB_FFT);
    }
    dereverb->floor_gain = (float)pow(
        10.0, -(double)attenuation_db / 20.0);
    le_voice_dereverb_reset(dereverb);
    return 0;
}

void le_voice_dereverb_process(struct le_voice_dereverb *dereverb,
                               int16_t *samples, size_t count)
{
    size_t i;

    if (!dereverb || !samples)
        return;
    for (i = 0; i < count; ++i) {
        int16_t value = samples[i];

        samples[i] = dereverb->output[dereverb->position];
        dereverb->input[dereverb->position] = value;
        if (++dereverb->position == LE_VOICE_DEREVERB_HOP) {
            process_block(dereverb);
            dereverb->position = 0;
        }
    }
}

void le_voice_dereverb_set_attenuation(struct le_voice_dereverb *dereverb,
                                       int attenuation_db)
{
    if (!dereverb)
        return;
    if (attenuation_db < LE_VOICE_DEREVERB_MIN_ATTENUATION_DB)
        attenuation_db = LE_VOICE_DEREVERB_MIN_ATTENUATION_DB;
    if (attenuation_db > LE_VOICE_DEREVERB_MAX_ATTENUATION_DB)
        attenuation_db = LE_VOICE_DEREVERB_MAX_ATTENUATION_DB;
    dereverb->floor_gain = (float)pow(
        10.0, -(double)attenuation_db / 20.0);
}
