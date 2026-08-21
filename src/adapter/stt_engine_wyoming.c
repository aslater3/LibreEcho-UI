#define _POSIX_C_SOURCE 200809L

#include "stt_engine.h"
#include "voice_dsp.h"
#include "wyoming_client.h"
#include "wyoming_protocol.h"
#include "../json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STT_RATE 16000U
#define MIN_STREAM_SAMPLES (STT_RATE / 2U)
#define DEFAULT_END_SILENCE_MS 600U
#define DEFAULT_START_TIMEOUT_MS 8000U
#define DEFAULT_MAX_UTTERANCE_MS 12000U
struct stt_engine {
    char endpoint[LE_WYOMING_URI_MAX];
    char model[128];
    char language[32];
    unsigned int vad_floor_rms;
    unsigned int end_silence_ms;
    unsigned int start_timeout_ms;
    unsigned int max_utterance_ms;
};

struct stt_stream {
    struct stt_engine *engine;
    int fd;
    size_t samples;
    size_t speech_start_samples;
    size_t quiet_samples;
    int16_t vad_frame[LE_VOICE_FRAME_SAMPLES];
    size_t vad_frame_used;
    struct le_voice_vad vad;
    int speech_seen;
};

static const char *environment_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return value && value[0] ? value : fallback;
}

static unsigned int environment_unsigned(const char *name,
                                         unsigned int fallback,
                                         unsigned int minimum,
                                         unsigned int maximum)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;

    if (!value || !value[0])
        return fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || *end || parsed < minimum || parsed > maximum)
        return fallback;
    return (unsigned int)parsed;
}

struct stt_engine *stt_engine_init(const char *model_dir,
                                   unsigned int threads)
{
    struct stt_engine *engine;
    const char *endpoint = environment_or("LE_STT_WYOMING_URI", model_dir);
    const char *model = environment_or("LE_STT_WYOMING_MODEL",
                                        "whisper-small");
    const char *language = environment_or("LE_STT_WYOMING_LANGUAGE", "en");

    (void)threads;
    if (!endpoint || !le_wyoming_uri_valid(endpoint) ||
        strlen(model) >= sizeof(engine->model) ||
        strlen(language) >= sizeof(engine->language))
        return NULL;
    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    snprintf(engine->endpoint, sizeof(engine->endpoint), "%s", endpoint);
    snprintf(engine->model, sizeof(engine->model), "%s", model);
    snprintf(engine->language, sizeof(engine->language), "%s", language);
    engine->vad_floor_rms = environment_unsigned(
        "LE_STT_VAD_FLOOR_RMS", LE_VOICE_VAD_DEFAULT_FLOOR_RMS, 1, 1024);
    engine->end_silence_ms = environment_unsigned(
        "LE_STT_END_SILENCE_MS", DEFAULT_END_SILENCE_MS, 200, 3000);
    engine->start_timeout_ms = environment_unsigned(
        "LE_STT_START_TIMEOUT_MS", DEFAULT_START_TIMEOUT_MS, 1000, 15000);
    engine->max_utterance_ms = environment_unsigned(
        "LE_STT_MAX_UTTERANCE_MS", DEFAULT_MAX_UTTERANCE_MS, 2000, 20000);
    return engine;
}

void stt_engine_destroy(struct stt_engine *engine)
{
    free(engine);
}

struct stt_stream *stt_engine_stream_create(struct stt_engine *engine)
{
    struct stt_stream *stream;
    char model[256];
    char language[64];
    char data[384];

    if (!engine)
        return NULL;
    stream = calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->fd = le_wyoming_connect(engine->endpoint, 3000);
    if (stream->fd < 0) {
        free(stream);
        return NULL;
    }
    stream->engine = engine;
    le_voice_vad_init(&stream->vad);
    (void)le_voice_vad_set_floor_rms(
        &stream->vad, engine->vad_floor_rms);
    /*
     * Recognition begins after a wake event, so its first frame can already
     * contain speech. Seed the adaptive detector from the configured floor
     * instead of treating that first frame as room noise.
     */
    stream->vad.noise_energy =
        (uint64_t)engine->vad_floor_rms * engine->vad_floor_rms;
    stream->vad.initialized = 1;
    json_escape(model, sizeof(model), engine->model);
    json_escape(language, sizeof(language), engine->language);
    snprintf(data, sizeof(data),
             "{\"name\":\"%s\",\"language\":\"%s\"}", model, language);
    if (le_wyoming_send(stream->fd, "transcribe", data, NULL, 0) < 0 ||
        le_wyoming_send(
            stream->fd, "audio-start",
            "{\"rate\":16000,\"width\":2,\"channels\":1}", NULL, 0) < 0) {
        close(stream->fd);
        free(stream);
        return NULL;
    }
    return stream;
}

void stt_engine_stream_destroy(struct stt_stream *stream)
{
    if (!stream)
        return;
    if (stream->fd >= 0)
        close(stream->fd);
    free(stream);
}

int stt_engine_stream_accept(struct stt_stream *stream,
                             const int16_t *samples, size_t count,
                             char *text, size_t text_size)
{
    size_t offset = 0;
    size_t end_silence_samples;
    size_t start_timeout_samples;
    size_t max_utterance_samples;

    if (!stream || stream->fd < 0 || !samples || !count)
        return -1;
    if (text && text_size)
        text[0] = '\0';
    if (le_wyoming_send(
            stream->fd, "audio-chunk",
            "{\"rate\":16000,\"width\":2,\"channels\":1}",
            samples, count * sizeof(*samples)) < 0)
        return -1;
    while (offset < count) {
        size_t available = LE_VOICE_FRAME_SAMPLES - stream->vad_frame_used;
        size_t copy = count - offset < available ? count - offset : available;

        memcpy(stream->vad_frame + stream->vad_frame_used,
               samples + offset, copy * sizeof(*samples));
        stream->vad_frame_used += copy;
        offset += copy;
        if (stream->vad_frame_used == LE_VOICE_FRAME_SAMPLES) {
            struct le_voice_vad_result result = le_voice_vad_process(
                &stream->vad, stream->vad_frame, LE_VOICE_FRAME_SAMPLES);

            if (result.active) {
                if (!stream->speech_seen)
                    stream->speech_start_samples =
                        stream->samples + offset - LE_VOICE_FRAME_SAMPLES;
                stream->speech_seen = 1;
                stream->quiet_samples = 0;
            } else if (stream->speech_seen) {
                stream->quiet_samples += LE_VOICE_FRAME_SAMPLES;
            }
            stream->vad_frame_used = 0;
        }
    }
    stream->samples += count;
    end_silence_samples =
        (size_t)STT_RATE * stream->engine->end_silence_ms / 1000U;
    start_timeout_samples =
        (size_t)STT_RATE * stream->engine->start_timeout_ms / 1000U;
    max_utterance_samples =
        (size_t)STT_RATE * stream->engine->max_utterance_ms / 1000U;
    if (stream->speech_seen &&
        stream->samples >= MIN_STREAM_SAMPLES &&
        stream->quiet_samples >= end_silence_samples)
        return 1;
    /* Once speech has been observed the silence test above was the only way
       out, so a room that never falls quiet -- background conversation, a
       television, steady fan noise -- keeps quiet_samples pinned at zero and
       the utterance never ends.  The start timeout does not apply here
       because it is guarded on !speech_seen.  Bound the utterance so a turn
       always terminates: it keeps the STT backend from being handed an
       unbounded recording, and it limits how much room audio leaves the
       device on any single wake. */
    if (stream->speech_seen &&
        stream->samples >= stream->speech_start_samples + max_utterance_samples)
        return 1;
    return !stream->speech_seen &&
           stream->samples >= start_timeout_samples;
}

int stt_engine_stream_finish(struct stt_stream *stream,
                             char *text, size_t text_size)
{
    struct le_wyoming_event event;

    if (!stream || stream->fd < 0 || !text || !text_size ||
        le_wyoming_send(
            stream->fd, "audio-stop",
            "{\"rate\":16000,\"width\":2,\"channels\":1}", NULL, 0) < 0)
        return -1;
    text[0] = '\0';
    for (;;) {
        const char *json;

        if (le_wyoming_read_header(stream->fd, &event) < 0)
            return -1;
        if (event.payload_length) {
            unsigned char *discard = malloc(event.payload_length);
            int result;

            if (!discard)
                return -1;
            result = le_wyoming_read_payload(
                stream->fd, discard, event.payload_length, &event);
            free(discard);
            if (result < 0)
                return -1;
        }
        json = event.data_length ? event.data : event.header;
        if (!strcmp(event.type, "transcript"))
            return json_get_string(json, "text", text, text_size) == 1
                ? 0 : -1;
        if (!strcmp(event.type, "error"))
            return -1;
    }
}

const char *stt_engine_name(const struct stt_engine *engine)
{
    (void)engine;
    return "wyoming";
}
