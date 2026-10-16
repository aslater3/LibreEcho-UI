/*
 * Regression: starting the additive sleep-noise source "heartbeat" through the
 * mock backend must not overflow the legacy 8-byte noise_colour alias.
 *
 * noise_source is the authoritative generator selection (white/pink/brown/
 * heartbeat); noise_colour is only its legacy white/pink/brown alias. A
 * heartbeat selection therefore cannot be copied wholesale into noise_colour:
 * "heartbeat" plus its terminator is ten bytes and the alias is eight. When the
 * mock copied it with strcpy() the daemon aborted on the fortify interceptor
 * (__strcpy_chk: destlen=8, src="heartbeat"), which the real browser suite saw
 * as the web daemon dying right after an LED/nursery request.
 *
 * The test drives the real mock backend through the public ops table, so it
 * exercises the same code path the HTTP handler does.
 */
#include "backend_internal.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            fprintf(stderr, "FAIL: %s\n", what);                              \
        }                                                                     \
    } while (0)

static int legacy_colour_valid(const char *c)
{
    return !strcmp(c, "white") || !strcmp(c, "pink") || !strcmp(c, "brown");
}

int main(void)
{
    struct le_backend backend;
    struct le_noise_request req;
    struct le_audio_state audio;

    memset(&backend, 0, sizeof(backend));
    CHECK(le_mock_create(&backend, NULL, NULL, 1) == LE_OK,
          "mock backend is created");

    memset(&req, 0, sizeof(req));
    req.level = 30;
    req.minutes = 60;
    req.tempo = 72;
    req.fade_seconds = 120;
    strcpy(req.source, "heartbeat");
    strcpy(req.bed, "brown");

    /* The crash happened here, in the mock's copy into noise_colour. */
    CHECK(backend.ops->noise_start_ex(&backend, &req) == LE_OK,
          "heartbeat source starts on the mock backend");

    memset(&audio, 0, sizeof(audio));
    CHECK(backend.ops->audio(&backend, &audio) == LE_OK,
          "audio state is readable after heartbeat start");
    CHECK(!strcmp(audio.noise_source, "heartbeat"),
          "noise_source keeps the authoritative heartbeat selection");
    CHECK(legacy_colour_valid(audio.noise_colour),
          "legacy noise_colour alias stays a white/pink/brown colour");
    CHECK(!strcmp(audio.noise_bed, "brown"), "noise_bed is retained");
    CHECK(audio.noise_tempo == 72, "noise_tempo is retained");
    CHECK(audio.noise_fade_seconds == 120, "noise_fade_seconds is retained");
    CHECK(audio.noise_level == 30, "noise_level is retained");
    CHECK(audio.noise_active == 1, "noise generator is active");

    /* A colour source must still map onto its own legacy alias. Negative
       tempo/fade mean "not supplied" and must be rejected only when present. */
    memset(&req, 0, sizeof(req));
    req.level = 40;
    req.minutes = 0;
    req.tempo = -1;
    req.fade_seconds = -1;
    strcpy(req.source, "pink");
    CHECK(backend.ops->noise_start_ex(&backend, &req) == LE_OK,
          "pink source starts on the mock backend");
    memset(&audio, 0, sizeof(audio));
    CHECK(backend.ops->audio(&backend, &audio) == LE_OK,
          "audio state is readable after pink start");
    CHECK(!strcmp(audio.noise_source, "pink"), "noise_source is pink");
    CHECK(!strcmp(audio.noise_colour, "pink"),
          "pink source maps onto the pink legacy alias");

    /* The legacy colour-only call keeps its own contract. */
    CHECK(backend.ops->noise_start(&backend, "brown", 25, 5) == LE_OK,
          "legacy colour-only start still works");
    memset(&audio, 0, sizeof(audio));
    CHECK(backend.ops->audio(&backend, &audio) == LE_OK,
          "audio state is readable after legacy start");
    CHECK(!strcmp(audio.noise_colour, "brown"),
          "legacy colour-only start keeps its colour");

    backend.ops->destroy(&backend);

    if (failures) {
        fprintf(stderr, "mock noise heartbeat regression: %d failure(s)\n",
                failures);
        return 1;
    }
    puts("mock noise heartbeat regression: ok");
    return 0;
}
