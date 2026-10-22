#include "mic_beamformer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mic_array_geometry.h"

#define BEAM_PI 3.14159265358979323846
/* Speech band used to score beams: bins 5..64 are 312 Hz to 4 kHz. */
#define BEAM_SCORE_FIRST_BIN 5
#define BEAM_SCORE_LAST_BIN 64
/* Per-hop power smoothing (one hop is 8 ms). */
#define BEAM_POWER_SMOOTHING 0.8f
/*
 * Each beam keeps a noise floor that falls at once and rises 3 dB per second
 * (minimum tracking).  Beams are scored by power over floor, a running SNR,
 * so a talker's bursts win over a loud steady source such as music from the
 * device's own speaker, which raises its beam's floor as well as its power.
 * The score is smoothed over ~200 ms.
 */
#define BEAM_FLOOR_RISE 1.00554f
#define BEAM_SCORE_SMOOTHING 0.96f
/* A rival beam must score 1.5 dB higher for 12 hops (96 ms) to take over. */
#define BEAM_SWITCH_RATIO 1.4125f
#define BEAM_SWITCH_HOPS 12U
/*
 * Only a beam that is clearly carrying something above its floor (3 dB) may
 * take over.  Between utterances every beam sits at its floor, and without
 * this gate the selection would random-walk and greet the next utterance
 * pointing the wrong way; with it, the beam stays on the last talker.
 */
#define BEAM_ACTIVE_SCORE 2.0f

struct cplx { double re, im; };

static void mic_position(unsigned int lane, double *x, double *y)
{
    double radius = LE_MIC_ARRAY_RING_RADIUS_UM * 1e-6;
    double angle;

    if (lane == LE_MIC_ARRAY_CENTRE_LANE) {
        *x = 0.0;
        *y = 0.0;
        return;
    }
    angle = (LE_MIC_ARRAY_LANE0_AZIMUTH_DEG +
             (double)lane * LE_MIC_ARRAY_RING_STEP_DEG) * BEAM_PI / 180.0;
    *x = radius * cos(angle);
    *y = radius * sin(angle);
}

/*
 * Solve A x = b for a small dense complex system by Gaussian elimination with
 * partial pivoting.  A and b are overwritten.  Returns -1 when singular.
 */
static int solve(struct cplx a[LE_VOICE_LOGICAL_CHANNELS]
                              [LE_VOICE_LOGICAL_CHANNELS],
                 struct cplx b[LE_VOICE_LOGICAL_CHANNELS], unsigned int n)
{
    unsigned int col, row, k;

    for (col = 0; col < n; ++col) {
        unsigned int pivot = col;
        double best = a[col][col].re * a[col][col].re +
                      a[col][col].im * a[col][col].im;
        struct cplx inv;
        double norm;

        for (row = col + 1; row < n; ++row) {
            double mag = a[row][col].re * a[row][col].re +
                         a[row][col].im * a[row][col].im;
            if (mag > best) {
                best = mag;
                pivot = row;
            }
        }
        if (best < 1e-30)
            return -1;
        if (pivot != col) {
            struct cplx t;
            for (k = 0; k < n; ++k) {
                t = a[col][k]; a[col][k] = a[pivot][k]; a[pivot][k] = t;
            }
            t = b[col]; b[col] = b[pivot]; b[pivot] = t;
        }
        norm = a[col][col].re * a[col][col].re +
               a[col][col].im * a[col][col].im;
        inv.re = a[col][col].re / norm;
        inv.im = -a[col][col].im / norm;
        for (row = col + 1; row < n; ++row) {
            struct cplx f;
            f.re = a[row][col].re * inv.re - a[row][col].im * inv.im;
            f.im = a[row][col].re * inv.im + a[row][col].im * inv.re;
            for (k = col; k < n; ++k) {
                a[row][k].re -= f.re * a[col][k].re - f.im * a[col][k].im;
                a[row][k].im -= f.re * a[col][k].im + f.im * a[col][k].re;
            }
            b[row].re -= f.re * b[col].re - f.im * b[col].im;
            b[row].im -= f.re * b[col].im + f.im * b[col].re;
        }
    }
    for (row = n; row-- > 0;) {
        struct cplx s = b[row];
        double norm;
        for (k = row + 1; k < n; ++k) {
            s.re -= a[row][k].re * b[k].re - a[row][k].im * b[k].im;
            s.im -= a[row][k].re * b[k].im + a[row][k].im * b[k].re;
        }
        norm = a[row][row].re * a[row][row].re +
               a[row][row].im * a[row][row].im;
        b[row].re = (s.re * a[row][row].re + s.im * a[row][row].im) / norm;
        b[row].im = (s.im * a[row][row].re - s.re * a[row][row].im) / norm;
    }
    return 0;
}

/*
 * MVDR weights against diffuse noise loaded by mu, for steering vector d:
 * w = (G + mu I)^-1 d / (d^H (G + mu I)^-1 d).  Returns the white-noise gain
 * 1/|w|^2, or a negative value when the system is singular.
 */
static double mvdr(double gamma[LE_VOICE_LOGICAL_CHANNELS]
                                     [LE_VOICE_LOGICAL_CHANNELS],
                   const struct cplx d[LE_VOICE_LOGICAL_CHANNELS],
                   unsigned int n, double mu,
                   struct cplx w[LE_VOICE_LOGICAL_CHANNELS])
{
    struct cplx a[LE_VOICE_LOGICAL_CHANNELS][LE_VOICE_LOGICAL_CHANNELS];
    struct cplx denom = {0.0, 0.0};
    double norm, energy = 0.0;
    unsigned int i, j;

    for (i = 0; i < n; ++i) {
        for (j = 0; j < n; ++j) {
            a[i][j].re = gamma[i][j] + (i == j ? mu : 0.0);
            a[i][j].im = 0.0;
        }
        w[i] = d[i];
    }
    if (solve(a, w, n) < 0)
        return -1.0;
    for (i = 0; i < n; ++i) {
        /* d^H x */
        denom.re += d[i].re * w[i].re + d[i].im * w[i].im;
        denom.im += d[i].re * w[i].im - d[i].im * w[i].re;
    }
    norm = denom.re * denom.re + denom.im * denom.im;
    if (norm < 1e-30)
        return -1.0;
    for (i = 0; i < n; ++i) {
        struct cplx v = w[i];
        w[i].re = (v.re * denom.re + v.im * denom.im) / norm;
        w[i].im = (v.im * denom.re - v.re * denom.im) / norm;
        energy += w[i].re * w[i].re + w[i].im * w[i].im;
    }
    return energy > 0.0 ? 1.0 / energy : -1.0;
}

static void steering(const double px[], const double py[], unsigned int n,
                     double frequency, double azimuth,
                     struct cplx d[LE_VOICE_LOGICAL_CHANNELS])
{
    double c = LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S * 1e-3;
    double ux = cos(azimuth), uy = sin(azimuth);
    unsigned int m;

    for (m = 0; m < n; ++m) {
        /* A plane wave from the look direction reaches a microphone further
           along it earlier: arrival delay is -(p . u) / c. */
        double tau = -(px[m] * ux + py[m] * uy) / c;
        double phase = -2.0 * BEAM_PI * frequency * tau;
        d[m].re = cos(phase);
        d[m].im = sin(phase);
    }
}

static int design(struct le_beamformer *bf)
{
    double px[LE_VOICE_LOGICAL_CHANNELS], py[LE_VOICE_LOGICAL_CHANNELS];
    double gamma[LE_VOICE_LOGICAL_CHANNELS][LE_VOICE_LOGICAL_CHANNELS];
    double wng_min = pow(10.0, LE_BEAM_WNG_MIN_DB / 10.0);
    double c = LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S * 1e-3;
    unsigned int n = bf->mic_count, k, b, i, j;

    for (i = 0; i < n; ++i)
        mic_position(bf->mics[i], &px[i], &py[i]);
    for (k = 0; k < LE_BEAM_BINS; ++k) {
        double frequency = (double)k * LE_VOICE_RATE / LE_BEAM_FFT;
        struct cplx d[LE_VOICE_LOGICAL_CHANNELS], w[LE_VOICE_LOGICAL_CHANNELS];

        /* Spherically diffuse coherence: sin(x)/x with x = 2 pi f r / c. */
        for (i = 0; i < n; ++i) {
            for (j = 0; j < n; ++j) {
                double r = hypot(px[i] - px[j], py[i] - py[j]);
                double x = 2.0 * BEAM_PI * frequency * r / c;
                gamma[i][j] = x < 1e-9 ? 1.0 : sin(x) / x;
            }
        }
        /*
         * For each beam, pick the smallest loading that keeps white-noise
         * gain above the floor.  Beams are not all alike: a four-microphone
         * square only repeats every 90 degrees, so a beam aimed at a
         * microphone needs different loading from one aimed between two.
         */
        for (b = 0; b < bf->beam_count; ++b) {
            double lo = 1e-6, hi = 1e3;
            int step;

            steering(px, py, n, frequency,
                     bf->azimuth_deg[b] * BEAM_PI / 180.0, d);
            if (mvdr(gamma, d, n, lo, w) >= wng_min) {
                hi = lo;
            } else {
                for (step = 0; step < 40; ++step) {
                    double mid = sqrt(lo * hi);
                    double wng = mvdr(gamma, d, n, mid, w);
                    if (wng >= wng_min)
                        hi = mid;
                    else
                        lo = mid;
                }
            }
            if (mvdr(gamma, d, n, hi, w) < 0.0)
                return -1;
            for (i = 0; i < n; ++i) {
                /* Store conj(w) so the hot loop is a plain multiply-add. */
                bf->w_re[b][k][i] = (float)w[i].re;
                bf->w_im[b][k][i] = (float)-w[i].im;
            }
        }
    }
    return 0;
}

static void fft(const struct le_beamformer *bf, float re[], float im[])
{
    unsigned int i, j = 0, len;

    for (i = 1; i < LE_BEAM_FFT; ++i) {
        unsigned int bit = LE_BEAM_FFT >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= LE_BEAM_FFT; len <<= 1) {
        unsigned int half = len >> 1, stride = LE_BEAM_FFT / len;
        for (i = 0; i < LE_BEAM_FFT; i += len) {
            for (j = 0; j < half; ++j) {
                float wr = bf->twiddle_re[j * stride];
                float wi = bf->twiddle_im[j * stride];
                unsigned int p = i + j, q = p + half;
                float tr = re[q] * wr - im[q] * wi;
                float ti = re[q] * wi + im[q] * wr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
            }
        }
    }
}

int le_beamformer_init(struct le_beamformer *bf,
                       unsigned int mic_mask, unsigned int beam_count)
{
    unsigned int i;

    memset(bf, 0, sizeof(*bf));
    if (beam_count == 0 || beam_count > LE_BEAM_MAX_BEAMS)
        return -1;
    mic_mask &= LE_BEAM_ALL_MIC_MASK;
    for (i = 0; i < LE_VOICE_LOGICAL_CHANNELS; ++i)
        if (mic_mask & (1U << i))
            bf->mics[bf->mic_count++] = (uint8_t)i;
    if (bf->mic_count < 2)
        return -1;
    bf->mic_mask = mic_mask;
    bf->beam_count = beam_count;
    bf->locked = -1;
    for (i = 0; i < beam_count; ++i)
        bf->azimuth_deg[i] = (int)(i * 360U / beam_count);
    for (i = 0; i < LE_BEAM_FFT; ++i)
        /* Periodic sqrt-Hann for analysis and synthesis: at 50% overlap the
           product sums to exactly one, so a pass-through reconstructs. */
        bf->window[i] = (float)sqrt(0.5 - 0.5 * cos(2.0 * BEAM_PI * i /
                                                   LE_BEAM_FFT));
    for (i = 0; i < LE_BEAM_FFT / 2; ++i) {
        bf->twiddle_re[i] = (float)cos(-2.0 * BEAM_PI * i / LE_BEAM_FFT);
        bf->twiddle_im[i] = (float)sin(-2.0 * BEAM_PI * i / LE_BEAM_FFT);
    }
    return design(bf);
}

static void process_hop(struct le_beamformer *bf)
{
    static float xr[LE_VOICE_LOGICAL_CHANNELS][LE_BEAM_BINS];
    static float xi[LE_VOICE_LOGICAL_CHANNELS][LE_BEAM_BINS];
    float re[LE_BEAM_FFT], im[LE_BEAM_FFT];
    float yr[LE_BEAM_BINS], yi[LE_BEAM_BINS];
    unsigned int m, k, b, best = 0;

    for (m = 0; m < bf->mic_count; ++m) {
        for (k = 0; k < LE_BEAM_FFT; ++k) {
            re[k] = bf->input[m][k] * bf->window[k];
            im[k] = 0.0f;
        }
        fft(bf, re, im);
        for (k = 0; k < LE_BEAM_BINS; ++k) {
            xr[m][k] = re[k];
            xi[m][k] = im[k];
        }
    }
    for (b = 0; b < bf->beam_count; ++b) {
        float energy = 0.0f;
        for (k = BEAM_SCORE_FIRST_BIN; k <= BEAM_SCORE_LAST_BIN; ++k) {
            float sr = 0.0f, si = 0.0f;
            for (m = 0; m < bf->mic_count; ++m) {
                sr += bf->w_re[b][k][m] * xr[m][k] - bf->w_im[b][k][m] * xi[m][k];
                si += bf->w_re[b][k][m] * xi[m][k] + bf->w_im[b][k][m] * xr[m][k];
            }
            energy += sr * sr + si * si;
        }
        if (!bf->primed) {
            /* Start every beam at its first measurement, scored 0 dB. */
            bf->power[b] = bf->floor[b] = energy;
            bf->score[b] = 1.0f;
        }
        bf->power[b] = BEAM_POWER_SMOOTHING * bf->power[b] +
                       (1.0f - BEAM_POWER_SMOOTHING) * energy;
        if (bf->floor[b] <= 0.0f || bf->power[b] < bf->floor[b])
            bf->floor[b] = bf->power[b];
        else
            bf->floor[b] *= BEAM_FLOOR_RISE;
        bf->score[b] = BEAM_SCORE_SMOOTHING * bf->score[b] +
                       (1.0f - BEAM_SCORE_SMOOTHING) *
                       (bf->power[b] / (bf->floor[b] + 1e-3f));
        if (bf->score[b] > bf->score[best])
            best = b;
    }
    bf->primed = 1;
    if (bf->locked >= 0) {
        bf->selected = (unsigned int)bf->locked;
        bf->candidate_frames = 0;
    } else if (best != bf->selected && bf->score[best] > BEAM_ACTIVE_SCORE &&
        bf->score[best] > bf->score[bf->selected] * BEAM_SWITCH_RATIO) {
        if (best == bf->candidate) {
            if (++bf->candidate_frames >= BEAM_SWITCH_HOPS) {
                bf->selected = best;
                bf->candidate_frames = 0;
                ++bf->switches;
            }
        } else {
            bf->candidate = best;
            bf->candidate_frames = 1;
        }
    } else {
        bf->candidate_frames = 0;
    }
    b = bf->selected;
    for (k = 0; k < LE_BEAM_BINS; ++k) {
        float sr = 0.0f, si = 0.0f;
        for (m = 0; m < bf->mic_count; ++m) {
            sr += bf->w_re[b][k][m] * xr[m][k] - bf->w_im[b][k][m] * xi[m][k];
            si += bf->w_re[b][k][m] * xi[m][k] + bf->w_im[b][k][m] * xr[m][k];
        }
        yr[k] = sr;
        yi[k] = si;
    }
    /* Inverse real FFT via the conjugate trick on the Hermitian spectrum. */
    for (k = 0; k < LE_BEAM_BINS; ++k) {
        re[k] = yr[k];
        im[k] = -yi[k];
    }
    im[0] = 0.0f;
    im[LE_BEAM_FFT / 2] = 0.0f;
    for (k = LE_BEAM_BINS; k < LE_BEAM_FFT; ++k) {
        re[k] = yr[LE_BEAM_FFT - k];
        im[k] = yi[LE_BEAM_FFT - k];
    }
    fft(bf, re, im);
    for (k = 0; k < LE_BEAM_FFT; ++k)
        re[k] *= bf->window[k] / LE_BEAM_FFT;
    for (k = 0; k < LE_BEAM_HOP; ++k) {
        float v = bf->overlap[k] + re[k];
        long rounded = lrintf(v);
        if (rounded > INT16_MAX) rounded = INT16_MAX;
        if (rounded < INT16_MIN) rounded = INT16_MIN;
        bf->output[k] = (int16_t)rounded;
        bf->overlap[k] = re[k + LE_BEAM_HOP];
    }
}

int16_t le_beamformer_sample(struct le_beamformer *bf,
                             const int16_t logical[LE_VOICE_LOGICAL_CHANNELS])
{
    unsigned int pos = LE_BEAM_HOP + bf->fill, m;
    int16_t out = bf->output[bf->fill];

    for (m = 0; m < bf->mic_count; ++m)
        bf->input[m][pos] = (float)logical[bf->mics[m]];
    if (++bf->fill == LE_BEAM_HOP) {
        process_hop(bf);
        for (m = 0; m < bf->mic_count; ++m)
            memmove(bf->input[m], bf->input[m] + LE_BEAM_HOP,
                    LE_BEAM_HOP * sizeof(float));
        bf->fill = 0;
    }
    return out;
}

void le_beamformer_lock(struct le_beamformer *bf, int azimuth_deg)
{
    unsigned int b, best = 0;
    int best_err = 360;

    if (azimuth_deg < 0) {
        bf->locked = -1;
        return;
    }
    azimuth_deg %= 360;
    for (b = 0; b < bf->beam_count; ++b) {
        int err = abs(bf->azimuth_deg[b] - azimuth_deg);
        if (err > 180)
            err = 360 - err;
        if (err < best_err) {
            best_err = err;
            best = b;
        }
    }
    bf->locked = (int)best;
    bf->selected = best;
}

float le_beamformer_beam_power(const struct le_beamformer *bf, unsigned int b)
{
    return b < bf->beam_count ? bf->power[b] : 0.0f;
}

int le_beamformer_selected_azimuth(const struct le_beamformer *bf)
{
    return bf->azimuth_deg[bf->selected];
}

unsigned int le_beamformer_parse_mics(const char *text)
{
    unsigned int mask = 0;
    const char *p = text;

    if (!text || !*text)
        return 0;
    if (!strcmp(text, "all"))
        return LE_BEAM_ALL_MIC_MASK;
    while (*p) {
        if (*p < '0' || *p >= '0' + LE_VOICE_LOGICAL_CHANNELS)
            return 0;
        if (mask & (1U << (*p - '0')))
            return 0;
        mask |= 1U << (*p - '0');
        ++p;
        if (*p == ',')
            ++p;
        else if (*p)
            return 0;
    }
    return mask;
}
