#ifndef LIBREECHO_WAKE_ENGINE_H
#define LIBREECHO_WAKE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct le_wake_engine;

struct le_wake_engine *le_wake_engine_create(const char *model_directory,
                                             unsigned int threads);

/*
 * As le_wake_engine_create, and also loads a second classifier that runs on
 * the same mel and embedding features as the first. Only the small classifier
 * is duplicated, so a second wake word adds one classifier inference per 80 ms
 * block. A NULL second_classifier gives the single-word engine. A second
 * classifier that fails to load is reported on stderr and the engine comes up
 * with the first word only.
 */
struct le_wake_engine *le_wake_engine_create_words(
    const char *model_directory,
    unsigned int threads,
    const char *second_classifier);

/* Number of classifiers the engine runs: 1, or 2 with a second word. */
size_t le_wake_engine_word_count(const struct le_wake_engine *engine);

/*
 * Feed continuous mono S16_LE/16 kHz audio.  A new classifier score per word
 * is produced every 1280 samples (80 ms).  scores[0] is the first word and
 * scores[1] the second; slots beyond the engine's word count are set to zero.
 * `new_score` is zero when this call only accumulated history and one when
 * the scores were updated.
 */
int le_wake_engine_feed_words(struct le_wake_engine *engine,
                              const int16_t *samples,
                              size_t count,
                              float *scores,
                              size_t score_slots,
                              int *new_score);

/* Single-word form of le_wake_engine_feed_words. */
int le_wake_engine_feed(struct le_wake_engine *engine,
                        const int16_t *samples,
                        size_t count,
                        float *score,
                        int *new_score);

unsigned int le_wake_engine_last_inference_us(
    const struct le_wake_engine *engine);

void le_wake_engine_destroy(struct le_wake_engine *engine);

#ifdef __cplusplus
}
#endif

#endif
