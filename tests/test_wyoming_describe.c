/* Wyoming TTS voice catalogue (#63): parse a real wyoming-piper style info document. */
#include "adapter/wyoming_describe.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    struct le_wyoming_voice v[LE_WYOMING_VOICES_MAX];
    size_t n = 0;
    int truncated = 0, i;
    static char big[60000];
    const char *info =
        "{\"asr\":[],\"tts\":[{\"name\":\"piper\",\"attribution\":{\"name\":\"rhasspy\",\"url\":\"https://github.com/rhasspy/piper\"},"
        "\"installed\":true,\"description\":\"A fast, local, neural text to speech engine\",\"version\":\"1.2.0\",\"voices\":["
        "{\"name\":\"en_US-hfc_female-medium\",\"attribution\":{\"name\":\"hfc\",\"url\":\"x\"},\"installed\":true,\"description\":\"hfc_female\",\"languages\":[\"en_US\"],\"speakers\":null,\"version\":null},"
        "{\"name\":\"en_GB-alan-medium\",\"installed\":false,\"languages\":[\"en_GB\"]},"
        "{\"name\":\"bad name;rm\",\"installed\":true},"
        "{\"name\":\"en_GB-cori-high\",\"languages\":[\"en_GB\",\"en\"],\"description\":\"Cori \\\"high\\\"\"}]}],\"handle\":[]}";

    CHECK(le_wyoming_parse_tts_voices(info, v, LE_WYOMING_VOICES_MAX, &n, &truncated) == 0, "parse info");
    CHECK(n == 3, "unsafe voice names are skipped");
    CHECK(!strcmp(v[0].name, "en_US-hfc_female-medium") && !strcmp(v[0].language, "en_US") && v[0].installed == 1, "first voice fields");
    CHECK(!strcmp(v[1].name, "en_GB-alan-medium") && v[1].installed == 0, "installed false kept");
    CHECK(!strcmp(v[2].name, "en_GB-cori-high") && !strcmp(v[2].language, "en_GB") && v[2].installed == 1, "missing installed defaults to true");
    CHECK(!truncated, "not truncated");

    CHECK(le_wyoming_parse_tts_voices("{\"asr\":[{\"name\":\"whisper\"}]}", v, LE_WYOMING_VOICES_MAX, &n, &truncated) == 0 && n == 0, "no tts programs is empty, not error");

    strcpy(big, "{\"tts\":[{\"name\":\"piper\",\"voices\":[");
    for (i = 0; i < 70; i++) {
        char one[96];
        snprintf(one, sizeof(one), "%s{\"name\":\"voice-%02d\",\"languages\":[\"en_US\"]}", i ? "," : "", i);
        strcat(big, one);
    }
    strcat(big, "]}]}");
    CHECK(le_wyoming_parse_tts_voices(big, v, LE_WYOMING_VOICES_MAX, &n, &truncated) == 0, "large catalogue parses");
    CHECK(n == LE_WYOMING_VOICES_MAX && truncated, "capped at 64 and marked truncated");
    CHECK(le_wyoming_parse_tts_voices("{\"tts\":[{\"voices\":[{\"name\":\"x\"", v, LE_WYOMING_VOICES_MAX, &n, &truncated) != 0 || n == 0, "truncated document does not crash");

    if (failures) { fprintf(stderr, "wyoming describe: %d failure(s)\n", failures); return 1; }
    puts("wyoming describe: ok");
    return 0;
}
