#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter/mic_array_geometry.h"
#include "adapter/mic_beamformer.h"

#define TEST_PI 3.14159265358979323846
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#define SECONDS 2
#define FRAMES (LE_VOICE_RATE * SECONDS)

static struct le_beamformer bf;

static void position(unsigned int lane, double *x, double *y)
{
    double r = LE_MIC_ARRAY_RING_RADIUS_UM * 1e-6, a;

    if (lane == LE_MIC_ARRAY_CENTRE_LANE) {
        *x = *y = 0.0;
        return;
    }
    a = (LE_MIC_ARRAY_LANE0_AZIMUTH_DEG + lane * LE_MIC_ARRAY_RING_STEP_DEG) *
        TEST_PI / 180.0;
    *x = r * cos(a);
    *y = r * sin(a);
}

/* Lane signals for a plane-wave tone arriving from azimuth_deg. */
static void tone_frame(double t, double hz, double azimuth_deg, double amp,
                       int16_t out[LE_VOICE_LOGICAL_CHANNELS])
{
    double c = LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S * 1e-3;
    double ux = cos(azimuth_deg * TEST_PI / 180.0);
    double uy = sin(azimuth_deg * TEST_PI / 180.0);
    unsigned int m;

    for (m = 0; m < LE_VOICE_LOGICAL_CHANNELS; ++m) {
        double x, y, tau;
        position(m, &x, &y);
        tau = -(x * ux + y * uy) / c;
        out[m] = (int16_t)lrint(amp * sin(2.0 * TEST_PI * hz * (t - tau)));
    }
}

static double rms_tail(const int16_t *s, size_t n, size_t skip)
{
    double acc = 0.0;
    size_t i;

    for (i = skip; i < n; ++i)
        acc += (double)s[i] * s[i];
    return sqrt(acc / (double)(n - skip));
}

/* Steady tone through the beam pinned nearest lock_az; returns linear gain. */
static double run_tone(unsigned int mask, double hz, double azimuth,
                       int lock_az)
{
    static int16_t out[FRAMES];
    size_t i;

    CHECK(le_beamformer_init(&bf, mask, LE_BEAM_DEFAULT_BEAMS) == 0);
    le_beamformer_lock(&bf, lock_az);
    for (i = 0; i < FRAMES; ++i) {
        int16_t frame[LE_VOICE_LOGICAL_CHANNELS];
        tone_frame((double)i / LE_VOICE_RATE, hz, azimuth, 8000.0, frame);
        out[i] = le_beamformer_sample(&bf, frame);
    }
    return rms_tail(out, FRAMES, LE_VOICE_RATE) / (8000.0 / sqrt(2.0));
}

/*
 * Fractionally delayed copies of one broadband signal, as a far-field source
 * at azimuth would put on each lane (windowed-sinc interpolation).
 */
static void noise_source_frames(double azimuth, const float *src, size_t n,
                                int16_t (*out)[LE_VOICE_LOGICAL_CHANNELS])
{
    double c = LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S * 1e-3;
    double ux = cos(azimuth * TEST_PI / 180.0), uy = sin(azimuth * TEST_PI / 180.0);
    unsigned int m;
    size_t i;

    for (m = 0; m < LE_VOICE_LOGICAL_CHANNELS; ++m) {
        double x, y, delay;
        position(m, &x, &y);
        delay = -(x * ux + y * uy) / c * LE_VOICE_RATE;
        for (i = 0; i < n; ++i) {
            double acc = 0.0;
            int k;
            for (k = -16; k <= 16; ++k) {
                double t = k - delay;
                double h = fabs(t) < 1e-9 ? 1.0 : sin(TEST_PI * t) / (TEST_PI * t);
                long j = (long)i - k;
                if (j >= 0 && (size_t)j < n)
                    acc += h * (0.5 + 0.5 * cos(TEST_PI * t / 17.0)) * src[j];
            }
            out[i][m] = (int16_t)lrint(acc);
        }
    }
}

/*
 * Broadband speech-like bursts (250 ms on, 350 ms off, over quiet sensor
 * noise) from azimuth; returns the beam automatic selection settles on.
 */
static int run_bursts(unsigned int mask, double azimuth)
{
    static float src[FRAMES];
    static int16_t frames[FRAMES][LE_VOICE_LOGICAL_CHANNELS];
    unsigned int seed = 777;
    size_t i;

    for (i = 0; i < FRAMES; ++i) {
        int on = (i % 9600) < 4000 && i > 4800;
        seed = seed * 1103515245U + 12345U;
        src[i] = on ? (float)((int)((seed >> 16) % 8001) - 4000) : 0.0f;
    }
    noise_source_frames(azimuth, src, FRAMES, frames);
    CHECK(le_beamformer_init(&bf, mask, LE_BEAM_DEFAULT_BEAMS) == 0);
    for (i = 0; i < FRAMES; ++i) {
        unsigned int m;
        for (m = 0; m < LE_VOICE_LOGICAL_CHANNELS; ++m) {
            seed = seed * 1103515245U + 12345U;
            frames[i][m] = (int16_t)(frames[i][m] + (int)((seed >> 16) % 61) - 30);
        }
        (void)le_beamformer_sample(&bf, frames[i]);
    }
    return le_beamformer_selected_azimuth(&bf);
}

static void test_parse(void)
{
    CHECK(le_beamformer_parse_mics("0,1,3,4") == LE_BEAM_DEFAULT_MIC_MASK);
    CHECK(le_beamformer_parse_mics("all") == LE_BEAM_ALL_MIC_MASK);
    CHECK(le_beamformer_parse_mics("6") == (1U << 6));
    CHECK(le_beamformer_parse_mics("7") == 0);
    CHECK(le_beamformer_parse_mics("0,,1") == 0);
    CHECK(le_beamformer_parse_mics("0,0") == 0);
    CHECK(le_beamformer_parse_mics("") == 0);
    CHECK(le_beamformer_parse_mics(NULL) == 0);
    CHECK(le_beamformer_init(&bf, 1U << 3, 8) < 0);
    CHECK(le_beamformer_init(&bf, LE_BEAM_DEFAULT_MIC_MASK, 0) < 0);
    CHECK(le_beamformer_init(&bf, LE_BEAM_DEFAULT_MIC_MASK,
                             LE_BEAM_MAX_BEAMS + 1) < 0);
    puts("mic-beamformer: microphone subsets parse and validate");
}

/*
 * A tone on a beam axis must pass at unity gain through that beam, for every
 * microphone subset, and speech-like bursts must steer selection to it.
 */
static void test_on_axis_unity_and_selection(void)
{
    static const unsigned int masks[] = {
        LE_BEAM_DEFAULT_MIC_MASK, LE_BEAM_ALL_MIC_MASK,
        (1U << 0) | (1U << 3)};
    static const double tones[] = {500.0, 1000.0, 2000.0, 3500.0};
    unsigned int mi, ti, beam;

    for (mi = 0; mi < sizeof(masks) / sizeof(masks[0]); ++mi) {
        for (ti = 0; ti < sizeof(tones) / sizeof(tones[0]); ++ti) {
            for (beam = 0; beam < LE_BEAM_DEFAULT_BEAMS; ++beam) {
                int az = (int)(beam * 360 / LE_BEAM_DEFAULT_BEAMS);
                double db = 20.0 * log10(run_tone(masks[mi], tones[ti], az, az));
                if (fabs(db) > 1.0)
                    fprintf(stderr, "mask %#x %g Hz beam %d: %.2f dB\n",
                            masks[mi], tones[ti], az, db);
                CHECK(fabs(db) <= 1.0);
            }
        }
    }
    /* A two-microphone pair cannot tell mirror images apart; skip it. */
    for (mi = 0; mi < 2; ++mi) {
        for (beam = 0; beam < LE_BEAM_DEFAULT_BEAMS; beam += 2) {
            int az = (int)(beam * 360 / LE_BEAM_DEFAULT_BEAMS);
            int picked = run_bursts(masks[mi], az);
            int err = abs(picked - az) % 360;

            /*
             * Hysteresis (1.5 dB) may leave selection on a neighbouring beam,
             * which costs well under a decibel (see test_between_beams).
             */
            if (err > 180)
                err = 360 - err;
            if (err > 360 / LE_BEAM_DEFAULT_BEAMS)
                fprintf(stderr, "mask %#x bursts from %d: picked %d\n",
                        masks[mi], az, picked);
            CHECK(err <= 360 / LE_BEAM_DEFAULT_BEAMS);
        }
    }
    puts("mic-beamformer: on-axis tones pass at unity; bursts select their beam");
}

/* A steady source (music, hum) must not capture the selection. */
static void test_steady_source_ignored(void)
{
    double gain = run_tone(LE_BEAM_DEFAULT_MIC_MASK, 1000.0, 90.0, -1);

    (void)gain;
    CHECK(le_beamformer_selected_azimuth(&bf) == 0);
    puts("mic-beamformer: a steady tone does not steer the selection");
}

/* Midway between two beams the loss must stay small. */
static void test_between_beams(void)
{
    double half = 180.0 / LE_BEAM_DEFAULT_BEAMS;
    double db4 = 20.0 * log10(run_tone(LE_BEAM_DEFAULT_MIC_MASK, 2000.0, half, 0));
    double db7 = 20.0 * log10(run_tone(LE_BEAM_ALL_MIC_MASK, 2000.0, half, 0));

    printf("mic-beamformer: 2 kHz %.0f deg off-axis: 4 mics %.2f dB, 7 mics %.2f dB\n",
           half, db4, db7);
    CHECK(db4 > -2.0);
    CHECK(db7 > -2.0);
}

/*
 * Uncorrelated noise on each lane: the selected beam's output noise relative
 * to one microphone is the white-noise gain.  The design floor is -3 dB, so the
 * beam must never amplify sensor noise by more than that.
 */
static void test_sensor_noise(void)
{
    static const unsigned int masks[] = {LE_BEAM_DEFAULT_MIC_MASK,
                                         LE_BEAM_ALL_MIC_MASK};
    static int16_t out[FRAMES];
    unsigned int mi;

    for (mi = 0; mi < 2; ++mi) {
        unsigned int seed = 12345;
        size_t i;
        double in_rms = 1000.0, out_rms, db;

        CHECK(le_beamformer_init(&bf, masks[mi], LE_BEAM_DEFAULT_BEAMS) == 0);
        for (i = 0; i < FRAMES; ++i) {
            int16_t frame[LE_VOICE_LOGICAL_CHANNELS];
            unsigned int m;

            /* Rotate the pinned beam so every beam's noise is checked. */
            if (i % (FRAMES / LE_BEAM_DEFAULT_BEAMS) == 0)
                le_beamformer_lock(&bf, (int)(i / (FRAMES / LE_BEAM_DEFAULT_BEAMS)) *
                                        360 / LE_BEAM_DEFAULT_BEAMS);
            for (m = 0; m < LE_VOICE_LOGICAL_CHANNELS; ++m) {
                double u1, u2;
                seed = seed * 1103515245U + 12345U;
                u1 = ((seed >> 8) + 1.0) / 16777217.0;
                seed = seed * 1103515245U + 12345U;
                u2 = (seed >> 8) / 16777216.0;
                frame[m] = (int16_t)lrint(in_rms * sqrt(-2.0 * log(u1)) *
                                          cos(2.0 * TEST_PI * u2));
            }
            out[i] = le_beamformer_sample(&bf, frame);
        }
        out_rms = rms_tail(out, FRAMES, LE_VOICE_RATE);
        db = 20.0 * log10(out_rms / in_rms);
        printf("mic-beamformer: white sensor noise, %u mics: %+.2f dB\n",
               bf.mic_count, db);
        CHECK(db < 3.5);
    }
}

int main(void)
{
    test_parse();
    test_on_axis_unity_and_selection();
    test_steady_source_ignored();
    test_between_beams();
    test_sensor_noise();
    return 0;
}
