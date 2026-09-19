#ifndef LIBREECHO_LIVE_SESSION_H
#define LIBREECHO_LIVE_SESSION_H

/*
 * GPT-Live conversation lifecycle.
 *
 * One session owns exactly one wake-triggered conversation.  It does no I/O of
 * its own: the transport carries model traffic, the ring carries preroll audio,
 * and the daemon supplies playback, tool dispatch and state notifications
 * through callbacks.  That split is what makes the state machine, the
 * exactly-once delegation cache and the timeout behaviour testable on the host
 * without a device, a network, or an OpenAI account.
 */

#include "live_ring.h"
#include "live_transport.h"

#include <stddef.h>
#include <stdint.h>

enum le_live_state {
    LE_LIVE_IDLE = 0,
    LE_LIVE_CONNECTING,
    LE_LIVE_LISTENING,
    LE_LIVE_SPEAKING,
    LE_LIVE_COOLDOWN,
    LE_LIVE_WAITING_FOR_TOOL,
    LE_LIVE_CLOSING
};

enum le_live_end_reason {
    LE_LIVE_END_NONE = 0,
    LE_LIVE_END_TIMEOUT,
    LE_LIVE_END_MAX_DURATION,
    LE_LIVE_END_TRANSPORT_CLOSED,
    LE_LIVE_END_TRANSPORT_ERROR,
    LE_LIVE_END_CONNECT_FAILED,
    LE_LIVE_END_AUDIO_FAILED,
    LE_LIVE_END_STOPPED
};

/*
 * Bounded transcript.  A speech-to-speech model never returns text, so the
 * daemon reconstructs just enough of one to delegate against, to show the last
 * conversation, and to debug a tool request.  Ten turns is the whole budget and
 * it never touches disk.
 */
#define LE_LIVE_TRANSCRIPT_TURNS 10U

/*
 * Bounded exactly-once cache.  A reconnected transport can replay a delegation
 * it already sent, and timers, media and (later) Home Assistant actions are not
 * safe to run twice.  Every delegation id we have executed is remembered with
 * its result so a replay returns the stored answer instead of acting again.
 */
#define LE_LIVE_DELEGATION_CACHE 16U

struct le_live_turn {
    enum le_live_speaker speaker;
    int final;
    char text[LE_LIVE_TEXT_MAX];
};

struct le_live_delegation_record {
    char id[LE_LIVE_DELEGATION_ID_MAX];
    char tool[LE_LIVE_TOOL_NAME_MAX];
    char result[LE_LIVE_ARGUMENT_MAX];
    int used;
    int ok;
};

struct le_live_session_ops {
    /* Model audio, already at the rate the model negotiated. */
    int (*output_audio)(void *context, const int16_t *samples, size_t count,
                        unsigned int rate);
    /* Discard everything queued for playback and stop the current period. */
    void (*cancel_output)(void *context);
    /* Run an allow-listed tool.  Returns 0 on success. */
    int (*dispatch)(void *context, const char *tool, const char *arguments,
                    char *result, size_t size);
    void (*state_changed)(void *context, enum le_live_state state);
    /* True only when the playback FIFO and hardware engine report empty. */
    int (*output_drained)(void *context);
    /* Backpressure pauses provider polling, never wake/control processing. */
    int (*output_ready)(void *context);
    uint64_t (*output_played_ms)(void *context);
};

struct le_live_session_config {
    const struct le_live_transport_ops *transport_ops;
    /* Silence after the model stops speaking before the session closes. */
    unsigned int conversation_timeout_ms;
    /* Hard ceiling on one conversation. */
    unsigned int max_session_ms;
    /* How long to wait for the transport to open before giving up. */
    unsigned int connect_timeout_ms;
    /* Audio sent upstream before the wake sample, to avoid clipping. */
    unsigned int wake_preroll_ms;
    /* Post-AEC RMS that counts as the user speaking over the model. */
    unsigned int barge_in_rms;
    /*
     * Barge-in is relative, not absolute.  A fixed RMS threshold cannot be
     * right on more than one device: measured on the MT8163 Echo, ambient
     * post-AEC audio peaks around 3000 RMS, which is far above any constant
     * that would also detect a person.  The threshold is therefore
     * `max(barge_in_rms, observed_floor * barge_in_factor)`, so it calibrates
     * itself to the room and the microphone gain while still refusing to fire
     * in a silent room on noise alone.
     */
    unsigned int barge_in_factor;
    /* Consecutive frames above the threshold before barge-in fires. */
    unsigned int barge_in_frames;
    const char *model;
    const char *voice;
    const char *instructions;
    const char *initial_text;
    int preview_only;
    /* Opt-in until the device's AEC/double-talk acceptance is measured. */
    int full_duplex;
    const char *credentials_path;
    /* Forwarded to the transport: see le_live_transport_config. */
    const char *url;
    const char *ca_path;
    const char *mock_scenario;
};

/* Sensible starting points; the release tunes these on hardware. */
#define LE_LIVE_DEFAULT_TIMEOUT_MS 15000U
#define LE_LIVE_DEFAULT_MAX_SESSION_MS 600000U
#define LE_LIVE_DEFAULT_CONNECT_TIMEOUT_MS 8000U
#define LE_LIVE_DEFAULT_WAKE_PREROLL_MS 150U
#define LE_LIVE_DEFAULT_BARGE_IN_RMS 900U
#define LE_LIVE_DEFAULT_BARGE_IN_FACTOR 6U
#define LE_LIVE_DEFAULT_BARGE_IN_FRAMES 3U
#define LE_LIVE_OUTPUT_DRAIN_STABLE_MS 80U
#define LE_LIVE_OUTPUT_DRAIN_TIMEOUT_MS 10000U

struct le_live_session {
    struct le_live_transport transport;
    /*
     * Held by value.  A pointer here is a lifetime hazard: the natural way to
     * write the caller is a block-scoped table, which leaves the session
     * pointing at a dead stack frame for the rest of its life.
     */
    struct le_live_session_ops ops;
    void *context;
    struct le_live_session_config config;
    struct le_live_ring *ring;

    enum le_live_state state;
    enum le_live_end_reason last_end;
    uint64_t opened_ms;
    uint64_t last_model_speech_ms;
    uint64_t last_user_speech_ms;
    uint64_t first_audio_at_ms;
    uint64_t drain_started_ms;
    uint64_t drain_quiet_since_ms;
    /* First sample not yet handed to the transport. */
    uint64_t next_send_sample;
    unsigned int barge_in_run;
    int audio_started;
    /*
     * Highest frame RMS seen on the post-AEC stream.  The barge-in threshold is
     * the one number in this design that cannot be chosen from the code: it
     * depends on the room, the microphone gain and the AEC tail.  Exposing the
     * observed peak turns that from a guess into a measurement.
     */
    unsigned int input_peak_rms;
    /* Highest frame RMS seen while the model was speaking. */
    unsigned int speech_peak_rms;
    /* Slow-tracking estimate of the ambient post-AEC level. */
    unsigned int input_floor_rms;
    int floor_primed;

    struct le_live_turn turns[LE_LIVE_TRANSCRIPT_TURNS];
    size_t turn_count;
    size_t turn_next;

    struct le_live_delegation_record delegation_cache[LE_LIVE_DELEGATION_CACHE];
    size_t delegation_next;

    uint64_t sessions_started;
    uint64_t sessions_completed;
    uint64_t sessions_failed;
    uint64_t audio_input_ms;
    uint64_t audio_output_ms;
    uint64_t delegation_count;
    uint64_t delegation_failures;
    uint64_t delegation_deduped;
    uint64_t barge_ins;
    uint64_t last_connection_ms;
    uint64_t last_first_audio_ms;
    uint64_t last_drain_ms;
    uint64_t drain_timeouts;
    char last_error[LE_LIVE_TEXT_MAX];
};

void le_live_session_init(struct le_live_session *session,
                          struct le_live_ring *ring,
                          const struct le_live_session_config *config,
                          const struct le_live_session_ops *ops,
                          void *context);

/*
 * Start a conversation for a wake detected at `detection_sample`.
 * Returns 0 when a session was started, 1 when one is already active (the
 * caller must not start a second pipeline), -1 on error.
 */
int le_live_session_wake(struct le_live_session *session,
                         uint64_t detection_sample, uint64_t now_ms);
int le_live_session_interrupt_for_wake(struct le_live_session *session,
                                       uint64_t detection_sample,
                                       uint64_t now_ms);

/*
 * Feed post-AEC audio that has just arrived from waked.  Frames are appended to
 * the ring; while a session is open they are also forwarded, starting with
 * whatever preroll the wake point requires.
 */
int le_live_session_feed(struct le_live_session *session, uint64_t first_sample,
                         const int16_t *samples, size_t count, uint64_t now_ms);

/* Drain transport events and enforce timeouts. */
int le_live_session_pump(struct le_live_session *session, int timeout_ms,
                         uint64_t now_ms);

/* End the conversation.  Safe to call when already idle. */
void le_live_session_close(struct le_live_session *session,
                           enum le_live_end_reason reason, uint64_t now_ms);

int le_live_session_active(const struct le_live_session *session);

const char *le_live_state_name(enum le_live_state state);
const char *le_live_end_reason_name(enum le_live_end_reason reason);

/* Bounded operational status.  Never includes audio or credentials. */
void le_live_session_status_json(const struct le_live_session *session,
                                 char *out, size_t size);

/*
 * Bounded transcript for the last conversation.  Only produced on request so
 * that nothing exposes speech content unless it is asked for.
 */
void le_live_session_transcript_json(const struct le_live_session *session,
                                     char *out, size_t size);

#endif
