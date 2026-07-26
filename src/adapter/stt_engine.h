#ifndef LIBREECHO_STT_ENGINE_H
#define LIBREECHO_STT_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stt_engine;
struct stt_stream;

struct stt_engine *stt_engine_init(const char *model_dir,
                                   unsigned int threads);
void stt_engine_destroy(struct stt_engine *engine);

struct stt_stream *stt_engine_stream_create(struct stt_engine *engine);
void stt_engine_stream_destroy(struct stt_stream *stream);

/*
 * Feed mono 16 kHz S16_LE PCM. The latest complete/partial transcript is
 * copied to text. Returns 1 at an endpoint, 0 while listening, or -1 on
 * failure.
 */
int stt_engine_stream_accept(struct stt_stream *stream,
                             const int16_t *samples, size_t count,
                             char *text, size_t text_size);

/* Flush the stream and copy the final transcript. */
int stt_engine_stream_finish(struct stt_stream *stream,
                             char *text, size_t text_size);

const char *stt_engine_name(const struct stt_engine *engine);

#ifdef __cplusplus
}
#endif

#endif
