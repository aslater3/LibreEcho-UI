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
static void run_sequence(const float *scores, const int *vad_active,
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
     * The peak frame's observation is what a detection is attributed to, not
     * the newest frame's. Kept from the original coverage: the peak frame is
     * active while the newest frame is inactive.
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
     * The energy VAD does not gate acceptance at all: the same sequence fires
     * with vad_active never set on any frame.
     *
     * This assertion is the inverse of what this file first required, and the
     * inversion is the point. Gating on the VAD was silently discarding real
     * detections -- over 22 hours of room audio, 34 of 38 above-threshold
     * blocks were refused for that reason alone, scoring as high as 0.9534,
     * and one wake event was delivered in the whole period. The gate wants
     * frame_energy > noise_energy * 6, which ordinary speech at this array
     * does not reach, so requiring it here would pin a detector that cannot
     * detect.
     */
    {
        static const float measured[] = {0.083f, 0.102f, 0.554f, 0.414f};
        static const int vad_active[] = {0, 0, 0, 0};

        run_sequence(measured, vad_active, 4, &capture, &metrics);
        assert(capture.count == 1);
        assert(close_to(capture.events[0].score, 0.554f));
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
    }

    /*
     * Removing the VAD veto must not loosen anything else. A lone spike is
     * still refused with the VAD inactive, so acceptance rests on the
     * classifier and the support rule rather than on frame energy.
     */
    {
        static const float spike[] = {0.05f, 0.90f, 0.05f, 0.05f, 0.05f};
        static const int vad_active[] = {0, 0, 0, 0, 0};

        run_sequence(spike, vad_active, 5, &capture, &metrics);
        assert(capture.count == 0);
        assert(metrics.events == 0);
        assert(close_to(metrics.max_score, 0.90f));
    }

    /*
     * And a sequence that never reaches the accept threshold stays refused
     * with the VAD inactive, so the threshold still does the gating.
     */
    {
        static const float quiet[] = {0.21f, 0.33f, 0.42f, 0.38f};
        static const int vad_active[] = {0, 0, 0, 0};

        run_sequence(quiet, vad_active, 4, &capture, &metrics);
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
        static const int vad_active[] = {1, 1, 1, 1, 1, 1, 1, 1};

        run_sequence(repeated, vad_active, 8, &capture, &metrics);
        assert(capture.count == 1);
        assert(capture.events[0].detection_sample == 3u * BLOCK_SAMPLES);
        assert(metrics.events == 1);
        assert(metrics.scores == 8);
        assert(metrics.dropped_blocks == 0);
    }

    return 0;
}
