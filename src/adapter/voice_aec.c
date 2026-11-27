#include "voice_aec.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <string.h>

static SpeexPreprocessState *create_suppressor(
    SpeexEchoState *echo_state, unsigned int frame_samples)
{
    SpeexPreprocessState *state = speex_preprocess_state_init(
        (int)frame_samples, LE_VOICE_AEC_RATE);
    int enabled = 1;
    int disabled = 0;
    int noise_db = 0;

    if (!state)
        return NULL;
    /*
     * Only residual echo suppression here: noise reduction and AGC are
     * separate stages (le_voice_ns), and the VAD belongs to voice_dsp.
     */
    (void)speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
    /*
     * libspeexdsp only applies its echo gain floor when the preprocessor is
     * "denoising", so keep that flag set but neutralise its noise floor: bins
     * holding only noise keep unity gain, bins holding residual echo are cut.
     * Background noise remains le_voice_ns's job.
     */
    (void)speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                               &noise_db);
    (void)speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC, &disabled);
    (void)speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_ECHO_STATE,
                               echo_state);
    return state;
}

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
    aec->suppressor_state = create_suppressor(state, frame_samples);
    if (aec->suppressor_state) {
        int suppress = LE_VOICE_AEC_RESIDUAL_SUPPRESS_DB;
        int active = LE_VOICE_AEC_RESIDUAL_ACTIVE_DB;

        (void)speex_preprocess_ctl(
            (SpeexPreprocessState *)aec->suppressor_state,
            SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &suppress);
        (void)speex_preprocess_ctl(
            (SpeexPreprocessState *)aec->suppressor_state,
            SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &active);
        aec->residual_echo_enabled = 1;
        aec->residual_suppress_db = suppress;
        aec->residual_active_db = active;
    }
    return 0;
}

int le_voice_aec_set_residual_echo(struct le_voice_aec *aec, int enabled,
                                   int suppress_db, int active_db)
{
    if (!aec)
        return -1;
    if (enabled && (!aec->suppressor_state || suppress_db >= 0 ||
                    active_db >= 0 ||
                    active_db < suppress_db))
        return -1;
    if (!enabled) {
        aec->residual_echo_enabled = 0;
        return 0;
    }
    if (speex_preprocess_ctl((SpeexPreprocessState *)aec->suppressor_state,
                             SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                             &suppress_db) != 0 ||
        speex_preprocess_ctl((SpeexPreprocessState *)aec->suppressor_state,
                             SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                             &active_db) != 0)
        return -1;
    aec->residual_suppress_db = suppress_db;
    aec->residual_active_db = active_db;
    aec->residual_echo_enabled = 1;
    return 0;
}

void le_voice_aec_reset(struct le_voice_aec *aec)
{
    if (!aec || !aec->echo_state)
        return;
    speex_echo_state_reset((SpeexEchoState *)aec->echo_state);
    /*
     * The suppressor has no reset entry point; rebuilding it also drops the
     * old residual statistics with the old filter.
     */
    if (aec->suppressor_state) {
        speex_preprocess_state_destroy(
            (SpeexPreprocessState *)aec->suppressor_state);
        aec->suppressor_state = create_suppressor(
            (SpeexEchoState *)aec->echo_state, aec->frame_samples);
        if (!aec->suppressor_state)
            aec->residual_echo_enabled = 0;
        else if (aec->residual_suppress_db < 0) {
            (void)speex_preprocess_ctl(
                (SpeexPreprocessState *)aec->suppressor_state,
                SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,
                &aec->residual_suppress_db);
            (void)speex_preprocess_ctl(
                (SpeexPreprocessState *)aec->suppressor_state,
                SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                &aec->residual_active_db);
        }
    }
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
    if (aec->residual_echo_enabled && aec->suppressor_state)
        (void)speex_preprocess_run(
            (SpeexPreprocessState *)aec->suppressor_state, output);
}

void le_voice_aec_destroy(struct le_voice_aec *aec)
{
    if (!aec)
        return;
    if (aec->suppressor_state)
        speex_preprocess_state_destroy(
            (SpeexPreprocessState *)aec->suppressor_state);
    if (aec->echo_state)
        speex_echo_state_destroy((SpeexEchoState *)aec->echo_state);
    memset(aec, 0, sizeof(*aec));
}
