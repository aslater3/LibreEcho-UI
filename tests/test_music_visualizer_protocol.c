/*
 * Focused behavioural tests for the frozen v2 visualizer feature transport.
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 \
 *       -o build/test-music-viz-protocol \
 *       tests/test_music_visualizer_protocol.c \
 *       src/adapter/music_visualizer_protocol.c
 */
#include "../src/adapter/music_visualizer_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "music protocol: %s\n", message);
        exit(1);
    }
}

static const char VALID[] =
    "{\"feature_version\":2,\"seq\":7,\"timestamp_ms\":12345,\"session\":42,"
    "\"energy\":10,\"warmth\":20,\"brightness_axis\":30,\"density\":40,"
    "\"transientness\":50,\"groove\":60,\"build\":70,\"spaciousness\":80,"
    "\"loudness_fast\":90,\"loudness_slow\":100,\"onset_low\":110,"
    "\"onset_mid\":120,\"onset_high\":130,\"beat_strength\":140,"
    "\"beat_confidence\":150,\"novelty\":160,\"event_strength\":170,"
    "\"beat_phase\":32000,\"bpm_x100\":12800,\"events\":273}";

int main(void)
{
    struct le_music_features f;
    struct le_music_stream stream;

    /* 1. A valid v2 frame parses exactly. */
    memset(&f, 0, sizeof(f));
    require_condition(le_music_parse_v2(VALID, strlen(VALID), &f) == 1,
                      "valid frame must parse");
    require_condition(f.feature_version == 2U, "version must be 2");
    require_condition(f.seq == 7U && f.session == 42U,
                      "seq/session must round-trip");
    require_condition(f.events == 273U, "events bitmask must parse");
    require_condition(f.beat_phase == 32000U && f.bpm_x100 == 12800U,
                      "phase/bpm must round-trip");
    require_condition(f.novelty == 160U && f.event_strength == 170U,
                      "structural fields must round-trip");

    /* 2. Legacy v1 (no feature_version) is not a v2 frame, not an error. */
    require_condition(le_music_parse_v2("{\"seq\":1}", 9, &f) == 0,
                      "missing feature_version is a legacy frame");

    /* 3. A different feature_version is rejected. */
    {
        const char bad[] =
            "{\"feature_version\":3,\"seq\":1,\"timestamp_ms\":1,"
            "\"session\":1}";
        require_condition(le_music_parse_v2(bad, strlen(bad), &f) == -1,
                          "unknown feature_version must be rejected");
    }

    /* 4. A malformed (missing required field) frame is rejected. */
    {
        const char bad[] =
            "{\"feature_version\":2,\"seq\":1,\"timestamp_ms\":1,"
            "\"session\":1,\"energy\":5}";
        require_condition(le_music_parse_v2(bad, strlen(bad), &f) == -1,
                          "missing fields must be rejected");
    }

    /* 5. Out-of-range fields are rejected, not clamped. */
    {
        char bad[sizeof(VALID)];
        memcpy(bad, VALID, sizeof(VALID));
        {
            char *p = strstr(bad, "32000");
            memcpy(p, "70000", 5);
        }
        require_condition(le_music_parse_v2(bad, strlen(bad), &f) == -1,
                          "beat_phase out of range must be rejected");
    }
    /* 6. Session must be nonzero. */
    {
        char bad[sizeof(VALID)];
        memcpy(bad, VALID, sizeof(VALID));
        {
            char *p = strstr(bad, "\"session\":42");
            memcpy(p, "\"session\":0\"", 12);
        }
        require_condition(le_music_parse_v2(bad, strlen(bad), &f) == -1,
                          "zero session must be rejected");
    }

    /* 7. Events may also arrive as names or an array. */
    {
        char named[sizeof(VALID) + 64];
        char *p;
        memcpy(named, VALID, sizeof(VALID));
        p = strstr(named, "\"events\":273");
        require_condition(p != NULL, "events field present");
        strcpy(p, "\"events\":\"kick,snare,high,drop\"}");
        require_condition(le_music_parse_v2(named, strlen(named), &f) == 1,
                          "named events must parse");
        require_condition(f.events == (1U | 2U | 4U | 256U),
                          "named events must map to bits");
    }

    /* 8. Stream ordering: strictly increasing seq, non-decreasing time. */
    le_music_stream_reset(&stream);
    memset(&f, 0, sizeof(f));
    f.session = 5U; f.seq = 10U; f.timestamp_ms = 1000U;
    require_condition(le_music_stream_accept(&stream, &f) == 1,
                      "first frame of a new session accepted");
    f.seq = 10U; f.timestamp_ms = 1000U;
    require_condition(le_music_stream_accept(&stream, &f) == 0,
                      "duplicate seq rejected");
    f.seq = 9U; f.timestamp_ms = 900U;
    require_condition(le_music_stream_accept(&stream, &f) == 0,
                      "reordered seq rejected");
    f.seq = 11U; f.timestamp_ms = 500U;
    require_condition(le_music_stream_accept(&stream, &f) == 0,
                      "backwards timestamp rejected");
    f.seq = 11U; f.timestamp_ms = 1100U;
    require_condition(le_music_stream_accept(&stream, &f) == 1,
                      "monotonic frame accepted");
    require_condition(stream.last_seq == 11U, "tracker must advance");

    /* 9. A new nonzero session restarts without a special reset. */
    f.session = 6U; f.seq = 1U; f.timestamp_ms = 50U;
    require_condition(le_music_stream_accept(&stream, &f) == 1,
                      "new session must be accepted");
    require_condition(stream.last_seq == 1U, "new session resets sequence");
    /* Rejection must not mutate the tracker. */
    f.session = 6U; f.seq = 0U; f.timestamp_ms = 40U;
    require_condition(le_music_stream_accept(&stream, &f) == 0,
                      "stale frame in current session rejected");
    require_condition(stream.last_seq == 1U,
                      "rejected frame must not mutate the tracker");

    puts("music visualizer v2 protocol: ok");
    return 0;
}
