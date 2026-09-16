#ifndef LIBREECHO_LIVE_TRANSPORT_H
#define LIBREECHO_LIVE_TRANSPORT_H

/*
 * GPT-Live transport boundary.
 *
 * libreecho-lived owns the voice-conversation lifecycle, the preroll ring, the
 * delegation policy and the audio path.  It does not own, and must not depend
 * on, a particular way of reaching the model.  Everything that knows how the
 * bytes actually travel sits behind this interface:
 *
 *   - mock  : deterministic, in-process, no network.  Used by the host tests
 *             and by the device hot-stage so the whole daemon can be exercised
 *             without an OpenAI account.
 *   - realtime: subscription- or key-authenticated transport to GPT-Live.
 *
 * The interface is deliberately narrow and pull-based: the daemon drives the
 * event loop, so there is no transport-owned thread and no unbounded internal
 * queue.  A transport must never block a poll() call for longer than the
 * timeout it is given.
 *
 * Bounded by design, matching the rest of the adapter layer:
 *   - one event per poll, copied into a caller-owned struct;
 *   - no dynamic allocation inside the transport;
 *   - audio in/out capped at LE_LIVE_AUDIO_SAMPLES per call.
 */

#include <stddef.h>
#include <stdint.h>

/* Canonical input format on the waked post-AEC stream. */
#define LE_LIVE_INPUT_RATE 16000
#define LE_LIVE_INPUT_CHANNELS 1

/* Upper bound for one chunk handed to or received from a transport. */
#define LE_LIVE_AUDIO_SAMPLES 1280U

/* Bounded sizes for model-supplied strings.  Nothing here is ever grown. */
#define LE_LIVE_TOOL_NAME_MAX 48
#define LE_LIVE_DELEGATION_ID_MAX 64
#define LE_LIVE_ARGUMENT_MAX 512
#define LE_LIVE_TEXT_MAX 512

enum le_live_event_kind {
    LE_LIVE_EVENT_NONE = 0,
    LE_LIVE_EVENT_OPEN,
    LE_LIVE_EVENT_TRANSCRIPT,
    /* Model speech.  Rate is reported by the transport; the playback path
       resamples to the speaker bus rate. */
    LE_LIVE_EVENT_AUDIO,
    /* Model asked for a local action.  Served through the allow-list. */
    LE_LIVE_EVENT_DELEGATION,
    /* Model finished speaking this turn: playback may drain. */
    LE_LIVE_EVENT_OUTPUT_DONE,
    /* User started a new turn (server-side VAD). */
    LE_LIVE_EVENT_INPUT_STARTED,
    LE_LIVE_EVENT_CLOSED,
    LE_LIVE_EVENT_ERROR
};

enum le_live_speaker {
    LE_LIVE_SPEAKER_USER = 0,
    LE_LIVE_SPEAKER_MODEL
};

struct le_live_event {
    enum le_live_event_kind kind;
    /* Transcript */
    enum le_live_speaker speaker;
    int final;
    char text[LE_LIVE_TEXT_MAX];
    /* Audio */
    unsigned int rate;
    size_t count;
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    /* Delegation */
    char delegation_id[LE_LIVE_DELEGATION_ID_MAX];
    char tool[LE_LIVE_TOOL_NAME_MAX];
    char arguments[LE_LIVE_ARGUMENT_MAX];
    /* Error/close */
    char detail[LE_LIVE_TEXT_MAX];
};

struct le_live_transport_config {
    /* Model identifier, e.g. "gpt-live-1-codex". */
    const char *model;
    /* Voice name from the provider's published set. */
    const char *voice;
    /* Path to the shared credential file (openai-codex.json). */
    const char *credentials_path;
    /*
     * Scenario selector for the mock transport; ignored by real transports.
     * Kept in the shared config so the daemon needs one configuration path.
     */
    const char *mock_scenario;
    unsigned int mock_seed;
};

struct le_live_transport;

struct le_live_transport_ops {
    const char *name;
    /*
     * Open a session.  On failure the transport writes one bounded, speakable
     * sentence into `detail` - the difference between "sign in again" and
     * "network is down" is the whole difference for the user, and only the
     * transport knows which it is.
     */
    int (*start)(struct le_live_transport *transport,
                 const struct le_live_transport_config *config,
                 char *detail, size_t detail_size);
    /* Forward post-AEC PCM.  Must not block; returns -1 to drop the session. */
    int (*send_audio)(struct le_live_transport *transport,
                      const int16_t *samples, size_t count);
    /*
     * Wait up to timeout_ms for one event.  Returns 1 when an event was
     * produced (caller inspects event->kind), 0 on timeout, -1 on failure.
     */
    int (*poll)(struct le_live_transport *transport,
                struct le_live_event *event, int timeout_ms);
    /*
     * Barge-in.  Cancels model output generation server-side and discards any
     * buffered model audio.  After this returns, no further AUDIO events may
     * be delivered for the cancelled turn.
     */
    int (*interrupt)(struct le_live_transport *transport);
    /* Return a delegation result to the model. */
    int (*complete_delegation)(struct le_live_transport *transport,
                               const char *delegation_id,
                               const char *result);
    void (*stop)(struct le_live_transport *transport);
    /*
     * Bounded operational metrics.  Implementations fill only the fields they
     * know and must not block.
     */
    void (*metrics)(const struct le_live_transport *transport, char *out,
                    size_t size);
};

struct le_live_transport {
    const struct le_live_transport_ops *ops;
    /* Implementation-private state.  Bounded; no dynamic growth. */
    unsigned char state[2048];
};

/* Transports available in this build. */
const struct le_live_transport_ops *le_live_transport_mock_ops(void);
/*
 * Realtime transport for the authenticated GPT-Live path.  Returns NULL when
 * the build has no usable transport (no TLS/WebRTC support compiled in), which
 * is reported as an explicit, bounded configuration error rather than a
 * silent fallback.
 */
const struct le_live_transport_ops *le_live_transport_realtime_ops(void);

#endif
