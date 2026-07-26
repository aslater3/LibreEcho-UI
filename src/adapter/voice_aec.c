#include "voice_aec.h"

#include <speex/speex_echo.h>

#include <string.h>

int le_voice_aec_init(struct le_voice_aec *aec,
                      unsigned int frame_samples,
                      unsigned int filter_samples)
{
    SpeexEchoState *state;
    int sample_rate = LE_VOICE_AEC_RATE;

    if (!aec || frame_samples == 0 || filter_samples < frame_samples)
        return -1;
    memset(aec, 0, sizeof(*aec));
    state = speex_echo_state_init((int)frame_samples, (int)filter_samples);
    if (!state)
        return -1;
    if (speex_echo_ctl(state, SPEEX_ECHO_SET_SAMPLING_RATE,
                       &sample_rate) != 0) {
        speex_echo_state_destroy(state);
        return -1;
    }
    aec->echo_state = state;
    aec->frame_samples = frame_samples;
    aec->filter_samples = filter_samples;
    return 0;
}

void le_voice_aec_reset(struct le_voice_aec *aec)
{
    if (aec && aec->echo_state)
        speex_echo_state_reset((SpeexEchoState *)aec->echo_state);
}

void le_voice_aec_process(struct le_voice_aec *aec,
                          const int16_t *microphone,
                          const int16_t *reference,
                          int16_t *output)
{
    if (!aec || !aec->echo_state || !microphone || !reference || !output)
        return;
    speex_echo_cancellation((SpeexEchoState *)aec->echo_state,
                            microphone, reference, output);
}

void le_voice_aec_destroy(struct le_voice_aec *aec)
{
    if (!aec)
        return;
    if (aec->echo_state)
        speex_echo_state_destroy((SpeexEchoState *)aec->echo_state);
    memset(aec, 0, sizeof(*aec));
}
