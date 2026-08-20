#ifndef LIBREECHO_VOICE_AEC_H
#define LIBREECHO_VOICE_AEC_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_AEC_RATE 16000
#define LE_VOICE_AEC_FRAME_SAMPLES 160
#define LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES 3200

struct le_voice_aec {
    void *echo_state;
    unsigned int frame_samples;
    unsigned int filter_samples;
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

#endif
