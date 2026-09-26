#define _POSIX_C_SOURCE 200809L

#include "live_session.h"

#include <stdio.h>
#include <string.h>

/* Frame size the daemon feeds us; used for the input-time metrics. */
#define LE_LIVE_FEED_FRAME_SAMPLES 160U

static void copy_text(char *output, size_t size, const char *value)
{
    size_t length;

    if (!output || !size)
        return;
    if (!value)
        value = "";
    length = strlen(value);
    if (length >= size)
        length = size - 1;
    memcpy(output, value, length);
    output[length] = '\0';
}

/* JSON string escaping into a fixed buffer; truncates rather than overflows. */
static int append_escaped(char *out, size_t size, size_t *used,
                          const char *value)
{
    static const char hex[] = "0123456789abcdef";

    if (!out || !used || *used >= size)
        return -1;
    for (; value && *value; ++value) {
        unsigned char c = (unsigned char)*value;
        const char *replacement = NULL;
        char buffer[8];

        size_t replacement_length;

        if (c == '"' || c == '\\') {
            buffer[0] = '\\';
            buffer[1] = (char)c;
            buffer[2] = '\0';
            replacement = buffer;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            buffer[0] = '\\';
            buffer[1] = c == '\n' ? 'n' : (c == '\r' ? 'r' : 't');
            buffer[2] = '\0';
            replacement = buffer;
        } else if (c < 0x20U) {
            buffer[0] = '\\';
            buffer[1] = 'u';
            buffer[2] = '0';
            buffer[3] = '0';
            buffer[4] = hex[c >> 4];
            buffer[5] = hex[c & 15U];
            buffer[6] = '\0';
            replacement = buffer;
        } else {
            if (*used + 1 >= size)
                return -1;
            out[(*used)++] = (char)c;
            continue;
        }
        replacement_length = strlen(replacement);
        if (*used + replacement_length >= size)
            return -1;
        memcpy(out + *used, replacement, replacement_length);
        *used += replacement_length;
    }
    if (*used >= size)
        return -1;
    out[*used] = '\0';
    return 0;
}

static int escape_text(char *out, size_t size, const char *value)
{
    size_t used = 0;

    return append_escaped(out, size, &used, value);
}

const char *le_live_state_name(enum le_live_state state)
{
    switch (state) {
    case LE_LIVE_IDLE: return "idle";
    case LE_LIVE_CONNECTING: return "connecting";
    case LE_LIVE_LISTENING: return "listening";
    case LE_LIVE_SPEAKING: return "speaking";
    case LE_LIVE_WAITING_FOR_TOOL: return "waiting_for_tool";
    case LE_LIVE_CLOSING: return "closing";
    }
    return "unknown";
}

const char *le_live_end_reason_name(enum le_live_end_reason reason)
{
    switch (reason) {
    case LE_LIVE_END_NONE: return "none";
    case LE_LIVE_END_TIMEOUT: return "timeout";
    case LE_LIVE_END_MAX_DURATION: return "max_duration";
    case LE_LIVE_END_TRANSPORT_CLOSED: return "transport_closed";
    case LE_LIVE_END_TRANSPORT_ERROR: return "transport_error";
    case LE_LIVE_END_CONNECT_FAILED: return "connect_failed";
    case LE_LIVE_END_AUDIO_FAILED: return "audio_failed";
    case LE_LIVE_END_STOPPED: return "stopped";
    }
    return "unknown";
}

static void set_state(struct le_live_session *session, enum le_live_state state)
{
    if (session->state == state)
        return;
    session->state = state;
    if (session->ops.state_changed)
        session->ops.state_changed(session->context, state);
}

void le_live_session_init(struct le_live_session *session,
                          struct le_live_ring *ring,
                          const struct le_live_session_config *config,
                          const struct le_live_session_ops *ops,
                          void *context)
{
    if (!session)
        return;
    memset(session, 0, sizeof(*session));
    session->ring = ring;
    if (ops)
        session->ops = *ops;
    session->context = context;
    if (config)
        session->config = *config;

    if (!session->config.conversation_timeout_ms)
        session->config.conversation_timeout_ms = LE_LIVE_DEFAULT_TIMEOUT_MS;
    if (!session->config.max_session_ms)
        session->config.max_session_ms = LE_LIVE_DEFAULT_MAX_SESSION_MS;
    if (!session->config.connect_timeout_ms)
        session->config.connect_timeout_ms = LE_LIVE_DEFAULT_CONNECT_TIMEOUT_MS;
    if (!session->config.barge_in_frames)
        session->config.barge_in_frames = LE_LIVE_DEFAULT_BARGE_IN_FRAMES;
    if (!session->config.barge_in_factor)
        session->config.barge_in_factor = LE_LIVE_DEFAULT_BARGE_IN_FACTOR;
    if (!session->config.model)
        session->config.model = "gpt-live-1-codex";
    session->last_end = LE_LIVE_END_NONE;
}

int le_live_session_active(const struct le_live_session *session)
{
    return session && session->state != LE_LIVE_IDLE;
}

/* --- transcript --------------------------------------------------------- */

static void transcript_append(struct le_live_session *session,
                              enum le_live_speaker speaker, int final,
                              const char *text)
{
    struct le_live_turn *turn;

    /*
     * Streaming semantics: partial updates to the speaker whose turn is still
     * open rewrite that turn in place, and the final event closes it.  One
     * utterance is one turn, so the bounded buffer ends up holding what was
     * actually said rather than every intermediate revision of it.
     */
    if (session->turn_count) {
        struct le_live_turn *last = &session->turns[session->turn_next ?
            session->turn_next - 1 : LE_LIVE_TRANSCRIPT_TURNS - 1];

        if (last->speaker == speaker && !last->final) {
            copy_text(last->text, sizeof(last->text), text);
            last->final = final;
            return;
        }
    }
    turn = &session->turns[session->turn_next];
    turn->speaker = speaker;
    turn->final = final;
    copy_text(turn->text, sizeof(turn->text), text);
    session->turn_next = (session->turn_next + 1) % LE_LIVE_TRANSCRIPT_TURNS;
    if (session->turn_count < LE_LIVE_TRANSCRIPT_TURNS)
        ++session->turn_count;
}

void le_live_session_transcript_json(const struct le_live_session *session,
                                     char *out, size_t size)
{
    size_t used = 0;
    size_t i;
    int written;

    if (!out || !size)
        return;
    out[0] = '\0';
    if (!session)
        return;
    written = snprintf(out, size, "{\"turns\":[");
    if (written < 0 || (size_t)written >= size)
        return;
    used = (size_t)written;
    for (i = 0; i < session->turn_count; ++i) {
        size_t index = (session->turn_next + LE_LIVE_TRANSCRIPT_TURNS -
                        session->turn_count + i) % LE_LIVE_TRANSCRIPT_TURNS;
        const struct le_live_turn *turn = &session->turns[index];

        if (used + 48 >= size ||
            snprintf(out + used, size - used, "%s{\"role\":\"%s\",",
                     i ? "," : "",
                     turn->speaker == LE_LIVE_SPEAKER_USER
                         ? "user" : "assistant") < 0)
            return;
        while (used < size && out[used])
            ++used;
        if (used + 16 >= size ||
            snprintf(out + used, size - used, "\"final\":%s,\"text\":\"",
                     turn->final ? "true" : "false") < 0)
            return;
        while (used < size && out[used])
            ++used;
        if (append_escaped(out, size, &used, turn->text) < 0) {
            used = size - 1;
            out[used] = '\0';
            return;
        }
        /*
         * append_escaped leaves `used` on the terminating NUL, so the closing
         * quote of the turn is written over it and the buffer stays a valid
         * string as each turn is appended.
         */
        if (used + 3 >= size) {
            out[size - 1] = '\0';
            return;
        }
        memcpy(out + used, "\"}", 2);
        used += 2;
        out[used] = '\0';
    }
    if (used + 4 >= size)
        return;
    memcpy(out + used, "]}", 3);
    used += 2;
    out[used] = '\0';
}

/* --- delegation cache --------------------------------------------------- */

static struct le_live_delegation_record *delegation_lookup(
    struct le_live_session *session, const char *id)
{
    size_t i;

    if (!id || !id[0])
        return NULL;
    for (i = 0; i < LE_LIVE_DELEGATION_CACHE; ++i) {
        struct le_live_delegation_record *record = &session->delegation_cache[i];

        if (record->used && !strcmp(record->id, id))
            return record;
    }
    return NULL;
}

static void delegation_store(struct le_live_session *session, const char *id,
                             const char *tool, int ok, const char *result)
{
    struct le_live_delegation_record *record =
        &session->delegation_cache[session->delegation_next];

    copy_text(record->id, sizeof(record->id), id);
    copy_text(record->tool, sizeof(record->tool), tool);
    copy_text(record->result, sizeof(record->result), result);
    record->ok = ok;
    record->used = 1;
    session->delegation_next =
        (session->delegation_next + 1) % LE_LIVE_DELEGATION_CACHE;
}

static void handle_delegation(struct le_live_session *session,
                              const struct le_live_event *event,
                              uint64_t now_ms)
{
    struct le_live_delegation_record *replay;
    char result[LE_LIVE_ARGUMENT_MAX];
    int ok;

    result[0] = '\0';
    /*
     * Replay.  The same delegation id on a reconnected or retrying transport
     * must return the recorded answer, never run the action a second time.
     */
    replay = delegation_lookup(session, event->delegation_id);
    if (replay) {
        ++session->delegation_deduped;
        if (session->transport.ops->complete_delegation)
            (void)session->transport.ops->complete_delegation(
                &session->transport, replay->id, replay->result);
        return;
    }
    ++session->delegation_count;
    if (session->ops.dispatch)
        ok = session->ops.dispatch(session->context, event->tool,
                                    event->arguments, result, sizeof(result)) == 0;
    else {
        ok = 0;
        copy_text(result, sizeof(result), "unsupported");
    }
    if (!ok) {
        ++session->delegation_failures;
        if (!result[0])
            copy_text(result, sizeof(result), "unsupported");
    }
    delegation_store(session, event->delegation_id, event->tool, ok, result);
    if (session->transport.ops->complete_delegation)
        (void)session->transport.ops->complete_delegation(
            &session->transport, event->delegation_id, result);
    if (strstr(result, "\"end_session\":true")) {
        le_live_session_close(session, LE_LIVE_END_STOPPED, now_ms);
        return;
    }
    set_state(session, LE_LIVE_SPEAKING);
}

/* --- input forwarding --------------------------------------------------- */

static unsigned int frame_rms(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    size_t i;

    if (!count)
        return 0;
    for (i = 0; i < count; ++i) {
        int32_t sample = samples[i];

        energy += (uint64_t)(sample < 0 ? -sample : sample);
    }
    return (unsigned int)(energy / count);
}

/*
 * Send everything the session still owes upstream, oldest first.  The wake
 * point can sit behind the newest sample, so this drains the ring until it
 * catches up with what has been fed.
 */
static int flush_pending(struct le_live_session *session, uint64_t now_ms)
{
    int16_t chunk[LE_LIVE_AUDIO_SAMPLES];
    uint64_t end;

    if (!session->ring || !le_live_session_active(session))
        return 0;
    if (session->state == LE_LIVE_CONNECTING || session->state == LE_LIVE_CLOSING)
        return 0;
    end = le_live_ring_end(session->ring);
    while (session->next_send_sample < end) {
        size_t count = le_live_ring_read(session->ring,
                                         session->next_send_sample, chunk,
                                         LE_LIVE_AUDIO_SAMPLES);

        if (!count)
            break;
        if (!session->transport.ops->send_audio ||
            session->transport.ops->send_audio(&session->transport, chunk,
                                               count) < 0)
            return -1;
        session->next_send_sample += count;
        session->audio_input_ms +=
            (uint64_t)count * 1000U / LE_LIVE_INPUT_RATE;
    }
    (void)now_ms;
    return 0;
}

/*
 * Track what the room sounds like.
 *
 * This runs on every frame the daemon receives, including the idle frames
 * before a wake: the ambient level is a property of the room and the
 * microphone gain, not of the conversation.  Nothing here is ever transmitted;
 * it is one number used to decide whether a later frame is a person talking
 * over the model.
 */
static void observe_input(struct le_live_session *session,
                          const int16_t *samples, size_t count)
{
    unsigned int rms = frame_rms(samples, count);

    if (rms > session->input_peak_rms)
        session->input_peak_rms = rms;
    /*
     * Fall fast onto a quieter room, rise slowly out of a noisier one.  A barge
     * in must never be able to raise the threshold that is about to detect it.
     */
    if (!session->floor_primed) {
        session->input_floor_rms = rms;
        session->floor_primed = 1;
    } else if (rms < session->input_floor_rms) {
        session->input_floor_rms = rms;
    } else {
        session->input_floor_rms += (rms - session->input_floor_rms) / 64U;
    }
}

static void check_barge_in(struct le_live_session *session,
                           const int16_t *samples, size_t count, uint64_t now_ms)
{
    unsigned int rms = frame_rms(samples, count);
    unsigned int threshold = session->config.barge_in_rms;

    if (session->config.barge_in_factor) {
        unsigned int relative = session->input_floor_rms *
                                session->config.barge_in_factor;

        if (relative > threshold)
            threshold = relative;
    }
    if (session->state != LE_LIVE_SPEAKING) {
        session->barge_in_run = 0;
        return;
    }
    if (rms > session->speech_peak_rms)
        session->speech_peak_rms = rms;
    if (rms < threshold) {
        session->barge_in_run = 0;
        return;
    }
    ++session->barge_in_run;
    if (session->barge_in_run < session->config.barge_in_frames)
        return;
    /*
     * Confirmed.  Stop generating server-side, drop queued playback, and keep
     * the microphone path running: the model must not keep talking and must
     * not hear its own truncated tail as a new user turn.
     */
    session->barge_in_run = 0;
    ++session->barge_ins;
    if (session->transport.ops->interrupt)
        (void)session->transport.ops->interrupt(&session->transport);
    if (session->ops.cancel_output)
        session->ops.cancel_output(session->context);
    session->last_user_speech_ms = now_ms;
    set_state(session, LE_LIVE_LISTENING);
}

int le_live_session_feed(struct le_live_session *session, uint64_t first_sample,
                         const int16_t *samples, size_t count, uint64_t now_ms)
{
    int model_speaking;

    if (!session || !samples || !count)
        return -1;
    if (session->ring &&
        le_live_ring_append(session->ring, first_sample, samples, count) < 0)
        return -1;
    model_speaking = session->state == LE_LIVE_SPEAKING;
    if (le_live_session_active(session))
        check_barge_in(session, samples, count, now_ms);
    /* Model output and a user's barge-in are not ambient-room samples. Feeding
       either into the floor would raise the threshold while it is being used,
       but retain the observed peak for diagnostics. */
    if (!model_speaking)
        observe_input(session, samples, count);
    else {
        unsigned int rms = frame_rms(samples, count);

        if (rms > session->input_peak_rms)
            session->input_peak_rms = rms;
    }
    if (!le_live_session_active(session))
        return 0;
    if (flush_pending(session, now_ms) < 0) {
        copy_text(session->last_error, sizeof(session->last_error),
                  "audio upload failed");
        le_live_session_close(session, LE_LIVE_END_TRANSPORT_ERROR, now_ms);
        return -1;
    }
    return 0;
}

/* --- lifecycle ---------------------------------------------------------- */

int le_live_session_wake(struct le_live_session *session,
                         uint64_t detection_sample, uint64_t now_ms)
{
    struct le_live_transport_config transport_config;
    uint64_t preroll_samples;

    if (!session)
        return -1;
    if (le_live_session_active(session))
        return 1;
    if (!session->config.transport_ops)
        return -1;
    if (!session->ring || !session->ring->primed || !session->ring->count ||
        detection_sample > le_live_ring_end(session->ring)) {
        copy_text(session->last_error, sizeof(session->last_error),
                  "wake audio is unavailable");
        return -1;
    }

    session->transport.ops = session->config.transport_ops;
    memset(&transport_config, 0, sizeof(transport_config));
    transport_config.model = session->config.model;
    transport_config.voice = session->config.voice;
    transport_config.credentials_path = session->config.credentials_path;
    transport_config.url = session->config.url;
    transport_config.ca_path = session->config.ca_path;
    transport_config.mock_scenario = session->config.mock_scenario;

    preroll_samples = (uint64_t)session->config.wake_preroll_ms *
                      LE_LIVE_INPUT_RATE / 1000U;
    session->next_send_sample =
        le_live_ring_start_for_wake(session->ring, detection_sample,
                                    preroll_samples);
    session->opened_ms = now_ms;
    session->last_model_speech_ms = now_ms;
    session->last_user_speech_ms = now_ms;
    session->first_audio_at_ms = 0;
    session->audio_started = 0;
    session->barge_in_run = 0;
    session->speech_peak_rms = 0;
    session->last_error[0] = '\0';
    session->last_end = LE_LIVE_END_NONE;
    ++session->sessions_started;
    set_state(session, LE_LIVE_CONNECTING);

    if (session->transport.ops->start &&
        session->transport.ops->start(&session->transport, &transport_config,
                                      session->last_error,
                                      sizeof(session->last_error)) < 0) {
        if (!session->last_error[0])
            copy_text(session->last_error, sizeof(session->last_error),
                      "transport unavailable");
        ++session->sessions_failed;
        set_state(session, LE_LIVE_CLOSING);
        session->last_end = LE_LIVE_END_CONNECT_FAILED;
        set_state(session, LE_LIVE_IDLE);
        return -1;
    }
    return 0;
}

static void handle_event(struct le_live_session *session,
                         const struct le_live_event *event, uint64_t now_ms)
{
    switch (event->kind) {
    case LE_LIVE_EVENT_OPEN:
        session->last_connection_ms = now_ms - session->opened_ms;
        set_state(session, LE_LIVE_LISTENING);
        break;
    case LE_LIVE_EVENT_TRANSCRIPT:
        transcript_append(session, event->speaker, event->final, event->text);
        if (event->final) {
            if (event->speaker == LE_LIVE_SPEAKER_MODEL)
                session->last_model_speech_ms = now_ms;
            else
                session->last_user_speech_ms = now_ms;
        }
        break;
    case LE_LIVE_EVENT_AUDIO:
        if (session->ops.output_audio &&
            session->ops.output_audio(session->context, event->samples,
                                       event->count, event->rate) < 0) {
            copy_text(session->last_error, sizeof(session->last_error),
                      "audio output unavailable");
            le_live_session_close(session, LE_LIVE_END_AUDIO_FAILED, now_ms);
            return;
        }
        if (!session->audio_started) {
            session->audio_started = 1;
            session->first_audio_at_ms = now_ms;
            session->last_first_audio_ms = now_ms - session->opened_ms;
        }
        session->audio_output_ms +=
            (uint64_t)event->count * 1000U /
            (event->rate ? event->rate : LE_LIVE_INPUT_RATE);
        set_state(session, LE_LIVE_SPEAKING);
        break;
    case LE_LIVE_EVENT_OUTPUT_DONE:
        /*
         * The follow-up window starts when the model stops talking, so a
         * conversational reply does not need the wake word again.
         */
        session->last_model_speech_ms = now_ms;
        set_state(session, LE_LIVE_LISTENING);
        break;
    case LE_LIVE_EVENT_INPUT_STARTED:
        session->last_user_speech_ms = now_ms;
        break;
    case LE_LIVE_EVENT_DELEGATION:
        set_state(session, LE_LIVE_WAITING_FOR_TOOL);
        handle_delegation(session, event, now_ms);
        break;
    case LE_LIVE_EVENT_CLOSED:
        le_live_session_close(session, LE_LIVE_END_TRANSPORT_CLOSED, now_ms);
        break;
    case LE_LIVE_EVENT_ERROR:
        copy_text(session->last_error, sizeof(session->last_error),
                  event->detail);
        le_live_session_close(session, LE_LIVE_END_TRANSPORT_ERROR, now_ms);
        break;
    case LE_LIVE_EVENT_NONE:
    default:
        break;
    }
}

int le_live_session_pump(struct le_live_session *session, int timeout_ms,
                         uint64_t now_ms)
{
    struct le_live_event event;
    uint64_t reference;
    int result;

    if (!session || !le_live_session_active(session))
        return 0;

    memset(&event, 0, sizeof(event));
    result = session->transport.ops->poll
        ? session->transport.ops->poll(&session->transport, &event, timeout_ms)
        : 0;
    if (result < 0) {
        copy_text(session->last_error, sizeof(session->last_error),
                  "transport poll failed");
        le_live_session_close(session, LE_LIVE_END_TRANSPORT_ERROR, now_ms);
        return -1;
    }
    if (result > 0) {
        handle_event(session, &event, now_ms);
        if (!le_live_session_active(session))
            return 0;
    }

    /* Crypto-free wall-clock guards; all bounded by configuration. */
    if (session->state == LE_LIVE_CONNECTING) {
        if (now_ms - session->opened_ms >= session->config.connect_timeout_ms) {
            copy_text(session->last_error, sizeof(session->last_error),
                      "GPT-Live unavailable");
            le_live_session_close(session, LE_LIVE_END_CONNECT_FAILED, now_ms);
            return 0;
        }
        return 0;
    }
    if (now_ms - session->opened_ms >= session->config.max_session_ms) {
        le_live_session_close(session, LE_LIVE_END_MAX_DURATION, now_ms);
        return 0;
    }
    reference = session->last_model_speech_ms > session->last_user_speech_ms
        ? session->last_model_speech_ms : session->last_user_speech_ms;
    if (now_ms - reference >= session->config.conversation_timeout_ms) {
        le_live_session_close(session, LE_LIVE_END_TIMEOUT, now_ms);
        return 0;
    }
    if (flush_pending(session, now_ms) < 0) {
        copy_text(session->last_error, sizeof(session->last_error),
                  "audio upload failed");
        le_live_session_close(session, LE_LIVE_END_TRANSPORT_ERROR, now_ms);
        return -1;
    }
    return 0;
}

void le_live_session_close(struct le_live_session *session,
                           enum le_live_end_reason reason, uint64_t now_ms)
{
    if (!session || session->state == LE_LIVE_IDLE)
        return;
    set_state(session, LE_LIVE_CLOSING);
    /*
     * Cancel before stopping the transport: queued model audio must not survive
     * the session, and the playback sink has to be quiet before the next wake
     * re-arms.
     */
    if (session->ops.cancel_output)
        session->ops.cancel_output(session->context);
    if (session->transport.ops && session->transport.ops->stop)
        session->transport.ops->stop(&session->transport);
    session->last_end = reason;
    if (reason == LE_LIVE_END_TIMEOUT || reason == LE_LIVE_END_MAX_DURATION ||
        reason == LE_LIVE_END_STOPPED ||
        reason == LE_LIVE_END_TRANSPORT_CLOSED)
        ++session->sessions_completed;
    else
        ++session->sessions_failed;
    session->next_send_sample = 0;
    session->barge_in_run = 0;
    memset(session->delegation_cache, 0, sizeof(session->delegation_cache));
    session->delegation_next = 0;
    (void)now_ms;
    set_state(session, LE_LIVE_IDLE);
}

/* --- status ------------------------------------------------------------- */

void le_live_session_status_json(const struct le_live_session *session,
                                 char *out, size_t size)
{
    char escaped_model[LE_LIVE_TEXT_MAX * 2U + 1U];
    char escaped_error[LE_LIVE_TEXT_MAX * 6U + 1U];
    int written;

    if (!out || !size)
        return;
    out[0] = '\0';
    if (!session)
        return;
    if (escape_text(escaped_model, sizeof(escaped_model),
                    session->config.model ? session->config.model : "") < 0)
        snprintf(escaped_model, sizeof(escaped_model), "model name unavailable");
    if (escape_text(escaped_error, sizeof(escaped_error), session->last_error) < 0)
        snprintf(escaped_error, sizeof(escaped_error), "error detail unavailable");
    written = snprintf(
        out, size,
        "{\"state\":\"%s\",\"active\":%s,\"model\":\"%s\","
        "\"sessions_started\":%llu,\"sessions_completed\":%llu,"
        "\"sessions_failed\":%llu,\"last_end\":\"%s\","
        "\"audio_input_ms\":%llu,\"audio_output_ms\":%llu,"
        "\"delegations\":%llu,\"delegation_failures\":%llu,"
        "\"delegation_deduped\":%llu,\"barge_ins\":%llu,"
        "\"connection_ms\":%llu,\"first_audio_ms\":%llu,"
        "\"transcript_turns\":%u,\"conversation_timeout_ms\":%u,"
        "\"max_session_ms\":%u,\"barge_in_rms\":%u,"
        "\"barge_in_factor\":%u,\"input_floor_rms\":%u,"
        "\"input_peak_rms\":%u,\"speech_peak_rms\":%u,"
        "\"last_error\":\"%s\"}",
        le_live_state_name(session->state),
        le_live_session_active(session) ? "true" : "false",
        escaped_model,
        (unsigned long long)session->sessions_started,
        (unsigned long long)session->sessions_completed,
        (unsigned long long)session->sessions_failed,
        le_live_end_reason_name(session->last_end),
        (unsigned long long)session->audio_input_ms,
        (unsigned long long)session->audio_output_ms,
        (unsigned long long)session->delegation_count,
        (unsigned long long)session->delegation_failures,
        (unsigned long long)session->delegation_deduped,
        (unsigned long long)session->barge_ins,
        (unsigned long long)session->last_connection_ms,
        (unsigned long long)session->last_first_audio_ms,
        (unsigned)session->turn_count,
        session->config.conversation_timeout_ms,
        session->config.max_session_ms,
        session->config.barge_in_rms,
        session->config.barge_in_factor,
        session->input_floor_rms,
        session->input_peak_rms,
        session->speech_peak_rms,
        escaped_error);
    if (written < 0 || (size_t)written >= size)
        snprintf(out, size,
                 "{\"state\":\"%s\",\"active\":%s,"
                 "\"last_error\":\"status exceeds the adapter limit\"}",
                 le_live_state_name(session->state),
                 le_live_session_active(session) ? "true" : "false");
}
