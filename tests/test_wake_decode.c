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
static void run_sequence(const float *scores, size_t count, int vad_active,
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
        observation.vad_active = vad_active;
        observation.playback_active = 0;
        assert(le_wake_worker_submit(&worker, block, BLOCK_SAMPLES,
                                     &observation) == 0);
    }
    le_wake_worker_stop(&worker, metrics);
}

static int close_to(float actual, float expected)
{
    return fabsf(actual - expected) < 1e-6f;
}

int main(void)
{
    struct le_wake_worker_metrics metrics;
    struct capture capture;

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

        run_sequence(measured, 4, 1, &capture, &metrics);
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

        run_sequence(spike, 5, 1, &capture, &metrics);
        assert(capture.count == 0);
        assert(metrics.events == 0);
        assert(metrics.scores == 5);
        assert(!metrics.failed);
        /* The spike was seen and scored; it was judged, not missed. */
        assert(close_to(metrics.max_score, 0.90f));
    }

    /*
     * The VAD gate still applies, and it is now read from the peak frame's
     * observation rather than the newest one. With VAD inactive the same
     * measured sequence must not fire.
     */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};

        run_sequence(measured, 4, 0, &capture, &metrics);
        assert(capture.count == 0);
        assert(metrics.events == 0);
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

        run_sequence(repeated, 8, 1, &capture, &metrics);
        assert(capture.count == 1);
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
        assert(metrics.scores == 8);
        assert(metrics.dropped_blocks == 0);
    }

    return 0;
}
