/*
 * LibreEcho TTS engine interface (C linkage).
 *
 * Abstracts the speech synthesizer behind a small C ABI so the daemon
 * (ttsd.c, pure C99) never depends on C++.  Two implementations exist:
 *
 *   - tts_engine_mock.c   : generates a short sine "chirp".  Used to
 *                           validate the full announce pipeline (daemon,
 *                           adapter protocol, announcement bus, ducking,
 *                           LED) without shipping a neural model.
 *   - tts_engine_sherpa.cpp : real ZipVoice synthesis via sherpa-onnx.
 *
 * The engine produces S16_LE mono PCM at its native sample rate.  The
 * daemon is responsible for resampling to the announcement bus rate
 * (48 kHz) and duplicating the mono channel to stereo.
 *
 * Thread-safety: a handle is NOT reentrant.  The daemon serializes
 * synthesis by running at most one utterance at a time (fork model).
 */
#ifndef LE_TTS_ENGINE_H
#define LE_TTS_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Announcement bus PCM format (must match audio_engine expectations). */
#define LE_TTS_BUS_RATE 48000
#define LE_TTS_BUS_CHANNELS 2

/* Opaque synthesizer handle. */
struct tts_engine;
typedef int (*tts_engine_stream_callback)(const short *samples, size_t frames,
                                           int sample_rate, void *context);

/*
 * Load and warm the model from model_dir.
 *
 *   model_dir : directory containing the model files.  For the mock
 *               engine this may be NULL or empty (ignored).
 *   voice     : voice name hint (e.g. "zipvoice"); may be NULL.
 *
 * Returns a handle, or NULL on failure.  Call once at daemon startup.
 */
struct tts_engine *tts_engine_init(const char *model_dir, const char *voice);

/*
 * Native sample rate of the engine's raw output (before resampling).
 * The mock engine reports LE_TTS_BUS_RATE so no resampling is needed.
 */
int tts_engine_sample_rate(const struct tts_engine *engine);

/*
 * Synthesize text into S16_LE mono PCM at the engine's native rate.
 *
 *   text   : UTF-8 text to speak.
 *   out    : receives a malloc()ed sample buffer; caller frees with
 *            tts_engine_free_samples().
 *   n_out  : receives the number of samples (frames) in *out.
 *
 * Returns 0 on success, negative on failure.  On failure *out is NULL.
 */
int tts_engine_synthesize(struct tts_engine *engine, const char *text,
                          short **out, size_t *n_out);

/* Stream native-rate mono PCM as Wyoming produces it. */
int tts_engine_synthesize_stream(struct tts_engine *engine, const char *text,
                                 tts_engine_stream_callback callback,
                                 void *context);

/* Release a sample buffer returned by tts_engine_synthesize(). */
void tts_engine_free_samples(short *samples);

/* Destroy the engine and release all model resources. */
void tts_engine_destroy(struct tts_engine *engine);

#ifdef __cplusplus
}
#endif

#endif /* LE_TTS_ENGINE_H */
