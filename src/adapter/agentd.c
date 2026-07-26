#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "llm_http.h"
#include "llm_provider.h"
#include "llm_store.h"
#include "voice_pipeline.h"
#include "voice_playback.h"
#include "voice_reply.h"
#include "../config_store.h"
#include "../json.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_AGENT_SOCKET LE_ADAPTER_AGENT_SOCK
#define DEFAULT_AGENT_CONFIG "/data/libreecho/config/agent.json"
#define DEFAULT_AGENT_CREDENTIALS \
    "/data/libreecho/secrets/openai-codex.json"
#define DEFAULT_CURL "/usr/local/libexec/libreecho-curl"
#define DEFAULT_MODEL "gpt-5.4"
#define DEFAULT_AUDIO_SOCKET LE_ADAPTER_AUDIO_SOCK
#define DEFAULT_TTS_SOCKET LE_ADAPTER_TTS_SOCK
#define DEFAULT_WAKE_SOCKET LE_ADAPTER_WAKEWORD_SOCK
#define DEFAULT_STT_SOCKET LE_ADAPTER_STT_SOCK
#define DEFAULT_TTS_FIRST_PCM_FILE "/run/libreecho/tts-first-pcm"

enum auth_state {
    AUTH_SIGNED_OUT,
    AUTH_WAITING,
    AUTH_SIGNED_IN,
    AUTH_ERROR
};

struct agent_config {
    int enabled;
    char provider[64];
    char model[96];
    char prompt[2048];
};

struct agent_state {
    const struct le_llm_provider *provider;
    struct le_llm_auth_session auth;
    struct le_llm_credentials credentials;
    struct agent_config config;
    enum auth_state auth_state;
    char auth_error[256];
    char socket_path[256];
    char config_path[384];
    char credentials_path[384];
    char curl_path[384];
    char audio_socket[256];
    char tts_socket[256];
    char wake_socket[256];
    char stt_socket[256];
    char tts_first_pcm_file[384];
    time_t next_auth_poll;
    unsigned int poll_minimum;
    struct le_voice_playback playback;
    struct le_voice_pipeline *voice_pipeline;
    pthread_mutex_t control_mutex;
    pthread_mutex_t metrics_mutex;
    uint64_t turn_started_ms;
    uint64_t first_text_ms;
    uint64_t first_announce_ms;
    uint64_t first_pcm_ms;
    unsigned long latency_violations;
    char turn_request_id[64];
    unsigned long completed_turns;
};

static volatile sig_atomic_t running = 1;

static void stop_handler(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL +
           (uint64_t)now.tv_nsec / 1000000ULL;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *position = buffer;

    while (size) {
        ssize_t count = write(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        ssize_t count = read(fd, buffer + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int respond(int fd, unsigned long id, int ok, const char *payload)
{
    char response[LE_ADAPTER_MSG_MAX];
    int length = ok
        ? le_adapter_respond_ok(response, sizeof(response), id, payload)
        : le_adapter_respond_err(response, sizeof(response), id, payload);

    return length < 0 ? -1 :
        write_all(fd, response, (size_t)length);
}

static const char *auth_state_name(enum auth_state state)
{
    switch (state) {
    case AUTH_WAITING: return "waiting";
    case AUTH_SIGNED_IN: return "signed_in";
    case AUTH_ERROR: return "error";
    default: return "signed_out";
    }
}

static void config_defaults(struct agent_config *config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    strcpy(config->provider, "openai-codex");
    strcpy(config->model, DEFAULT_MODEL);
    snprintf(config->prompt, sizeof(config->prompt), "%s",
             le_llm_default_voice_prompt());
}

static int escape_json(char *output, size_t size, const char *input)
{
    size_t used = 0;

    while (input && *input) {
        unsigned char c = (unsigned char)*input++;

        if (c == '"' || c == '\\' || c == '\n' ||
            c == '\r' || c == '\t') {
            if (used + 2 >= size)
                return -1;
            output[used++] = '\\';
            output[used++] = c == '\n' ? 'n' :
                             c == '\r' ? 'r' :
                             c == '\t' ? 't' : (char)c;
        } else if (c < 0x20U) {
            return -1;
        } else {
            if (used + 1 >= size)
                return -1;
            output[used++] = (char)c;
        }
    }
    output[used] = '\0';
    return 0;
}

static int save_config(const struct agent_state *state)
{
    char prompt[sizeof(state->config.prompt) * 2U];
    char model[sizeof(state->config.model) * 2U];
    char json[sizeof(prompt) + 512U];
    int length;

    if (escape_json(prompt, sizeof(prompt), state->config.prompt) < 0 ||
        escape_json(model, sizeof(model), state->config.model) < 0)
        return -1;
    length = snprintf(
        json, sizeof(json),
        "{\"version\":1,\"enabled\":%s,"
        "\"provider\":\"openai-codex\",\"model\":\"%s\","
        "\"prompt\":\"%s\"}\n",
        state->config.enabled ? "true" : "false", model, prompt);
    return length > 0 && length < (int)sizeof(json)
        ? config_write_atomic(state->config_path, json, (size_t)length)
        : -1;
}

static void load_config(struct agent_state *state)
{
    char json[8192];
    int enabled;

    config_defaults(&state->config);
    if (config_read(state->config_path, json, sizeof(json)) < 0)
        return;
    if (json_get_bool(json, "enabled", &enabled) > 0)
        state->config.enabled = enabled;
    (void)json_get_string(json, "model", state->config.model,
                          sizeof(state->config.model));
    (void)json_get_string(json, "prompt", state->config.prompt,
                          sizeof(state->config.prompt));
    if (!state->config.model[0])
        strcpy(state->config.model, DEFAULT_MODEL);
    if (!state->config.prompt[0])
        snprintf(state->config.prompt, sizeof(state->config.prompt),
                 "%s", le_llm_default_voice_prompt());
}

static int http_call(struct agent_state *state,
                     const struct le_llm_http_request *request,
                     struct le_llm_http_response *response)
{
    return le_llm_http_execute(state->curl_path, request,
                               NULL, NULL, response);
}

static int adapter_call(const char *socket_path, int timeout_ms,
                        const char *command, const char *args,
                        char *response, size_t response_size)
{
    struct le_adapter *adapter =
        le_adapter_connect(socket_path, timeout_ms);
    int result;

    if (!adapter)
        return -1;
    result = le_adapter_call(adapter, command, args,
                             response, response_size);
    le_adapter_close(adapter);
    return result;
}

static int play_sentence(void *context, const char *text)
{
    struct agent_state *state = context;
    char escaped[LE_VOICE_REPLY_SEGMENT_MAX * 2U];
    char args[sizeof(escaped) + 128U];
    char request_id[sizeof(state->turn_request_id)];
    char response[LE_ADAPTER_MSG_MAX];
    struct timespec delay = {0, 50000000L};
    int length;
    unsigned int attempt;

    if (escape_json(escaped, sizeof(escaped), text) < 0)
        return -1;
    pthread_mutex_lock(&state->metrics_mutex);
    snprintf(request_id, sizeof(request_id), "%s",
             state->turn_request_id);
    pthread_mutex_unlock(&state->metrics_mutex);
    length = snprintf(
        args, sizeof(args),
        "{\"text\":\"%s\",\"request_id\":\"%s\"}",
        escaped, request_id);
    if (length <= 0 || length >= (int)sizeof(args) ||
        adapter_call(state->audio_socket, 1000, "speak", args,
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return -1;
    pthread_mutex_lock(&state->metrics_mutex);
    if (!state->first_announce_ms)
        state->first_announce_ms =
            monotonic_milliseconds() - state->turn_started_ms;
    pthread_mutex_unlock(&state->metrics_mutex);

    /*
     * ttsd acknowledges before synthesis. It is intentionally single-flight,
     * so wait until its status endpoint becomes responsive and idle before
     * dequeuing another sentence. During in-process synthesis the status call
     * times out; this keeps the provider stream unblocked in its own thread.
     */
    if (access(state->tts_socket, F_OK) != 0)
        return 0;
    for (attempt = 0; attempt < 600 && running; ++attempt) {
        FILE *marker = fopen(state->tts_first_pcm_file, "r");

        if (marker) {
            char marker_id[64];
            unsigned long long marker_ms;

            if (fscanf(marker, "%63s %llu",
                       marker_id, &marker_ms) == 2 &&
                !strcmp(marker_id, request_id)) {
                pthread_mutex_lock(&state->metrics_mutex);
                if (!state->first_pcm_ms &&
                    marker_ms >= state->turn_started_ms) {
                    state->first_pcm_ms =
                        marker_ms - state->turn_started_ms;
                    if (state->first_pcm_ms > 3000)
                        ++state->latency_violations;
                }
                pthread_mutex_unlock(&state->metrics_mutex);
            }
            fclose(marker);
        }
        if (adapter_call(state->tts_socket, 250, "status", NULL,
                         response, sizeof(response)) == LE_ADAPTER_OK &&
            strstr(response, "\"speaking\":false"))
            return 0;
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int command_status(struct agent_state *state, int fd,
                          unsigned long id)
{
    struct le_voice_pipeline_metrics voice_metrics;
    char prompt[sizeof(state->config.prompt) * 2U];
    char model[sizeof(state->config.model) * 2U];
    char code[sizeof(state->auth.user_code) * 2U];
    char url[sizeof(state->auth.verification_url) * 2U];
    char error[sizeof(state->auth_error) * 2U];
    char payload[LE_ADAPTER_MSG_MAX];
    uint64_t first_text_ms;
    uint64_t first_announce_ms;
    uint64_t first_pcm_ms;
    unsigned long completed_turns;
    unsigned long latency_violations;

    memset(&voice_metrics, 0, sizeof(voice_metrics));
    le_voice_pipeline_get_metrics(
        state->voice_pipeline, &voice_metrics);
    pthread_mutex_lock(&state->metrics_mutex);
    first_text_ms = state->first_text_ms;
    first_announce_ms = state->first_announce_ms;
    first_pcm_ms = state->first_pcm_ms;
    completed_turns = state->completed_turns;
    latency_violations = state->latency_violations;
    pthread_mutex_unlock(&state->metrics_mutex);

    if (escape_json(prompt, sizeof(prompt), state->config.prompt) < 0 ||
        escape_json(model, sizeof(model), state->config.model) < 0 ||
        escape_json(code, sizeof(code), state->auth.user_code) < 0 ||
        escape_json(url, sizeof(url), state->auth.verification_url) < 0 ||
        escape_json(error, sizeof(error), state->auth_error) < 0 ||
        snprintf(
            payload, sizeof(payload),
            "{\"ready\":true,\"enabled\":%s,"
            "\"provider\":\"%s\",\"provider_name\":\"%s\","
            "\"subscription_auth\":true,\"authenticated\":%s,"
            "\"auth_state\":\"%s\",\"user_code\":\"%s\","
            "\"verification_url\":\"%s\",\"auth_error\":\"%s\","
            "\"model\":\"%s\",\"prompt\":\"%s\","
            "\"voice_pipeline\":true,\"text_streaming\":true,"
            "\"wake_connected\":%s,\"audio_connected\":%s,"
            "\"recognizing\":%s,\"dispatching\":%s,"
            "\"wake_events\":%lu,\"completed_transcripts\":%lu,"
            "\"dropped_voice_turns\":%lu,"
            "\"last_stt_audio_ms\":%llu,"
            "\"last_stt_processing_ms\":%llu,"
            "\"latency_target_ms\":3000,\"completed_turns\":%lu,"
            "\"last_first_text_ms\":%llu,"
            "\"last_first_announce_dispatch_ms\":%llu,"
            "\"last_speech_end_to_first_pcm_ms\":%llu,"
            "\"latency_target_met\":%s,"
            "\"latency_violations\":%lu}",
            state->config.enabled ? "true" : "false",
            state->provider->id, state->provider->name,
            state->auth_state == AUTH_SIGNED_IN ? "true" : "false",
            auth_state_name(state->auth_state), code, url, error,
            model, prompt,
            voice_metrics.wake_connected ? "true" : "false",
            voice_metrics.audio_connected ? "true" : "false",
            voice_metrics.recognizing ? "true" : "false",
            voice_metrics.dispatching ? "true" : "false",
            voice_metrics.wake_events,
            voice_metrics.completed_transcripts,
            voice_metrics.dropped_turns,
            (unsigned long long)voice_metrics.last_stt_audio_ms,
            (unsigned long long)voice_metrics.last_stt_processing_ms,
            completed_turns,
            (unsigned long long)first_text_ms,
            (unsigned long long)first_announce_ms,
            (unsigned long long)first_pcm_ms,
            first_pcm_ms && first_pcm_ms <= 3000
                ? "true" : "false",
            latency_violations) >=
            (int)sizeof(payload))
        return respond(fd, id, 0, "status response too large");
    return respond(fd, id, 1, payload);
}

static int refresh_credentials(struct agent_state *state, int force)
{
    struct le_llm_http_request request;
    struct le_llm_http_response response;
    time_t now = time(NULL);

    if (!force && state->credentials.expires_at > now + 120)
        return 0;
    if (state->provider->refresh_request(
            &state->credentials, &request) < 0 ||
        http_call(state, &request, &response) < 0 ||
        state->provider->token_response(
            response.status, response.body,
            &state->credentials) < 0 ||
        le_llm_credentials_save(
            state->credentials_path, &state->credentials) < 0)
        return -1;
    return 0;
}

struct response_stream {
    struct agent_state *state;
    struct le_voice_reply reply;
    char full_text[LE_LLM_TEXT_MAX];
    size_t used;
    int complete;
};

static int enqueue_segment(void *context, const char *text)
{
    struct response_stream *stream = context;

    return le_voice_playback_enqueue(
        &stream->state->playback, text);
}

static int response_event(void *context, const char *data)
{
    struct response_stream *stream = context;
    char delta[1024];
    int result = stream->state->provider->stream_event(
        data, delta, sizeof(delta));

    if (result == LE_LLM_STREAM_IGNORED)
        return 0;
    if (result == LE_LLM_STREAM_ERROR)
        return -1;
    if (result == LE_LLM_STREAM_COMPLETE) {
        stream->complete = 1;
        return le_voice_reply_finish(&stream->reply);
    }
    pthread_mutex_lock(&stream->state->metrics_mutex);
    if (!stream->state->first_text_ms)
        stream->state->first_text_ms =
            monotonic_milliseconds() -
            stream->state->turn_started_ms;
    pthread_mutex_unlock(&stream->state->metrics_mutex);
    if (stream->used + strlen(delta) >= sizeof(stream->full_text))
        return -1;
    memcpy(stream->full_text + stream->used,
           delta, strlen(delta) + 1);
    stream->used += strlen(delta);
    return le_voice_reply_feed(&stream->reply, delta);
}

static int generate_response(struct agent_state *state,
                             const char *transcript,
                             uint64_t latency_start_ms,
                             char *full_text, size_t full_text_size,
                             char *error, size_t error_size)
{
    struct le_llm_http_request request;
    struct le_llm_http_response response;
    struct response_stream stream;
    if (state->auth_state != AUTH_SIGNED_IN) {
        snprintf(error, error_size, "%s",
                 "ChatGPT is not authenticated");
        return -1;
    }
    if (le_voice_playback_begin_turn(&state->playback) < 0) {
        snprintf(error, error_size, "%s",
                 "a voice response is already playing");
        return -1;
    }
    if (refresh_credentials(state, 0) < 0) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "ChatGPT token refresh failed");
        snprintf(error, error_size, "%s", state->auth_error);
        return -1;
    }
    memset(&stream, 0, sizeof(stream));
    stream.state = state;
    le_voice_reply_init(&stream.reply, enqueue_segment, &stream);
    pthread_mutex_lock(&state->metrics_mutex);
    state->turn_started_ms = latency_start_ms
        ? latency_start_ms : monotonic_milliseconds();
    state->first_text_ms = 0;
    state->first_announce_ms = 0;
    state->first_pcm_ms = 0;
    snprintf(state->turn_request_id,
             sizeof(state->turn_request_id), "%llu",
             (unsigned long long)monotonic_milliseconds());
    pthread_mutex_unlock(&state->metrics_mutex);
    (void)unlink(state->tts_first_pcm_file);
    memset(&response, 0, sizeof(response));
    if (state->provider->response_request(
            &state->credentials, state->config.model,
            state->config.prompt, transcript, &request) < 0 ||
        le_llm_http_execute(
            state->curl_path, &request, response_event,
            &stream, &response) < 0 ||
        response.status != 200) {
        if (response.status == 401 &&
            refresh_credentials(state, 1) == 0 &&
            state->provider->response_request(
                &state->credentials, state->config.model,
                state->config.prompt, transcript, &request) == 0) {
            memset(&stream, 0, sizeof(stream));
            stream.state = state;
            le_voice_reply_init(&stream.reply, enqueue_segment, &stream);
            if (le_llm_http_execute(
                    state->curl_path, &request, response_event,
                    &stream, &response) < 0 ||
                response.status != 200) {
                snprintf(error, error_size, "%s",
                         "ChatGPT response failed after refresh");
                return -1;
            }
        } else {
            snprintf(error, error_size, "%s",
                     "ChatGPT response failed");
            return -1;
        }
    }
    if (!stream.complete &&
        le_voice_reply_finish(&stream.reply) < 0) {
        snprintf(error, error_size, "%s",
                 "reply playback queue is full");
        return -1;
    }
    if (stream.used + 1 > full_text_size) {
        snprintf(error, error_size, "%s",
                 "response text is too large");
        return -1;
    }
    memcpy(full_text, stream.full_text, stream.used + 1);
    pthread_mutex_lock(&state->metrics_mutex);
    ++state->completed_turns;
    pthread_mutex_unlock(&state->metrics_mutex);
    return 0;
}

static int command_respond(struct agent_state *state, const char *args,
                           int fd, unsigned long id)
{
    char transcript[LE_LLM_TEXT_MAX];
    char full_text[LE_LLM_TEXT_MAX];
    char escaped[LE_LLM_TEXT_MAX * 2U];
    char error[256];
    char payload[LE_ADAPTER_MSG_MAX];
    uint64_t first_text_ms;
    int length;

    if (json_get_string(args, "text", transcript,
                        sizeof(transcript)) < 1 ||
        !transcript[0])
        return respond(fd, id, 0, "text is required");
    if (generate_response(
            state, transcript, 0, full_text, sizeof(full_text),
            error, sizeof(error)) < 0)
        return respond(fd, id, 0, error);
    if (escape_json(escaped, sizeof(escaped), full_text) < 0)
        return respond(fd, id, 0, "response text is too large");
    pthread_mutex_lock(&state->metrics_mutex);
    first_text_ms = state->first_text_ms;
    pthread_mutex_unlock(&state->metrics_mutex);
    length = snprintf(
        payload, sizeof(payload),
        "{\"queued\":true,\"text\":\"%s\","
        "\"first_text_ms\":%llu}",
        escaped, (unsigned long long)first_text_ms);
    return length > 0 && length < (int)sizeof(payload)
        ? respond(fd, id, 1, payload)
        : respond(fd, id, 0, "response is too large");
}

static void voice_transcript(
    void *context, const char *text,
    const struct le_voice_pipeline_turn *turn)
{
    struct agent_state *state = context;
    char reply[LE_LLM_TEXT_MAX];
    char error[256];

    pthread_mutex_lock(&state->control_mutex);
    if (state->config.enabled &&
        state->auth_state == AUTH_SIGNED_IN) {
        fprintf(stderr,
                "agentd: transcript detection_sample=%llu "
                "stt_audio_ms=%llu stt_processing_ms=%llu text=%s\n",
                (unsigned long long)turn->detection_sample,
                (unsigned long long)turn->stt_audio_ms,
                (unsigned long long)turn->stt_processing_ms,
                text);
        if (generate_response(
                state, text,
                turn->endpoint && turn->transcript_received_ms >= 500
                    ? turn->transcript_received_ms - 500 : 0,
                reply, sizeof(reply),
                error, sizeof(error)) < 0)
            fprintf(stderr, "agentd: voice response failed: %s\n",
                    error);
    }
    pthread_mutex_unlock(&state->control_mutex);
}

static int command_auth_start(struct agent_state *state, int fd,
                              unsigned long id)
{
    struct le_llm_http_request request;
    struct le_llm_http_response response;

    memset(&state->auth, 0, sizeof(state->auth));
    state->auth_error[0] = '\0';
    if (state->provider->auth_start_request(&request) < 0 ||
        http_call(state, &request, &response) < 0 ||
        state->provider->auth_start_response(
            response.status, response.body, &state->auth) < 0) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "Unable to start ChatGPT device login");
        return respond(fd, id, 0, state->auth_error);
    }
    state->auth_state = AUTH_WAITING;
    state->next_auth_poll =
        time(NULL) + (time_t)state->poll_minimum;
    return command_status(state, fd, id);
}

static int command_auth_poll(struct agent_state *state, int fd,
                             unsigned long id)
{
    struct le_llm_http_request request;
    struct le_llm_http_response response;
    time_t now = time(NULL);
    int poll_result;

    if (state->auth_state != AUTH_WAITING)
        return command_status(state, fd, id);
    if (now >= state->auth.expires_at) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "Device login code expired");
        return command_status(state, fd, id);
    }
    if (now < state->next_auth_poll)
        return command_status(state, fd, id);
    state->next_auth_poll = now +
        (time_t)(state->auth.interval_seconds > state->poll_minimum
            ? state->auth.interval_seconds : state->poll_minimum);
    if (state->provider->auth_poll_request(&state->auth, &request) < 0 ||
        http_call(state, &request, &response) < 0) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "ChatGPT login polling failed");
        return command_status(state, fd, id);
    }
    poll_result = state->provider->auth_poll_response(
        response.status, response.body, &state->auth);
    if (poll_result == 0)
        return command_status(state, fd, id);
    if (poll_result < 0 ||
        state->provider->token_exchange_request(
            &state->auth, &request) < 0 ||
        http_call(state, &request, &response) < 0 ||
        state->provider->token_response(
            response.status, response.body, &state->credentials) < 0 ||
        le_llm_credentials_save(
            state->credentials_path, &state->credentials) < 0) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "ChatGPT token exchange failed");
        return command_status(state, fd, id);
    }
    memset(&state->auth, 0, sizeof(state->auth));
    state->auth_state = AUTH_SIGNED_IN;
    state->auth_error[0] = '\0';
    return command_status(state, fd, id);
}

static int command_logout(struct agent_state *state, int fd,
                          unsigned long id)
{
    if (le_llm_credentials_remove(state->credentials_path) < 0)
        return respond(fd, id, 0, "Unable to remove credentials");
    le_llm_credentials_clear(&state->credentials);
    memset(&state->auth, 0, sizeof(state->auth));
    state->auth_state = AUTH_SIGNED_OUT;
    state->auth_error[0] = '\0';
    return command_status(state, fd, id);
}

static int command_configure(struct agent_state *state, const char *args,
                             int fd, unsigned long id)
{
    struct agent_config updated = state->config;
    char value[2048];
    int boolean;
    int parsed;

    parsed = json_get_bool(args, "enabled", &boolean);
    if (parsed < 0)
        return respond(fd, id, 0, "enabled must be a boolean");
    if (parsed > 0)
        updated.enabled = boolean;
    parsed = json_get_string(args, "provider", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && strcmp(value, "openai-codex")))
        return respond(fd, id, 0, "unsupported provider");
    parsed = json_get_string(args, "model", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && (!value[0] || strlen(value) >=
                                     sizeof(updated.model))))
        return respond(fd, id, 0, "invalid model");
    if (parsed > 0)
        strcpy(updated.model, value);
    parsed = json_get_string(args, "prompt", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && (!value[0] || strlen(value) >=
                                     sizeof(updated.prompt))))
        return respond(fd, id, 0, "invalid prompt");
    if (parsed > 0)
        strcpy(updated.prompt, value);
    state->config = updated;
    if (save_config(state) < 0)
        return respond(fd, id, 0, "unable to save agent configuration");
    return command_status(state, fd, id);
}

static void handle_client(struct agent_state *state, int client_fd)
{
    char message[LE_ADAPTER_MSG_MAX];
    char command[64];
    char *args = NULL;
    unsigned long id = 0;

    if (read_line(client_fd, message, sizeof(message)) < 0 ||
        le_adapter_parse_request(message, command, sizeof(command),
                                 &args, &id) < 0) {
        (void)respond(client_fd, id, 0, "malformed request");
        return;
    }
    pthread_mutex_lock(&state->control_mutex);
    if (!strcmp(command, "status"))
        (void)command_status(state, client_fd, id);
    else if (!strcmp(command, "auth_start"))
        (void)command_auth_start(state, client_fd, id);
    else if (!strcmp(command, "auth_poll"))
        (void)command_auth_poll(state, client_fd, id);
    else if (!strcmp(command, "logout"))
        (void)command_logout(state, client_fd, id);
    else if (!strcmp(command, "configure"))
        (void)command_configure(state, args, client_fd, id);
    else if (!strcmp(command, "respond"))
        (void)command_respond(state, args, client_fd, id);
    else
        (void)respond(client_fd, id, 0, "unknown command");
    pthread_mutex_unlock(&state->control_mutex);
}

int main(int argc, char **argv)
{
    struct agent_state state;
    const char *poll_minimum;
    int listener;
    int i;

    memset(&state, 0, sizeof(state));
    strcpy(state.socket_path, DEFAULT_AGENT_SOCKET);
    strcpy(state.config_path, DEFAULT_AGENT_CONFIG);
    strcpy(state.credentials_path, DEFAULT_AGENT_CREDENTIALS);
    strcpy(state.curl_path, DEFAULT_CURL);
    strcpy(state.audio_socket, DEFAULT_AUDIO_SOCKET);
    strcpy(state.tts_socket, DEFAULT_TTS_SOCKET);
    strcpy(state.wake_socket, DEFAULT_WAKE_SOCKET);
    strcpy(state.stt_socket, DEFAULT_STT_SOCKET);
    strcpy(state.tts_first_pcm_file, DEFAULT_TTS_FIRST_PCM_FILE);
    state.poll_minimum = 3;
    for (i = 1; i < argc; ++i) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(argv[i], "--socket") && value) {
            snprintf(state.socket_path, sizeof(state.socket_path),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--config") && value) {
            snprintf(state.config_path, sizeof(state.config_path),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--credentials") && value) {
            snprintf(state.credentials_path,
                     sizeof(state.credentials_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--curl") && value) {
            snprintf(state.curl_path, sizeof(state.curl_path),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--audio-socket") && value) {
            snprintf(state.audio_socket, sizeof(state.audio_socket),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--tts-socket") && value) {
            snprintf(state.tts_socket, sizeof(state.tts_socket),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--wake-socket") && value) {
            snprintf(state.wake_socket, sizeof(state.wake_socket),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--stt-socket") && value) {
            snprintf(state.stt_socket, sizeof(state.stt_socket),
                     "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--tts-first-pcm-file") && value) {
            snprintf(state.tts_first_pcm_file,
                     sizeof(state.tts_first_pcm_file),
                     "%s", argv[++i]);
        } else {
            fprintf(stderr,
                    "usage: %s [--socket PATH] [--config PATH] "
                    "[--credentials PATH] [--curl PATH] "
                    "[--audio-socket PATH] [--tts-socket PATH] "
                    "[--wake-socket PATH] [--stt-socket PATH] "
                    "[--tts-first-pcm-file PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    poll_minimum = getenv("LE_AGENT_AUTH_POLL_MIN_SECONDS");
    if (poll_minimum) {
        char *end;
        unsigned long value = strtoul(poll_minimum, &end, 10);

        if (!*end && value <= 30)
            state.poll_minimum = (unsigned int)value;
    }
    state.provider = le_llm_provider_by_id("openai-codex");
    if (!state.provider)
        return 1;
    load_config(&state);
    if (le_llm_credentials_load(state.credentials_path,
                                &state.credentials) == 0)
        state.auth_state = AUTH_SIGNED_IN;
    else
        state.auth_state = AUTH_SIGNED_OUT;
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);
    if (pthread_mutex_init(&state.control_mutex, NULL) != 0)
        return 1;
    if (pthread_mutex_init(&state.metrics_mutex, NULL) != 0) {
        pthread_mutex_destroy(&state.control_mutex);
        return 1;
    }
    if (le_voice_playback_start(
            &state.playback, play_sentence, &state) < 0)
        goto fail_mutex;
    listener = le_adapter_listen(state.socket_path);
    if (listener < 0) {
        perror("agentd: listen");
        le_voice_playback_stop(&state.playback);
        goto fail_mutex;
    }
    state.voice_pipeline = le_voice_pipeline_start(
        state.wake_socket, state.stt_socket,
        voice_transcript, &state);
    if (!state.voice_pipeline) {
        close(listener);
        unlink(state.socket_path);
        le_voice_playback_stop(&state.playback);
        goto fail_mutex;
    }
    fprintf(stderr, "agentd: ready socket=%s provider=%s\n",
            state.socket_path, state.provider->id);
    while (running) {
        int client_fd = le_adapter_accept(listener);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(&state, client_fd);
        close(client_fd);
    }
    close(listener);
    unlink(state.socket_path);
    le_voice_pipeline_stop(state.voice_pipeline);
    le_voice_playback_stop(&state.playback);
    le_llm_credentials_clear(&state.credentials);
    memset(&state.auth, 0, sizeof(state.auth));
    pthread_mutex_destroy(&state.metrics_mutex);
    pthread_mutex_destroy(&state.control_mutex);
    return running ? 1 : 0;

fail_mutex:
    pthread_mutex_destroy(&state.metrics_mutex);
    pthread_mutex_destroy(&state.control_mutex);
    return 1;
}
