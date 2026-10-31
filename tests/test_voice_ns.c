/* Noise suppressor for the assistant audio stream (#401). */
#include "adapter/voice_ns.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RATE LE_VOICE_NS_RATE
#define SECONDS 8
#define SAMPLES (RATE * SECONDS)
/* Bursts of loud broadband "speech" from 4 s, 300 ms on, 600 ms period. */
#define BURST_START (RATE * 4)
#define BURST_ON (RATE * 3 / 10)
#define BURST_PERIOD (RATE * 6 / 10)

static int16_t input[SAMPLES];
static int16_t output[SAMPLES];
static int16_t scratch[SAMPLES];
static uint32_t random_state = 0x4c454e53U;
static int failures;

#define CHECK(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static double gaussian(void)
{
    double sum = 0.0;
    int i;

    /* Irwin-Hall approximation, deterministic across hosts. */
    for (i = 0; i < 12; ++i) {
        random_state = random_state * 1664525U + 1013904223U;
        sum += (double)(random_state >> 8) / 16777216.0;
    }
    return sum - 6.0;
}

static int16_t clamp(double value)
{
    if (value > 32767.0)
        return 32767;
    if (value < -32768.0)
        return -32768;
    return (int16_t)value;
}

static int in_burst(size_t i)
{
    return i >= BURST_START && (i - BURST_START) % BURST_PERIOD < BURST_ON;
}

static double rms(const int16_t *samples, size_t first, size_t count)
{
    double sum = 0.0;
    size_t i;

    for (i = 0; i < count; ++i)
        sum += (double)samples[first + i] * samples[first + i];
    return sqrt(sum / (double)count);
}

static void run(int16_t *samples, size_t chunk, int attenuation_db)
{
    struct le_voice_ns ns;
    size_t done = 0;

    if (le_voice_ns_init(&ns, attenuation_db) < 0) {
        CHECK(0, "init failed");
        return;
    }
    while (done < SAMPLES) {
        size_t count = SAMPLES - done < chunk ? SAMPLES - done : chunk;

        le_voice_ns_process(&ns, samples + done, count);
        done += count;
    }
}

int main(void)
{
    struct le_voice_ns ns;
    double noise_in, noise_out, burst_in = 0.0, burst_out = 0.0;
    double best = -1.0;
    size_t best_lag = 0, lag, i, bursts = 0;
    int16_t zeros[1000];
    clock_t started;

    CHECK(le_voice_ns_init(&ns, LE_VOICE_NS_MIN_ATTENUATION_DB - 1) < 0,
          "attenuation below range accepted");
    CHECK(le_voice_ns_init(&ns, LE_VOICE_NS_MAX_ATTENUATION_DB + 1) < 0,
          "attenuation above range accepted");
    CHECK(le_voice_ns_init(NULL, LE_VOICE_NS_DEFAULT_ATTENUATION_DB) < 0,
          "NULL state accepted");

    /* Steady room noise (RMS ~400) with loud speech-like bursts on top. */
    for (i = 0; i < SAMPLES; ++i) {
        double value = 400.0 * gaussian();

        if (in_burst(i))
            value += 6000.0 * gaussian();
        input[i] = clamp(value);
    }
    memcpy(output, input, sizeof(output));
    started = clock();
    run(output, 160, LE_VOICE_NS_DEFAULT_ATTENUATION_DB);
    printf("voice-ns: %d s of audio in %.1f ms host CPU\n", SECONDS,
           1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC);

    /* Noise only, after the estimate has settled (2..3.5 s). */
    noise_in = rms(input, RATE * 2, RATE * 3 / 2);
    noise_out = rms(output, RATE * 2 + LE_VOICE_NS_LATENCY_SAMPLES,
                    RATE * 3 / 2);
    printf("voice-ns: steady noise %.1f -> %.1f RMS (%.1f dB)\n", noise_in,
           noise_out, 20.0 * log10(noise_out / noise_in));
    CHECK(noise_out < noise_in * 0.3162, /* at least 10 dB quieter */
          "steady noise only reduced from %.1f to %.1f", noise_in,
          noise_out);
    CHECK(noise_out > noise_in * 0.1, /* never deeper than the floor */
          "noise cut below the 15 dB floor: %.1f -> %.1f", noise_in,
          noise_out);

    /* Speech bursts keep their level (measured away from the edges). */
    for (i = BURST_START; i + BURST_PERIOD <= SAMPLES; i += BURST_PERIOD) {
        size_t first = i + 800, count = BURST_ON - 1600;

        burst_in += rms(input, first, count);
        burst_out += rms(output, first + LE_VOICE_NS_LATENCY_SAMPLES, count);
        ++bursts;
    }
    burst_in /= (double)bursts;
    burst_out /= (double)bursts;
    printf("voice-ns: speech %.1f -> %.1f RMS (%.2f dB)\n", burst_in,
           burst_out, 20.0 * log10(burst_out / burst_in));
    CHECK(burst_out > burst_in * 0.8913, /* within 1 dB */
          "speech lost level: %.1f -> %.1f", burst_in, burst_out);
    CHECK(burst_out < burst_in * 1.122, "speech gained level");

    /* The delay is exactly the documented constant. */
    for (lag = 0; lag <= 512; ++lag) {
        double sum = 0.0;

        for (i = RATE * 5; i < RATE * 6; ++i)
            sum += (double)input[i] * output[i + lag];
        if (sum > best) {
            best = sum;
            best_lag = lag;
        }
    }
    CHECK(best_lag == LE_VOICE_NS_LATENCY_SAMPLES,
          "delay is %zu samples, documented %u", best_lag,
          (unsigned)LE_VOICE_NS_LATENCY_SAMPLES);

    /* Frame size does not change the result: waked sends 160-sample
       frames, the stream allows up to 1280, and single samples work. */
    memcpy(scratch, input, sizeof(scratch));
    run(scratch, 1, LE_VOICE_NS_DEFAULT_ATTENUATION_DB);
    CHECK(!memcmp(scratch, output, sizeof(scratch)),
          "1-sample chunks differ from 160-sample chunks");
    memcpy(scratch, input, sizeof(scratch));
    run(scratch, 1280, LE_VOICE_NS_DEFAULT_ATTENUATION_DB);
    CHECK(!memcmp(scratch, output, sizeof(scratch)),
          "1280-sample chunks differ from 160-sample chunks");

    /* A deeper setting cuts deeper. */
    memcpy(scratch, input, sizeof(scratch));
    run(scratch, 160, LE_VOICE_NS_MAX_ATTENUATION_DB);
    CHECK(rms(scratch, RATE * 2 + LE_VOICE_NS_LATENCY_SAMPLES, RATE) <
              rms(output, RATE * 2 + LE_VOICE_NS_LATENCY_SAMPLES, RATE),
          "30 dB setting not deeper than 15 dB");

    /* Digital silence stays silent (no NaN, no DC from the filter). */
    CHECK(le_voice_ns_init(&ns, LE_VOICE_NS_DEFAULT_ATTENUATION_DB) == 0,
          "init");
    memset(zeros, 0, sizeof(zeros));
    for (i = 0; i < 20; ++i)
        le_voice_ns_process(&ns, zeros, sizeof(zeros) / sizeof(zeros[0]));
    for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i)
        CHECK(zeros[i] == 0, "silence became %d", zeros[i]);

    /* Full-scale input saturates instead of wrapping. */
    for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i)
        zeros[i] = (i / 8) % 2 ? 32767 : -32768;
    for (i = 0; i < 4; ++i)
        le_voice_ns_process(&ns, zeros, sizeof(zeros) / sizeof(zeros[0]));

    /* Reset empties the delay line. */
    le_voice_ns_reset(&ns);
    for (i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i)
        zeros[i] = 1000;
    le_voice_ns_process(&ns, zeros, LE_VOICE_NS_HOP);
    for (i = 0; i < LE_VOICE_NS_HOP; ++i)
        CHECK(zeros[i] == 0, "reset left %d in the delay line", zeros[i]);

    if (failures) {
        fprintf(stderr, "test-voice-ns: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-voice-ns: ok\n");
    return 0;
}
