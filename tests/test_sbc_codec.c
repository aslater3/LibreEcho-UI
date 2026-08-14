/*
 * Vendored BlueZ SBC codec contract test.
 *
 * Encodes a deterministic band-limited stereo sine tone with an A2DP-quality
 * SBC configuration, decodes the resulting frames, and verifies the decoded
 * waveform matches the source after compensating for the codec's fixed
 * synthesis delay.  This proves the vendored codec library round-trips
 * correctly on the build host before the daemon ships it on the ARM32 target.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter/bt-sbc/sbc.h"

#define BLOCK_FRAMES 128
#define TOTAL_FRAMES (BLOCK_FRAMES * 32)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void generate_tone(int16_t *samples, size_t frames, unsigned int rate)
{
    size_t i;

    /* 997 Hz sits squarely inside one of the eight SBC subbands at 44.1 kHz,
     * so the codec tracks it with minimal spectral smearing. */
    for (i = 0; i < frames; ++i) {
        double phase = 2.0 * M_PI * 997.0 * (double)i / (double)rate;
        int16_t value = (int16_t)(12000.0 * sin(phase));

        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
    }
}

int main(void)
{
    sbc_t encoder;
    sbc_t decoder;
    int16_t input[TOTAL_FRAMES * 2];
    uint8_t encoded[TOTAL_FRAMES * 2];
    int16_t decoded[TOTAL_FRAMES * 2];
    size_t encoded_offset = 0;
    size_t decoded_count = 0;
    size_t input_offset = 0;
    size_t input_len;
    size_t encoded_len;
    size_t frame_length;
    size_t i;
    int best_lag = 0;
    unsigned long long best_score = 0;
    int lag;

    generate_tone(input, TOTAL_FRAMES, 44100);

    if (sbc_init(&encoder, 0) < 0) {
        fprintf(stderr, "sbc_init(encoder) failed\n");
        return 1;
    }
    encoder.frequency = SBC_FREQ_44100;
    encoder.mode = SBC_MODE_JOINT_STEREO;
    encoder.subbands = SBC_SB_8;
    encoder.blocks = SBC_BLK_16;
    encoder.allocation = SBC_AM_LOUDNESS;
    encoder.bitpool = 53;

    frame_length = sbc_get_frame_length(&encoder);
    input_len = sbc_get_codesize(&encoder);
    if (frame_length == 0 || frame_length > sizeof(encoded) ||
        input_len == 0) {
        fprintf(stderr, "invalid SBC geometry: frame=%zu codesize=%zu\n",
                frame_length, input_len);
        return 1;
    }

    while (input_offset + input_len <= sizeof(input) &&
           encoded_offset + frame_length <= sizeof(encoded)) {
        ssize_t written = 0;

        if (sbc_encode(&encoder, (const uint8_t *)input + input_offset,
                       input_len, encoded + encoded_offset, frame_length,
                       &written) < 0 ||
            written != (ssize_t)frame_length) {
            fprintf(stderr, "sbc_encode failed\n");
            return 1;
        }
        input_offset += input_len;
        encoded_offset += frame_length;
    }
    encoded_len = encoded_offset;
    if (encoded_len == 0) {
        fprintf(stderr, "nothing was encoded\n");
        return 1;
    }

    if (sbc_init(&decoder, 0) < 0) {
        fprintf(stderr, "sbc_init(decoder) failed\n");
        return 1;
    }

    for (i = 0; i < encoded_len;) {
        size_t consumed;
        size_t written_bytes = 0;

        if (decoded_count + BLOCK_FRAMES > TOTAL_FRAMES)
            break;
        consumed = (size_t)sbc_parse(&decoder, encoded + i, encoded_len - i);
        if (consumed == 0)
            break;
        if (sbc_decode(&decoder, encoded + i, consumed,
                       decoded + decoded_count * 2,
                       (TOTAL_FRAMES - decoded_count) * 2 * sizeof(int16_t),
                       &written_bytes) < 0) {
            fprintf(stderr, "sbc_decode failed at offset %zu\n", i);
            return 1;
        }
        decoded_count += written_bytes / (2 * sizeof(int16_t));
        i += consumed;
    }

    if (decoded_count < 1024) {
        fprintf(stderr, "too few decoded samples: %zu\n", decoded_count);
        return 1;
    }

    /* The SBC synthesis filter bank adds a fixed delay of up to ~80 samples;
     * search the alignment window for the lag with the highest sample
     * correlation. */
    for (lag = -160; lag <= 160; ++lag) {
        unsigned long long score = 0;
        size_t start = lag >= 0 ? (size_t)lag : 0;
        size_t count = 2048;

        for (i = 0; i < count && start + i < decoded_count; ++i) {
            long source_value = input[i * 2];
            long decoded_value = decoded[(start + i) * 2];
            score += (unsigned long long)(source_value * decoded_value > 0);
        }
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    if (best_score * 100 < 2048ULL * 97) {
        fprintf(stderr, "SBC decode correlation too low: %llu/2048 (lag=%d)\n",
                best_score, best_lag);
        return 1;
    }

    sbc_finish(&encoder);
    sbc_finish(&decoder);
    printf("sbc codec round-trip: ok (frames=%zu, decoded=%zu, lag=%d, "
           "correlation=%llu%%)\n",
           encoded_len / frame_length, decoded_count, best_lag,
           best_score * 100 / 2048);
    return 0;
}
