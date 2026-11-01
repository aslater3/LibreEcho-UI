/*
 * Wake decoding: the score window is judged by its peak, not by its newest
 * frame.
 *
 * The support rule exists so a single noisy frame cannot wake the device.
 * Testing it against the newest score alone made it unsatisfiable for any
 * detection that rises quickly, because the two frames it looks back at are by
 * definition the approach to the peak and still low. This exercises the real
 * decoder against the sequence measured on hardware, and against the lone
 * spike the rule is there to reject.
 *
 * The ONNX engine is faked so score sequences can be scripted exactly. The
 * worker, its queue, its thread and decode_score() are the real ones.
 */

#include "adapter/wake_worker.h"
#include "adapter/wake_engine.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BLOCK_SAMPLES 1280u
#define ACCEPT_THRESHOLD 0.533f
#define MAX_FRAMES 8u

/* ---- faked engine: returns the scripted score, one per fed block ---- */

struct le_wake_engine {
    int unused;
};

static struct le_wake_engine fake_engine;
static const float *script_scores;
static size_t script_count;
static size_t script_position;

/* Second word: its script is used only when the engine was created with a
 * second classifier, as the real engine would run it. */
static const float *script2_scores;
static int fake_has_second;

struct le_wake_engine *le_wake_engine_create_words(
    const char *model_directory, unsigned int threads,
    const char *second_classifier)
{
    (void)model_directory;
    (void)threads;
    script_position = 0;
    fake_has_second = second_classifier != NULL;
    return &fake_engine;
}

int le_wake_engine_feed_words(struct le_wake_engine *engine,
                              const int16_t *samples,
                              size_t count,
                              float *scores,
                              size_t score_slots,
                              int *new_score)
{
    (void)engine;
    (void)samples;
    (void)count;
    scores[0] = script_position < script_count
        ? script_scores[script_position] : 0.0f;
    if (score_slots > 1)
        scores[1] = fake_has_second && script2_scores &&
            script_position < script_count
            ? script2_scores[script_position] : 0.0f;
    ++script_position;
    /*
     * Always a new score: the real engine produces one per 1280 samples, and
     * the worker treats "no new score" as an engine failure and stops.
     */
    *new_score = 1;
    return 0;
}

unsigned int le_wake_engine_last_inference_us(
    const struct le_wake_engine *engine)
{
    (void)engine;
    return 0;
}

void le_wake_engine_destroy(struct le_wake_engine *engine)
{
    (void)engine;
}

/* ---- harness ---- */

struct capture {
    unsigned int count;
    struct le_wake_event events[MAX_FRAMES];
};

static void on_wake_event(const struct le_wake_event *event, void *opaque)
{
    struct capture *capture = opaque;

    if (capture->count < MAX_FRAMES)
        capture->events[capture->count] = *event;
    ++capture->count;
}

/*
 * Submit one 1280-sample block per scripted score, then stop. Stopping drains
 * the queue and joins the worker thread, so the result is deterministic
 * without sleeping. Sequences stay within the 8-block queue so nothing is
 * dropped.
 */
static void run_sequence_mode(const float *scores, const int *vad_active,
                         const int *playback_active,
                         size_t count,
                         struct capture *capture,
                         struct le_wake_worker_metrics *metrics)
{
    struct le_wake_worker worker;
    int16_t block[BLOCK_SAMPLES];
    size_t i;

    assert(count <= MAX_FRAMES);
    memset(&worker, 0, sizeof(worker));
    memset(block, 0, sizeof(block));
    memset(capture, 0, sizeof(*capture));
    script_scores = scores;
    script_count = count;
    script2_scores = NULL;

    assert(le_wake_worker_start(&worker, "unused-model-dir", 1,
                                ACCEPT_THRESHOLD, on_wake_event,
                                capture) == 0);
    for (i = 0; i < count; ++i) {
        struct le_wake_observation observation;

        memset(&observation, 0, sizeof(observation));
        /* Frame n occupies samples [n*1280, (n+1)*1280). */
        observation.detection_sample = (uint64_t)(i + 1) * BLOCK_SAMPLES;
        observation.vad_score = 1.0f;
        observation.vad_active = vad_active[i];
        observation.playback_active = playback_active ? playback_active[i] : 0;
        assert(le_wake_worker_submit(&worker, block, BLOCK_SAMPLES,
                                     &observation) == 0);
    }
    le_wake_worker_stop(&worker, metrics);
}

static void run_sequence(const float *scores, const int *vad_active,
                         size_t count, struct capture *capture,
                         struct le_wake_worker_metrics *metrics)
{
    run_sequence_mode(scores, vad_active, NULL, count, capture, metrics);
}

static int close_to(float actual, float expected)
{
    return fabsf(actual - expected) < 1e-6f;
}

/*
 * Two wake words over one stream: scores1 is the first word's classifier and
 * scores2 the second's, both scored on the same blocks. The first model
 * directory is the stock one, so the first word is named alexa_v0.1.
 */
static void run_two_words(const float *scores1, const float *scores2,
                          const int *vad_active, size_t count,
                          struct capture *capture,
                          struct le_wake_worker_metrics *metrics)
{
    struct le_wake_worker worker;
    int16_t block[BLOCK_SAMPLES];
    size_t i;

    assert(count <= MAX_FRAMES);
    memset(&worker, 0, sizeof(worker));
    memset(block, 0, sizeof(block));
    memset(capture, 0, sizeof(*capture));
    script_scores = scores1;
    script2_scores = scores2;
    script_count = count;

    assert(le_wake_worker_start_words(&worker, "/usr/share/x/openwakeword", 1,
                                      ACCEPT_THRESHOLD,
                                      "/data/x/twin-word/alexa_v0.1.onnx",
                                      "twin-word", on_wake_event,
                                      capture) == 0);
    for (i = 0; i < count; ++i) {
        struct le_wake_observation observation;

        memset(&observation, 0, sizeof(observation));
        observation.detection_sample = (uint64_t)(i + 1) * BLOCK_SAMPLES;
        observation.vad_score = 1.0f;
        observation.vad_active = vad_active[i];
        assert(le_wake_worker_submit(&worker, block, BLOCK_SAMPLES,
                                     &observation) == 0);
    }
    le_wake_worker_stop(&worker, metrics);
}

int main(void)
{
    struct le_wake_worker_metrics metrics;
    struct capture capture;

    /* Playback belongs to the peak observation, not a later frame. */
    {
        static const float scores[] = {0.05f, 0.70f, 0.05f, 0.05f};
        static const float low[] = {0.05f, 0.40f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        static const int unvoiced_peak[] = {1, 0, 1, 1};
        static const int playback[] = {1, 1, 1, 1};
        static const int late_playback[] = {0, 0, 1, 1};
        run_sequence_mode(scores, voiced, playback, 4, &capture, &metrics);
        assert(capture.count == 1);
        assert(capture.events[0].playback_active);
        assert(capture.events[0].detection_sample == 2U * BLOCK_SAMPLES);
        run_sequence_mode(low, voiced, playback, 4, &capture, &metrics);
        assert(capture.count == 0);
        run_sequence_mode(scores, unvoiced_peak, playback, 4, &capture, &metrics);
        assert(capture.count == 0);
        run_sequence_mode(scores, voiced, late_playback, 4, &capture, &metrics);
        assert(capture.count == 0);
    }

    /*
     * The sequence measured on hardware. The 0.554 clears the 0.533 accept
     * threshold on frame 3, but only one of the three scores in that window
     * is above the support line, so frame 3 alone is not enough. Frame 4 adds
     * the second supporting score, and the peak is still visible in the
     * window -- so the detection fires on frame 4 and is attributed to the
     * frame that actually peaked.
     *
     * Judged by the newest frame instead, frame 4 would be tested as 0.414,
     * under the accept threshold, and the utterance would be lost. That is
     * the regression this guards.
     */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};
        static const int vad_active[] = {1, 1, 1, 1};

        run_sequence(measured, vad_active, 4, &capture, &metrics);
        assert(capture.count == 1);
        /* Attributed to frame 3's peak, not to frame 4's newest score. */
        assert(close_to(capture.events[0].score, 0.554f));
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
        assert(metrics.scores == 4);
        assert(metrics.dropped_blocks == 0);
        assert(!metrics.failed);
    }

    /*
     * A lone spike is still rejected. The peak clears the accept threshold by
     * a wide margin, but no neighbouring frame reaches the support line, so
     * support never exceeds one. This is what stops the peak rule degenerating
     * into "any single frame over the threshold wakes the device".
     */
    {
        static const float spike[] = {0.05f, 0.90f, 0.05f, 0.05f, 0.05f};
        static const int vad_active[] = {1, 1, 1, 1, 1};

        run_sequence(spike, vad_active, 5, &capture, &metrics);
        assert(capture.count == 0);
        assert(metrics.events == 0);
        assert(metrics.scores == 5);
        assert(!metrics.failed);
        /* The spike was seen and scored; it was judged, not missed. */
        assert(close_to(metrics.max_score, 0.90f));
    }

    /*
     * VAD gate still applies, and it is now read from the peak frame's
     * observation rather than the newest one. The peak frame is active while
     * the newest frame is inactive, so a decoder that reads the newest frame
     * would incorrectly suppress this event.
     */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};
        static const int vad_active[] = {1, 1, 1, 0};

        run_sequence(measured, vad_active, 4, &capture, &metrics);
        assert(capture.count == 1);
        assert(close_to(capture.events[0].score, 0.554f));
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
    }

    /*
     * Lockout is armed from the peak frame's sample. After the detection on
     * frame 3, the later pair of 0.60 scores would otherwise satisfy both the
     * accept threshold and support, but every one of their samples falls
     * inside the 28000-sample lockout, so exactly one event is reported.
     */
    {
        static const float repeated[] = {
            0.083f, 0.102f, 0.554f, 0.414f, 0.10f, 0.60f, 0.60f, 0.10f
        };
        static const int vad_active[] = {1, 1, 1, 1, 1, 1, 1, 1};

        run_sequence(repeated, vad_active, 8, &capture, &metrics);
        assert(capture.count == 1);
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
        assert(metrics.scores == 8);
        assert(metrics.dropped_blocks == 0);
    }

    /*
     * Two words. Each word decodes on its own scores and names itself in the
     * event. The first word, scored on the stock folder, is alexa_v0.1; the
     * second is the configured name.
     */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};
        static const float quiet[] = {0.05f, 0.05f, 0.05f, 0.05f};
        static const int vad_active[] = {1, 1, 1, 1};

        /* Only the second word fires: its event carries its own name. */
        run_two_words(quiet, measured, vad_active, 4, &capture, &metrics);
        assert(capture.count == 1);
        assert(strcmp(capture.events[0].model_name, "twin-word") == 0);
        assert(close_to(capture.events[0].score, 0.554f));
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
        assert(metrics.scores == 4);

        /* Only the first word fires. */
        run_two_words(measured, quiet, vad_active, 4, &capture, &metrics);
        assert(capture.count == 1);
        assert(strcmp(capture.events[0].model_name, "alexa_v0.1") == 0);
        assert(metrics.events == 1);

        /* Both fire on the same utterance: two events, first word first. */
        run_two_words(measured, measured, vad_active, 4, &capture, &metrics);
        assert(capture.count == 2);
        assert(strcmp(capture.events[0].model_name, "alexa_v0.1") == 0);
        assert(strcmp(capture.events[1].model_name, "twin-word") == 0);
        assert(metrics.events == 2);
        assert(metrics.scores == 4);

        /*
         * The single-word worker ignores the second slot: the same second
         * stream does nothing without a second classifier configured.
         */
        run_sequence(quiet, vad_active, 4, &capture, &metrics);
        assert(capture.count == 0);
        run_sequence_mode(quiet, vad_active, NULL, 4, &capture, &metrics);
        assert(capture.count == 0);
        assert(metrics.events == 0);
    }

    return 0;
}
