#ifndef LIBREECHO_VOICE_AEC_H
#define LIBREECHO_VOICE_AEC_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_AEC_RATE 16000
#define LE_VOICE_AEC_FRAME_SAMPLES 160
#define LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES 3200
/* Stock-style extra suppression: deep when only echo remains, shallow while
 * near-end speech is present so barge-in survives. */
#define LE_VOICE_AEC_RESIDUAL_SUPPRESS_DB (-40)
#define LE_VOICE_AEC_RESIDUAL_ACTIVE_DB (-10)

struct le_voice_aec {
    void *echo_state;
    /* Residual echo suppressor state (libspeexdsp preprocessor), run on the
     * canceller output. NULL when unavailable. */
    void *suppressor_state;
    unsigned int frame_samples;
    unsigned int filter_samples;
    int residual_echo_enabled;
    int residual_suppress_db;
    int residual_active_db;
};

int le_voice_aec_init(struct le_voice_aec *aec,
                      unsigned int frame_samples,
                      unsigned int filter_samples);
void le_voice_aec_reset(struct le_voice_aec *aec);
void le_voice_aec_process(struct le_voice_aec *aec,
                          const int16_t *microphone,
                          const int16_t *reference,
                          int16_t *output);
void le_voice_aec_destroy(struct le_voice_aec *aec);

/*
 * Configure the residual echo suppressor that runs after the canceller, the
 * extra-suppression stage the stock AEC has. enabled=0 disables it and leaves
 * the canceller output untouched. suppress_db is the maximum cut when only
 * echo remains; active_db is the shallower cut while near-end speech is
 * present (both negative dB). Returns 0, or -1 when the values or the
 * libspeexdsp state are unusable.
 */
int le_voice_aec_set_residual_echo(struct le_voice_aec *aec, int enabled,
                                   int suppress_db, int active_db);

#endif
