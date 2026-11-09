#ifndef LIBREECHO_MIC_BEAMFORMER_H
#define LIBREECHO_MIC_BEAMFORMER_H

#include <stdint.h>

#include "voice_dsp.h"

/*
 * Fixed-beam frequency-domain beamformer with beam selection, structured like
 * the stock Radar speech front end: 256-point STFT, 128-sample hop, a set of
 * fixed beams spread evenly in azimuth, and a selector that follows the beam
 * carrying the most speech-band energy.
 *
 * The beams are superdirective (minimum variance against spherically diffuse
 * noise) with diagonal loading chosen per frequency so the white-noise gain
 * never falls below LE_BEAM_WNG_MIN_DB.  They are designed at start-up from
 * mic_array_geometry.h for whichever subset of the seven microphones is
 * selected, so no coefficient file is shipped.  Each beam is distortionless
 * in its look direction, so a talker on a beam axis passes at unity gain.
 *
 * Output is delayed by LE_BEAM_FFT samples (16 ms).
 */
#define LE_BEAM_FFT 256
#define LE_BEAM_HOP 128
#define LE_BEAM_BINS (LE_BEAM_FFT / 2 + 1)
#define LE_BEAM_MAX_BEAMS 12
#define LE_BEAM_DEFAULT_BEAMS 12
#define LE_BEAM_WNG_MIN_DB (-3.0)
/* The stock speech microphones: rear-left, rear-right, front-right, front-left. */
#define LE_BEAM_DEFAULT_MIC_MASK \
    ((1U << 0) | (1U << 1) | (1U << 3) | (1U << 4))
#define LE_BEAM_ALL_MIC_MASK ((1U << LE_VOICE_LOGICAL_CHANNELS) - 1U)

struct le_beamformer {
    unsigned int mic_mask;
    unsigned int mic_count;
    uint8_t mics[LE_VOICE_LOGICAL_CHANNELS];
    unsigned int beam_count;
    int azimuth_deg[LE_BEAM_MAX_BEAMS];
    /* Conjugated weights: Y_b[k] = sum_m w[b][k][m] * X_m[k]. */
    float w_re[LE_BEAM_MAX_BEAMS][LE_BEAM_BINS][LE_VOICE_LOGICAL_CHANNELS];
    float w_im[LE_BEAM_MAX_BEAMS][LE_BEAM_BINS][LE_VOICE_LOGICAL_CHANNELS];
    float window[LE_BEAM_FFT];
    float twiddle_re[LE_BEAM_FFT / 2];
    float twiddle_im[LE_BEAM_FFT / 2];
    float input[LE_VOICE_LOGICAL_CHANNELS][LE_BEAM_FFT];
    float overlap[LE_BEAM_HOP];
    int16_t output[LE_BEAM_HOP];
    unsigned int fill;
    float power[LE_BEAM_MAX_BEAMS];
    float floor[LE_BEAM_MAX_BEAMS];
    float score[LE_BEAM_MAX_BEAMS];
    unsigned int selected;
    unsigned int candidate;
    unsigned int candidate_frames;
    unsigned long switches;
    int locked;              /* -1 = automatic selection, else beam index */
    int primed;
};

/* Returns 0, or -1 when mic_mask or beam_count is unusable. */
int le_beamformer_init(struct le_beamformer *beamformer,
                       unsigned int mic_mask, unsigned int beam_count);

/* One calibrated logical frame in, one beamformed sample out. */
int16_t le_beamformer_sample(struct le_beamformer *beamformer,
                             const int16_t logical[LE_VOICE_LOGICAL_CHANNELS]);

/*
 * Pin the output to the beam nearest azimuth_deg, or pass a negative value to
 * return to automatic selection.  Beam powers keep updating while pinned, so
 * a caller (room measurement, direction finding) can still read them.
 */
void le_beamformer_lock(struct le_beamformer *beamformer, int azimuth_deg);

/* Smoothed speech-band power of beam index b (0 when out of range). */
float le_beamformer_beam_power(const struct le_beamformer *beamformer,
                               unsigned int b);

/* Azimuth of the beam currently selected (see mic_array_geometry.h). */
int le_beamformer_selected_azimuth(const struct le_beamformer *beamformer);

/* Parses "0,1,3,4" or "all" into a microphone mask; returns 0 when invalid. */
unsigned int le_beamformer_parse_mics(const char *text);

/*
 * How micd turns the calibrated lanes into the mono stream.  The beamformer
 * keeps running in every mode so its selection and beam view stay warm; the
 * mode only chooses which signal is sent on.
 */
enum le_mic_mix {
    LE_MIC_MIX_BEAMFORMER = 0,   /* fixed beams, follows the talker (default) */
    LE_MIC_MIX_AVERAGE = 1,      /* mean of the selected microphones */
    LE_MIC_MIX_SINGLE = 2,       /* one microphone: the lowest selected lane */
    LE_MIC_MIX_COUNT = 3
};

/* Parses "beamformer", "average" or "single"; returns -1 for anything else. */
int le_mic_mix_parse(const char *text);

/* The canonical name of a mode, or NULL when out of range. */
const char *le_mic_mix_name(int mode);

/* Mean of the lanes in mic_mask, rounded to nearest; 0 when mic_mask is 0. */
int16_t le_mic_mix_average(const int16_t logical[LE_VOICE_LOGICAL_CHANNELS],
                           unsigned int mic_mask);

/* The lowest-numbered lane in mic_mask, or -1 when mic_mask is 0. */
int le_mic_mix_single_lane(unsigned int mic_mask);

/*
 * The sample for the "single" mode: the chosen lane 0-6, or when lane is -1
 * (automatic) the lowest lane in mic_mask.  An out-of-range lane falls back to
 * the automatic choice rather than reading past the frame.
 */
int16_t le_mic_mix_single(const int16_t logical[LE_VOICE_LOGICAL_CHANNELS],
                          unsigned int mic_mask, int lane);

#endif
