#include "stt_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_TRAILING_SILENCE 3200U

struct stt_engine {
    unsigned int threads;
};

struct stt_stream {
    struct stt_engine *engine;
    size_t speech_samples;
    size_t trailing_silence;
};

struct stt_engine *stt_engine_init(const char *model_dir,
                                   unsigned int threads)
{
    struct stt_engine *engine;

    (void)model_dir;
    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    engine->threads = threads ? threads : 1;
    return engine;
}

void stt_engine_destroy(struct stt_engine *engine)
{
    free(engine);
}

struct stt_stream *stt_engine_stream_create(struct stt_engine *engine)
{
    struct stt_stream *stream;

    if (!engine)
        return NULL;
    stream = calloc(1, sizeof(*stream));
    if (stream)
        stream->engine = engine;
    return stream;
}

void stt_engine_stream_destroy(struct stt_stream *stream)
{
    free(stream);
}

static void copy_result(const struct stt_stream *stream,
                        char *text, size_t text_size)
{
    const char *result =
        stream->speech_samples ? "mock transcription" : "";

    if (text && text_size)
        snprintf(text, text_size, "%s", result);
}

int stt_engine_stream_accept(struct stt_stream *stream,
                             const int16_t *samples, size_t count,
                             char *text, size_t text_size)
{
    size_t i;

    if (!stream || !samples)
        return -1;
    for (i = 0; i < count; ++i) {
        if (samples[i] > 64 || samples[i] < -64) {
            ++stream->speech_samples;
            stream->trailing_silence = 0;
        } else if (stream->speech_samples) {
            ++stream->trailing_silence;
        }
    }
    copy_result(stream, text, text_size);
    return stream->speech_samples &&
           stream->trailing_silence >= MOCK_TRAILING_SILENCE;
}

int stt_engine_stream_finish(struct stt_stream *stream,
                             char *text, size_t text_size)
{
    if (!stream)
        return -1;
    copy_result(stream, text, text_size);
    return 0;
}

const char *stt_engine_name(const struct stt_engine *engine)
{
    (void)engine;
    return "mock";
}
