/*
 * Offline driver for the micd voice front end.
 *
 *   beam-eval MICS < capture.s24 > mono.s16
 *
 * Reads the 9-lane packed S24_3LE stream that fake-tinycap (or the real
 * tinycap) produces and runs it through the same stages micd uses for the
 * calibrated mono stream: decode with unity calibration, fixed-beam
 * beamformer over the microphone subset MICS ("0,1,3,4", "all", ...), then
 * the 80 Hz high-pass.  MICS may also be a single lane, which bypasses the
 * beamformer and emits that microphone alone as the reference.
 *
 * Writes the selected beam azimuth per hop to stderr when BEAM_EVAL_TRACE=1.
 * BEAM_EVAL_LOCK_AZIMUTH=deg pins the beam nearest that direction, which
 * separates beam quality from beam-selection quality.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter/mic_beamformer.h"
#include "adapter/voice_dsp.h"

static struct le_beamformer bf;

int main(int argc, char **argv)
{
    static const int unity[LE_VOICE_LOGICAL_CHANNELS] = {
        16384, 16384, 16384, 16384, 16384, 16384, 16384};
    static const uint8_t identity[LE_VOICE_LOGICAL_CHANNELS] = {
        0, 1, 2, 3, 4, 5, 6};
    struct le_voice_calibration calibration;
    struct le_voice_highpass highpass;
    unsigned char raw[LE_VOICE_RAW_FRAME_BYTES];
    unsigned int mask, single = 0;
    int trace = getenv("BEAM_EVAL_TRACE") && !strcmp(getenv("BEAM_EVAL_TRACE"), "1");
    unsigned long frames = 0;

    if (argc != 2 || !(mask = le_beamformer_parse_mics(argv[1]))) {
        fprintf(stderr, "usage: beam-eval MICS  (e.g. 0,1,3,4 | all | 6)\n");
        return 2;
    }
    if (!(mask & (mask - 1U))) {
        while (!(mask & (1U << single)))
            ++single;
    } else if (le_beamformer_init(&bf, mask, LE_BEAM_DEFAULT_BEAMS) < 0) {
        fprintf(stderr, "beam-eval: beamformer init failed\n");
        return 2;
    } else if (getenv("BEAM_EVAL_LOCK_AZIMUTH")) {
        le_beamformer_lock(&bf, atoi(getenv("BEAM_EVAL_LOCK_AZIMUTH")));
    }
    le_voice_calibration_init(&calibration, unity, identity,
                              LE_VOICE_CALIBRATION_DIRECT_Q14);
    le_voice_highpass_init(&highpass);
    while (fread(raw, sizeof(raw), 1, stdin) == 1) {
        int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
        int16_t out;

        le_voice_decode_calibrated(raw, &calibration, logical);
        out = mask & (mask - 1U) ? le_beamformer_sample(&bf, logical)
                                 : logical[single];
        out = le_voice_highpass_sample(&highpass, out);
        if (fwrite(&out, sizeof(out), 1, stdout) != 1)
            return 1;
        if (trace && (mask & (mask - 1U)) && ++frames % LE_BEAM_HOP == 0)
            fprintf(stderr, "%lu %d\n", frames,
                    le_beamformer_selected_azimuth(&bf));
    }
    return 0;
}
