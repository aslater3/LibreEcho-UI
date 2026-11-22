#ifndef LIBREECHO_VOICE_AEC_H
#define LIBREECHO_VOICE_AEC_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_AEC_RATE 16000
#define LE_VOICE_AEC_FRAME_SAMPLES 160
#define LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES 6400

struct le_voice_aec {
    void *echo_state;
    void *preprocess_state;
    unsigned int frame_samples;
    unsigned int filter_samples;
};

#ifdef __cplusplus
extern "C" {
#endif

int le_voice_aec_init(struct le_voice_aec *aec,
                      unsigned int frame_samples,
                      unsigned int filter_samples);
int le_voice_aec_init_ex(struct le_voice_aec *aec,
                         unsigned int frame_samples,
                         unsigned int filter_samples,
                         int preprocess);
/*
 * Like le_voice_aec_init_ex, but attaches a bare preprocess stage in
 * echo-suppression-only mode (no denoise, no agc): residual echo is pushed
 * suppress_db down while no near-end speech is detected and
 * suppress_active_db while speech is present.
 */
int le_voice_aec_init_suppress(struct le_voice_aec *aec,
                               unsigned int frame_samples,
                               unsigned int filter_samples,
                               int suppress_db, int suppress_active_db);
/* Enables the Speex preprocess denoiser on the attached stage (Speex
 * variant).  Returns -1 when the backend has no preprocess stage. */
int le_voice_aec_set_denoise(struct le_voice_aec *aec, int enabled);
/* Denoiser depth in dB (Speex NOISE_SUPPRESS, default -15; less negative
 * is gentler).  Returns -1 without a preprocess stage. */
int le_voice_aec_set_noise_db(struct le_voice_aec *aec, int db);
void le_voice_aec_reset(struct le_voice_aec *aec);
void le_voice_aec_process(struct le_voice_aec *aec,
                          const int16_t *microphone,
                          const int16_t *reference,
                          int16_t *output);
void le_voice_aec_destroy(struct le_voice_aec *aec);

/* Re-read the live config file (LE_AEC_CONF) and re-apply changed settings.
 * Called periodically by the capture loop; ~1 s granularity. */
void le_voice_aec_poll(struct le_voice_aec *aec);

#ifdef __cplusplus
}
#endif

#endif
