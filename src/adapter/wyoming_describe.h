#ifndef LIBREECHO_WYOMING_DESCRIBE_H
#define LIBREECHO_WYOMING_DESCRIBE_H
/*
 * Voice catalogue of a Wyoming TTS server (#63): send "describe", read the
 * "info" reply (bounded, with timeouts) and list the voices of its TTS
 * programs. Names are kept only if they are short safe tokens.
 */
#include <stddef.h>

#define LE_WYOMING_INFO_MAX 65536
#define LE_WYOMING_VOICES_MAX 64

struct le_wyoming_voice {
    char name[64], language[16], description[96];
    int installed;
};

/* 0 on success (count may be 0); -1 when the server cannot be reached or does
   not answer with a valid info event in time. *truncated is set when the server
   offered more than max voices. */
int le_wyoming_tts_voices(const char *uri, int timeout_ms, struct le_wyoming_voice *out,
                          size_t max, size_t *count, int *truncated);

/* Parse an info event's data document (exposed for tests). */
int le_wyoming_parse_tts_voices(const char *data, struct le_wyoming_voice *out, size_t max,
                                size_t *count, int *truncated);
#endif
