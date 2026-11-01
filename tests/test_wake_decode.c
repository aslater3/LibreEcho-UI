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

struct le_wake_engine *le_wake_engine_create(const char *model_directory,
                                             unsigned int threads)
{
    (void)model_directory;
    (void)threads;
    script_position = 0;
    return &fake_engine;
}

int le_wake_engine_feed(struct le_wake_engine *engine,
                        const int16_t *samples,
                        size_t count,
                        float *score,
                        int *new_score)
{
    (void)engine;
    (void)samples;
    (void)count;
    *score = script_position < script_count
        ? script_scores[script_position] : 0.0f;
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

/* ---- near-miss harness ---- */

struct near_capture {
    unsigned int count;
    struct le_wake_near_miss last;
    size_t last_samples;
    int16_t first_sample;
    int16_t newest_sample;
};

static void on_near_miss(const struct le_wake_near_miss *miss,
                         const int16_t *samples, size_t count, void *opaque)
{
    struct near_capture *capture = opaque;

    ++capture->count;
    capture->last = *miss;
    capture->last_samples = count;
    capture->first_sample = count ? samples[0] : 0;
    capture->newest_sample = count ? samples[count - 1] : 0;
}

/*
 * Like run_sequence_mode, but enables near-miss reporting after start. Block
 * i is filled with the value i+1 so the snapshot order can be checked.
 */
static void run_near_miss(const float *scores, const int *vad_active,
                          size_t count, float floor,
                          struct near_capture *near,
                          struct le_wake_worker_metrics *metrics,
                          int *set_result)
{
    struct le_wake_worker worker;
    struct capture ignored;
    int16_t block[BLOCK_SAMPLES];
    size_t i, k;

    assert(count <= MAX_FRAMES);
    memset(&worker, 0, sizeof(worker));
    memset(&ignored, 0, sizeof(ignored));
    memset(near, 0, sizeof(*near));
    script_scores = scores;
    script_count = count;

    assert(le_wake_worker_start(&worker, "unused-model-dir", 1,
                                ACCEPT_THRESHOLD, on_wake_event,
                                &ignored) == 0);
    *set_result = le_wake_worker_set_near_miss(&worker, floor,
                                               on_near_miss, near);
    for (i = 0; i < count; ++i) {
        struct le_wake_observation observation;

        for (k = 0; k < BLOCK_SAMPLES; ++k)
            block[k] = (int16_t)(i + 1);
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
     * Near misses. A peak between the 0.35 floor and the 0.533 accept
     * threshold, with speech present, is reported with no detection. The
     * snapshot is oldest-first and ends at the window that produced it.
     */
    {
        static const float low[] = {0.05f, 0.40f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(low, voiced, 4, 0.35f, &near, &metrics, &set_result);
        assert(set_result == 0);
        assert(near.count == 1);
        assert(metrics.near_misses == 1);
        assert(metrics.events == 0);
        assert(close_to(near.last.score, 0.40f));
        assert(near.last.detection_sample == 2u * BLOCK_SAMPLES);
        assert(close_to(near.last.accept_threshold, ACCEPT_THRESHOLD));
        /* Frame 2 is the peak: history holds frames 1 and 2 at the time. */
        assert(near.last_samples == 2u * BLOCK_SAMPLES);
        assert(near.first_sample == 1);
        assert(near.newest_sample == 2);
    }

    /* Floor 0 is off: the same window reports nothing. */
    {
        static const float low[] = {0.05f, 0.40f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(low, voiced, 4, 0.0f, &near, &metrics, &set_result);
        assert(set_result == 0);
        assert(near.count == 0);
        assert(metrics.near_misses == 0);
    }

    /* Unvoiced peaks are noise, not speech, so they are never reported. */
    {
        static const float low[] = {0.05f, 0.40f, 0.05f, 0.05f};
        static const int unvoiced[] = {1, 0, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(low, unvoiced, 4, 0.35f, &near, &metrics, &set_result);
        assert(near.count == 0);
        assert(metrics.near_misses == 0);
    }

    /* A peak below the floor is not a near miss. */
    {
        static const float quiet[] = {0.05f, 0.20f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(quiet, voiced, 4, 0.35f, &near, &metrics, &set_result);
        assert(near.count == 0);
        assert(metrics.near_misses == 0);
    }

    /* A peak that is accepted is a detection, never also a near miss. */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(measured, voiced, 4, 0.35f, &near, &metrics, &set_result);
        assert(near.count == 0);
        assert(metrics.events == 1);
        assert(metrics.near_misses == 0);
    }

    /* One utterance yields one clip: the second, higher peak is inside the
     * near-miss lockout armed by the first. */
    {
        static const float twice[] = {0.40f, 0.42f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(twice, voiced, 4, 0.35f, &near, &metrics, &set_result);
        assert(near.count == 1);
        assert(metrics.near_misses == 1);
        assert(near.last.detection_sample == 1u * BLOCK_SAMPLES);
    }

    /* A floor at or above the accept threshold reports nothing. */
    {
        static const float low[] = {0.05f, 0.40f, 0.05f, 0.05f};
        static const int voiced[] = {1, 1, 1, 1};
        struct near_capture near;
        int set_result = -1;

        run_near_miss(low, voiced, 4, 0.60f, &near, &metrics, &set_result);
        assert(set_result == 0);
        assert(near.count == 0);
        assert(metrics.near_misses == 0);
    }

    /* Invalid floors and a missing callback are refused. */
    {
        struct le_wake_worker worker;
        struct capture ignored;

        memset(&worker, 0, sizeof(worker));
        memset(&ignored, 0, sizeof(ignored));
        script_scores = NULL;
        script_count = 0;
        assert(le_wake_worker_start(&worker, "unused-model-dir", 1,
                                    ACCEPT_THRESHOLD, on_wake_event,
                                    &ignored) == 0);
        assert(le_wake_worker_set_near_miss(&worker, 1.0f, on_near_miss, NULL) == -1);
        assert(le_wake_worker_set_near_miss(&worker, -0.1f, on_near_miss, NULL) == -1);
        assert(le_wake_worker_set_near_miss(&worker, 0.35f, NULL, NULL) == -1);
        assert(le_wake_worker_set_near_miss(&worker, 0.0f, NULL, NULL) == 0);
        le_wake_worker_stop(&worker, &metrics);
    }

    return 0;
}
