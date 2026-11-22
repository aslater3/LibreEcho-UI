#include "voice_aec.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <limits.h>
#include <math.h>
#include <string.h>

int le_voice_aec_init_ex(struct le_voice_aec *aec,
                         unsigned int frame_samples,
                         unsigned int filter_samples,
                         int preprocess)
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
    if (preprocess) {
        SpeexPreprocessState *pre =
            speex_preprocess_state_init((int)frame_samples, sample_rate);
        if (!pre) {
            speex_echo_state_destroy(state);
            return -1;
        }
        int denoise = 1, agc = 1;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC, &agc);
        aec->preprocess_state = pre;
    }
    return 0;
}

int le_voice_aec_init(struct le_voice_aec *aec,
                      unsigned int frame_samples,
                      unsigned int filter_samples)
{
    return le_voice_aec_init_ex(aec, frame_samples, filter_samples, 0);
}

int le_voice_aec_init_suppress(struct le_voice_aec *aec,
                               unsigned int frame_samples,
                               unsigned int filter_samples,
                               int suppress_db, int suppress_active_db)
{
    SpeexPreprocessState *pre;
    int zero = 0;

    if (le_voice_aec_init_ex(aec, frame_samples, filter_samples, 0) < 0)
        return -1;
    pre = speex_preprocess_state_init((int)frame_samples, LE_VOICE_AEC_RATE);
    if (!pre) {
        le_voice_aec_destroy(aec);
        return -1;
    }
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_ECHO_STATE,
                         aec->echo_state);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &zero);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC, &zero);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_VAD, &zero);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DEREVERB, &zero);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                         &suppress_db);
    speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                         &suppress_active_db);
    aec->preprocess_state = pre;
    return 0;
}

int le_voice_aec_set_denoise(struct le_voice_aec *aec, int enabled)
{
    int value = enabled != 0;

    if (!aec || !aec->preprocess_state)
        return -1;
    speex_preprocess_ctl((SpeexPreprocessState *)aec->preprocess_state,
                         SPEEX_PREPROCESS_SET_DENOISE, &value);
    return 0;
}

int le_voice_aec_set_noise_db(struct le_voice_aec *aec, int db)
{
    if (!aec || !aec->preprocess_state)
        return -1;
    speex_preprocess_ctl((SpeexPreprocessState *)aec->preprocess_state,
                         SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &db);
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
    if (aec->preprocess_state)
        speex_preprocess_run((SpeexPreprocessState *)aec->preprocess_state, output);
}

void le_voice_aec_poll(struct le_voice_aec *aec)
{
    /* Runtime reconfiguration is an AEC3-only feature for now. */
    (void)aec;
}

void le_voice_aec_destroy(struct le_voice_aec *aec)
{
    if (!aec)
        return;
    if (aec->preprocess_state)
        speex_preprocess_state_destroy((SpeexPreprocessState *)aec->preprocess_state);
    if (aec->echo_state)
        speex_echo_state_destroy((SpeexEchoState *)aec->echo_state);
    memset(aec, 0, sizeof(*aec));
}
