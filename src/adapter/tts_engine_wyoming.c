#define _POSIX_C_SOURCE 200809L

#include "tts_engine.h"
#include "wyoming_client.h"
#include "wyoming_protocol.h"
#include "../json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INITIAL_PCM_SAMPLES 32768U
#define MAX_PCM_SAMPLES (48000U * 120U)

struct tts_engine {
    char endpoint[LE_WYOMING_URI_MAX];
    char voice[128];
    int sample_rate;
};

static int append_pcm(short **samples, size_t *used, size_t *capacity,
                      const void *payload, size_t payload_length)
{
    size_t incoming = payload_length / sizeof(short);
    size_t required = *used + incoming;
    short *grown;

    if (payload_length % sizeof(short) || required > MAX_PCM_SAMPLES)
        return -1;
    if (required > *capacity) {
        size_t next = *capacity ? *capacity : INITIAL_PCM_SAMPLES;

        while (next < required && next < MAX_PCM_SAMPLES)
            next *= 2U;
        if (next > MAX_PCM_SAMPLES)
            next = MAX_PCM_SAMPLES;
        if (next < required)
            return -1;
        grown = realloc(*samples, next * sizeof(**samples));
        if (!grown)
            return -1;
        *samples = grown;
        *capacity = next;
    }
    memcpy(*samples + *used, payload, payload_length);
    *used = required;
    return 0;
}

struct tts_engine *tts_engine_init(const char *model_dir, const char *voice)
{
    struct tts_engine *engine;
    const char *endpoint = getenv("LE_TTS_WYOMING_URI");
    const char *selected_voice = getenv("LE_TTS_WYOMING_VOICE");

    if (!endpoint || !endpoint[0])
        endpoint = model_dir;
    if (!selected_voice || !selected_voice[0])
        selected_voice = voice;
    if (!endpoint || !le_wyoming_uri_valid(endpoint) ||
        !selected_voice || !selected_voice[0] ||
        strlen(selected_voice) >= sizeof(engine->voice))
        return NULL;
    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    snprintf(engine->endpoint, sizeof(engine->endpoint), "%s", endpoint);
    snprintf(engine->voice, sizeof(engine->voice), "%s", selected_voice);
    engine->sample_rate = 22050;
    return engine;
}

int tts_engine_sample_rate(const struct tts_engine *engine)
{
    return engine ? engine->sample_rate : 22050;
}

int tts_engine_synthesize(struct tts_engine *engine, const char *text,
                          short **out, size_t *n_out)
{
    struct le_wyoming_event event;
    char escaped_text[4096];
    char escaped_voice[256];
    char data[4608];
    short *samples = NULL;
    size_t used = 0;
    size_t capacity = 0;
    int fd;
    int result = -1;

    if (!engine || !text || !text[0] || !out || !n_out)
        return -1;
    *out = NULL;
    *n_out = 0;
    json_escape(escaped_text, sizeof(escaped_text), text);
    json_escape(escaped_voice, sizeof(escaped_voice), engine->voice);
    if (snprintf(
            data, sizeof(data),
            "{\"text\":\"%s\",\"voice\":{\"name\":\"%s\"}}",
            escaped_text, escaped_voice) >= (int)sizeof(data))
        return -1;
    fd = le_wyoming_connect(engine->endpoint, 3000);
    if (fd < 0)
        return -1;
    if (le_wyoming_send(fd, "synthesize", data, NULL, 0) < 0)
        goto done;
    for (;;) {
        const char *json;

        if (le_wyoming_read_header(fd, &event) < 0)
            goto done;
        json = event.data_length ? event.data : event.header;
        if (!strcmp(event.type, "audio-start")) {
            int rate = 0;
            int width = 0;
            int channels = 0;

            if (json_get_int(json, "rate", &rate) != 1 ||
                json_get_int(json, "width", &width) != 1 ||
                json_get_int(json, "channels", &channels) != 1 ||
                rate < 8000 || rate > 96000 || width != 2 || channels != 1)
                goto done;
            engine->sample_rate = rate;
        }
        if (event.payload_length) {
            unsigned char *payload = malloc(event.payload_length);

            if (!payload ||
                le_wyoming_read_payload(
                    fd, payload, event.payload_length, &event) < 0) {
                free(payload);
                goto done;
            }
            if (!strcmp(event.type, "audio-chunk") &&
                append_pcm(&samples, &used, &capacity,
                           payload, event.payload_length) < 0) {
                free(payload);
                goto done;
            }
            free(payload);
        }
        if (!strcmp(event.type, "audio-stop")) {
            result = used ? 0 : -1;
            break;
        }
        if (!strcmp(event.type, "error"))
            break;
    }

done:
    close(fd);
    if (result < 0) {
        free(samples);
        return -1;
    }
    *out = samples;
    *n_out = used;
    return 0;
}

int tts_engine_synthesize_stream(struct tts_engine *engine, const char *text,
                                 tts_engine_stream_callback callback,
                                 void *context)
{
    struct le_wyoming_event event;
    char escaped_text[4096], escaped_voice[256], data[4608];
    int fd, result = -1;
    if (!engine || !text || !text[0] || !callback)
        return -1;
    json_escape(escaped_text, sizeof(escaped_text), text);
    json_escape(escaped_voice, sizeof(escaped_voice), engine->voice);
    if (snprintf(data, sizeof(data),
                 "{\"text\":\"%s\",\"voice\":{\"name\":\"%s\"}}",
                 escaped_text, escaped_voice) >= (int)sizeof(data))
        return -1;
    fd = le_wyoming_connect(engine->endpoint, 3000);
    if (fd < 0 || le_wyoming_send(fd, "synthesize", data, NULL, 0) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    for (;;) {
        const char *json;
        if (le_wyoming_read_header(fd, &event) < 0)
            break;
        json = event.data_length ? event.data : event.header;
        if (!strcmp(event.type, "audio-start")) {
            int rate = 0, width = 0, channels = 0;
            if (json_get_int(json, "rate", &rate) != 1 ||
                json_get_int(json, "width", &width) != 1 ||
                json_get_int(json, "channels", &channels) != 1 ||
                rate < 8000 || rate > 96000 || width != 2 || channels != 1)
                break;
            engine->sample_rate = rate;
        }
        if (event.payload_length) {
            unsigned char *payload = malloc(event.payload_length);
            if (!payload || le_wyoming_read_payload(fd, payload,
                                                     event.payload_length,
                                                     &event) < 0) {
                free(payload);
                break;
            }
            if (!strcmp(event.type, "audio-chunk") &&
                (event.payload_length % sizeof(short) ||
                 callback((const short *)payload,
                          event.payload_length / sizeof(short),
                          engine->sample_rate, context) < 0)) {
                free(payload);
                break;
            }
            free(payload);
        }
        if (!strcmp(event.type, "audio-stop")) {
            result = 0;
            break;
        }
        if (!strcmp(event.type, "error"))
            break;
    }
    close(fd);
    return result;
}

void tts_engine_free_samples(short *samples)
{
    free(samples);
}

void tts_engine_destroy(struct tts_engine *engine)
{
    free(engine);
}
