#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "llm_http.h"
#include "llm_provider.h"
#include "llm_store.h"
#include "voice_pipeline.h"
#include "voice_playback.h"
#include "timer_intent.h"
#include "voice_reply.h"
#include "../config_store.h"
#include "../json.h"
#include "../log.h"

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
#define DEFAULT_TIMER_SOCKET "/run/libreecho/timer.sock"
#define DEFAULT_TTS_SOCKET LE_ADAPTER_TTS_SOCK
#define DEFAULT_WAKE_SOCKET LE_ADAPTER_WAKEWORD_SOCK
#define DEFAULT_STT_SOCKET LE_ADAPTER_STT_SOCK
#define DEFAULT_TTS_FIRST_PCM_FILE "/run/libreecho/tts-first-pcm"
#define FOLLOW_UP_PLAYBACK_WAIT_MS 60000U
#define FOLLOW_UP_MAX_DEPTH 2U

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
    /*
     * Where the device is.  Without this the assistant has no way to
     * resolve "outside" and correctly refuses to answer weather questions;
     * it is stored here rather than in web-config.json because agentd is
     * the only consumer and already owns a persisted config.
     */
    char home_location[96];
    char latitude[16];
    char longitude[16];
    char weather_provider[24];
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
    char weather_text[160];
    time_t weather_fetched;
    char credentials_path[384];
    char curl_path[384];
    char audio_socket[256];
    char tts_socket[256];
    char timer_socket[256];
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
    char previous_voice_user[768];
    char previous_voice_reply[1536];
    unsigned int follow_up_depth;
    int follow_up_armed;
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

static void build_turn_prompt(struct agent_state *state, char *out,
                              size_t size);

static void config_defaults(struct agent_config *config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    strcpy(config->provider, "openai-codex");
    strcpy(config->model, DEFAULT_MODEL);
    snprintf(config->prompt, sizeof(config->prompt), "%s",
             le_llm_default_voice_prompt());
    strcpy(config->weather_provider, "open-meteo");
}

/*
 * Coordinates arrive as strings so they can be handed straight to the
 * weather query without reformatting a double.  Validate the text rather
 * than trusting it: it ends up in a URL, so anything that is not a plain
 * signed decimal is rejected outright.
 */
static int coordinate_valid(const char *text, double limit)
{
    char *end;
    double value;

    if (!text || !text[0] || strlen(text) > 15)
        return 0;
    errno = 0;
    value = strtod(text, &end);
    if (errno || *end || value < -limit || value > limit)
        return 0;
    return 1;
}

static int endpoint_valid(const char *url)
{
    const unsigned char *p = (const unsigned char *)url;

    if (!url || (strncmp(url, "http://", 7) &&
                 strncmp(url, "https://", 8)))
        return 0;
    for (; *p; ++p)
        if (*p <= 0x20U || *p == '"' || *p == '\\')
            return 0;
    return 1;
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
    char location[sizeof(state->config.home_location) * 2U];
    char json[sizeof(prompt) + 512U];
    int length;

    if (escape_json(prompt, sizeof(prompt), state->config.prompt) < 0 ||
        escape_json(model, sizeof(model), state->config.model) < 0 ||
        escape_json(location, sizeof(location),
                    state->config.home_location) < 0)
        return -1;
    length = snprintf(
        json, sizeof(json),
        "{\"version\":1,\"enabled\":%s,"
        "\"provider\":\"%s\",\"model\":\"%s\","
        "\"prompt\":\"%s\","
        "\"home_location\":\"%s\","
        "\"latitude\":\"%s\",\"longitude\":\"%s\","
        "\"weather_provider\":\"%s\"}\n",
        state->config.enabled ? "true" : "false", state->config.provider,
        model, prompt, location, state->config.latitude,
        state->config.longitude, state->config.weather_provider);
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
    (void)json_get_string(json, "provider", state->config.provider,
                          sizeof(state->config.provider));
    if (json_get_bool(json, "enabled", &enabled) > 0)
        state->config.enabled = enabled;
    (void)json_get_string(json, "model", state->config.model,
                          sizeof(state->config.model));
    (void)json_get_string(json, "prompt", state->config.prompt,
                          sizeof(state->config.prompt));
    (void)json_get_string(json, "home_location", state->config.home_location,
                          sizeof(state->config.home_location));
    (void)json_get_string(json, "latitude", state->config.latitude,
                          sizeof(state->config.latitude));
    (void)json_get_string(json, "longitude", state->config.longitude,
                          sizeof(state->config.longitude));
    (void)json_get_string(json, "weather_provider",
                          state->config.weather_provider,
                          sizeof(state->config.weather_provider));
    if (!state->config.weather_provider[0])
        strcpy(state->config.weather_provider, "open-meteo");
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
    char base_url[sizeof(state->credentials.base_url) * 2U];
    char location[sizeof(state->config.home_location) * 2U];
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
        escape_json(base_url, sizeof(base_url), state->credentials.base_url) < 0 ||
        escape_json(code, sizeof(code), state->auth.user_code) < 0 ||
        escape_json(url, sizeof(url), state->auth.verification_url) < 0 ||
        escape_json(error, sizeof(error), state->auth_error) < 0 ||
        escape_json(location, sizeof(location),
                    state->config.home_location) < 0 ||
        snprintf(
            payload, sizeof(payload),
            "{\"ready\":true,\"enabled\":%s,"
            "\"provider\":\"%s\",\"provider_name\":\"%s\","
            "\"subscription_auth\":%s,\"authenticated\":%s,"
            "\"auth_state\":\"%s\",\"user_code\":\"%s\","
            "\"verification_url\":\"%s\",\"auth_error\":\"%s\","
            "\"model\":\"%s\",\"prompt\":\"%s\","
            "\"home_location\":\"%s\","
            "\"latitude\":\"%s\",\"longitude\":\"%s\","
            "\"weather_provider\":\"%s\","
            "\"base_url\":\"%s\",\"api_key_configured\":%s,"
            "\"voice_pipeline\":true,\"text_streaming\":true,"
            "\"wake_connected\":%s,\"audio_connected\":%s,"
            "\"recognizing\":%s,\"dispatching\":%s,"
            "\"follow_up_pending\":%s,"
            "\"wake_events\":%lu,\"follow_up_listens\":%lu,"
            "\"completed_transcripts\":%lu,"
            "\"dropped_voice_turns\":%lu,"
            "\"last_stt_audio_ms\":%llu,"
            "\"last_stt_processing_ms\":%llu,"
            "\"last_stt_total_ms\":%llu,"
            "\"latency_target_ms\":3000,\"completed_turns\":%lu,"
            "\"last_first_text_ms\":%llu,"
            "\"last_first_announce_dispatch_ms\":%llu,"
            "\"last_speech_end_to_first_pcm_ms\":%llu,"
            "\"latency_target_met\":%s,"
            "\"latency_violations\":%lu}",
            state->config.enabled ? "true" : "false",
            state->provider->id, state->provider->name,
            state->provider->subscription_auth ? "true" : "false",
            state->auth_state == AUTH_SIGNED_IN ? "true" : "false",
            auth_state_name(state->auth_state), code, url, error,
            model, prompt, location, state->config.latitude,
            state->config.longitude, state->config.weather_provider,
            base_url,
            state->credentials.api_key[0] ? "true" : "false",
            voice_metrics.wake_connected ? "true" : "false",
            voice_metrics.audio_connected ? "true" : "false",
            voice_metrics.recognizing ? "true" : "false",
            voice_metrics.dispatching ? "true" : "false",
            voice_metrics.follow_up_pending ? "true" : "false",
            voice_metrics.wake_events,
            voice_metrics.follow_up_listens,
            voice_metrics.completed_transcripts,
            voice_metrics.dropped_turns,
            (unsigned long long)voice_metrics.last_stt_audio_ms,
            (unsigned long long)voice_metrics.last_stt_processing_ms,
            (unsigned long long)voice_metrics.last_stt_total_ms,
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

    if (!state->provider->subscription_auth)
        return 0;
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
    char tts_text[LE_LLM_TEXT_MAX];
    size_t used;
    size_t tts_used;
    int aggregate_tts;
    int complete;
};

static int enqueue_segment(void *context, const char *text)
{
    struct response_stream *stream = context;

    if (stream->aggregate_tts) {
        size_t length = strlen(text);
        if (stream->tts_used + length + 1 >= sizeof(stream->tts_text))
            return -1;
        if (stream->tts_used)
            stream->tts_text[stream->tts_used++] = ' ';
        memcpy(stream->tts_text + stream->tts_used, text, length + 1);
        stream->tts_used += length;
        return 0;
    }

    return le_voice_playback_enqueue(
        &stream->state->playback, text);
}

static int flush_aggregated_tts(struct response_stream *stream)
{
    if (!stream->aggregate_tts || !stream->tts_used)
        return 0;
    stream->tts_text[stream->tts_used] = '\0';
    return le_voice_playback_enqueue(&stream->state->playback,
                                     stream->tts_text);
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

/* --------------------------- Timer requests ----------------------------- */

/*
 * Timers are handled here rather than by the language model, which can
 * describe a timer but cannot start one. Matching locally also means the
 * common case answers in microseconds instead of a network round trip, and
 * keeps working with no internet at all.
 *
 * Returns 1 when the request was handled and nothing else should run, 0 to
 * pass the transcript on to the model unchanged.
 */
static int handle_timer_intent(struct agent_state *state,
                               const char *transcript, char *spoken,
                               size_t spoken_size)
{
    struct le_timer_intent intent;
    char response[LE_ADAPTER_MSG_MAX];
    char args[256];
    char speech[LE_TIMER_INTENT_SPEECH_MAX];
    int ringing = 0;
    int count = 0;
    int remaining = 0;
    int timer_id = 0;

    if (le_timer_intent_parse(transcript, &intent) == LE_TIMER_INTENT_NONE)
        return 0;

    if (adapter_call(state->timer_socket, 1000, "status", "{}", response,
                     sizeof(response)) != LE_ADAPTER_OK) {
        /* No timer daemon: say nothing about timers and let the model answer
           as it did before this existed. */
        return 0;
    }
    (void)json_get_int(response, "ringing", &ringing);

    switch (intent.kind) {
    case LE_TIMER_INTENT_DISMISS:
        /* "Stop" means a dozen things. It only means the timer while one is
           actually ringing; otherwise this is not our request to answer. */
        if (ringing <= 0)
            return 0;
        (void)adapter_call(state->timer_socket, 1000, "dismiss", "{}",
                           response, sizeof(response));
        /* Silence is the confirmation. */
        spoken[0] = '\0';
        return 1;

    case LE_TIMER_INTENT_SET:
        if (intent.seconds <= 0)
            break;
        {
            char label[LE_TIMER_INTENT_LABEL_MAX * 2];

            json_escape(label, sizeof(label), intent.label);
            snprintf(args, sizeof(args),
                     "{\"seconds\":%lld,\"label\":\"%s\"}",
                     intent.seconds, label);
        }
        if (adapter_call(state->timer_socket, 1000, "add", args, response,
                         sizeof(response)) != LE_ADAPTER_OK) {
            snprintf(speech, sizeof(speech),
                     "I could not set that timer.");
            snprintf(spoken, spoken_size, "%s", speech);
            (void)play_sentence(state, speech);
            return 1;
        }
        break;

    case LE_TIMER_INTENT_CANCEL:
        if (intent.cancel_all) {
            if (adapter_call(state->timer_socket, 1000, "cancel_all", "{}",
                             response, sizeof(response)) != LE_ADAPTER_OK)
                return 0;
            (void)json_get_int(response, "cancelled", &count);
            break;
        }
        if (intent.label[0]) {
            char label[LE_TIMER_INTENT_LABEL_MAX * 2];

            json_escape(label, sizeof(label), intent.label);
            snprintf(args, sizeof(args), "{\"label\":\"%s\"}", label);
            if (adapter_call(state->timer_socket, 1000, "cancel", args,
                             response, sizeof(response)) != LE_ADAPTER_OK) {
                snprintf(spoken, spoken_size,
                         "I could not find that timer.");
                (void)play_sentence(state, spoken);
                return 1;
            }
            count = 1;
            break;
        }
        if (adapter_call(state->timer_socket, 1000, "next", "{}", response,
                         sizeof(response)) != LE_ADAPTER_OK)
            return 0;
        (void)json_get_int(response, "count", &count);
        if (count > 1) {
            count = -1;
            break;
        }
        if (count == 1) {
            if (json_get_int(response, "id", &timer_id) != 1)
                return 0;
            snprintf(args, sizeof(args), "{\"id\":%d}", timer_id);
            if (adapter_call(state->timer_socket, 1000, "cancel", args,
                             response, sizeof(response)) != LE_ADAPTER_OK)
                return 0;
        }
        break;

    case LE_TIMER_INTENT_QUERY:
        if (adapter_call(state->timer_socket, 1000, "next", "{}", response,
                         sizeof(response)) != LE_ADAPTER_OK)
            return 0;
        (void)json_get_int(response, "count", &count);
        (void)json_get_int(response, "seconds_remaining", &remaining);
        break;

    default:
        return 0;
    }

    le_timer_intent_speech(&intent, count, remaining, speech, sizeof(speech));
    snprintf(spoken, spoken_size, "%s", speech);
    if (speech[0])
        (void)play_sentence(state, speech);
    return 1;
}

static int generate_response(struct agent_state *state,
                             const char *transcript,
                             uint64_t latency_start_ms,
                             char *full_text, size_t full_text_size,
                             char *error, size_t error_size)
{
    char turn_prompt[sizeof(state->config.prompt) + 512U];
    struct le_llm_http_request request;
    struct le_llm_http_response response;
    struct response_stream stream;
    if (state->auth_state != AUTH_SIGNED_IN) {
        snprintf(error, error_size, "%s",
                 "LLM provider is not configured");
        return -1;
    }
    if (le_voice_playback_begin_turn(&state->playback) < 0) {
        snprintf(error, error_size, "%s",
                 "a voice response is already playing");
        return -1;
    }
    if (refresh_credentials(state, 0) < 0) {
        state->auth_state = AUTH_ERROR;
        strcpy(state->auth_error, "LLM credentials refresh failed");
        snprintf(error, error_size, "%s", state->auth_error);
        return -1;
    }
    memset(&stream, 0, sizeof(stream));
    stream.state = state;
    stream.aggregate_tts = getenv("LE_AGENT_TTS_AGGREGATE") &&
                           !strcmp(getenv("LE_AGENT_TTS_AGGREGATE"), "1");
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
    build_turn_prompt(state, turn_prompt, sizeof(turn_prompt));
    if (state->provider->response_request(
            &state->credentials, state->config.model,
            turn_prompt, transcript, &request) < 0 ||
        le_llm_http_execute(
            state->curl_path, &request, response_event,
            &stream, &response) < 0 ||
        response.status != 200) {
        if (response.status == 401 &&
            refresh_credentials(state, 1) == 0 &&
            state->provider->response_request(
                &state->credentials, state->config.model,
                turn_prompt, transcript, &request) == 0) {
            memset(&stream, 0, sizeof(stream));
            stream.state = state;
            stream.aggregate_tts = getenv("LE_AGENT_TTS_AGGREGATE") &&
                                   !strcmp(getenv("LE_AGENT_TTS_AGGREGATE"), "1");
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
    if (flush_aggregated_tts(&stream) < 0) {
        snprintf(error, error_size, "%s", "reply playback queue is full");
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

/* ------------------------- Situational context -------------------------- */

/*
 * The assistant knows nothing about where or when it is, so it correctly
 * refuses location-dependent questions: asked for the weather it answers
 * that it cannot check without a weather location service.  Give it the
 * two facts it is missing -- the configured place and the current
 * conditions there -- as a short line appended to the system prompt.
 *
 * open-meteo is used because it needs no account and no API key, so the
 * device does not have to hold a credential tied to an identity in order
 * to answer "what is it like outside".  Only coarse coordinates leave the
 * device, and only when a location has actually been configured: with no
 * location set nothing is sent and the prompt is unchanged.
 */
#define WEATHER_REFRESH_SECONDS 600

/*
 * The shared JSON helpers expose int, bool and string but not a real
 * number, and these readings are fractional and nested under "current".
 * Scan for the key and parse the literal that follows it.
 */
static int json_number(const char *json, const char *key, double *out)
{
    char needle[64];
    const char *at;
    char *end;
    double value;

    if (!json || !key || !out ||
        (size_t)snprintf(needle, sizeof(needle), "\"%s\"", key) >=
            sizeof(needle))
        return 0;
    /*
     * Every key also appears in the response's "current_units" object with
     * a string value such as "\u00b0F", and that copy comes first.  Scope the
     * search to the readings, and skip any match whose value is a string
     * rather than a number, so a units block can never be parsed as one.
     */
    at = strstr(json, "\"current\":{");
    if (at)
        json = at;
    for (at = strstr(json, needle); at; at = strstr(at + 1, needle)) {
        const char *p = at + strlen(needle);

        while (*p == ' ' || *p == ':' || *p == '\t')
            ++p;
        if (*p == '"')
            continue;
        errno = 0;
        value = strtod(p, &end);
        if (errno || end == p)
            continue;
        *out = value;
        return 1;
    }
    return 0;
}

static const char *weather_description(int code)
{
    /* WMO codes, collapsed to what is worth saying out loud. */
    if (code == 0) return "clear";
    if (code <= 2) return "partly cloudy";
    if (code == 3) return "overcast";
    if (code <= 48) return "foggy";
    if (code <= 57) return "drizzling";
    if (code <= 67) return "raining";
    if (code <= 77) return "snowing";
    if (code <= 82) return "rain showers";
    if (code <= 86) return "snow showers";
    return "thunderstorms";
}

/* MET Norway reports Celsius and metres per second. */
static int fetch_met_no(struct agent_state *state, char *out, size_t size)
{
    /*
     * These two structures carry a 16 KiB body each.  Declaring them on the
     * stack costs about 32 KiB per call, and this runs on a pthread whose
     * default stack under musl is 128 KiB rather than the 8 MiB a glibc
     * main thread would give it.  That overflowed the thread stack and took
     * agentd down mid-turn: the fetch logged its reading and the process
     * died before the reply was generated.  Keep them on the heap.
     */
    struct le_llm_http_request *request;
    struct le_llm_http_response *response;
    int outcome = 0;
    char symbol[64] = "";
    double celsius = 0.0, mps = 0.0;
    size_t i;

    request = calloc(1, sizeof(*request));
    response = calloc(1, sizeof(*response));
    if (!request || !response)
        goto done;
    (void)snprintf(request->method, sizeof(request->method), "GET");
    if (snprintf(request->url, sizeof(request->url),
                 "https://api.met.no/weatherapi/locationforecast/2.0/compact"
                 "?lat=%s&lon=%s",
                 state->config.latitude,
                 state->config.longitude) >= (int)sizeof(request->url))
        goto done;
    if (le_llm_http_execute(state->curl_path, request, NULL, NULL,
                            response) < 0 || response->status != 200)
        goto done;
    if (!json_number(response->body, "air_temperature", &celsius))
        goto done;
    (void)json_number(response->body, "wind_speed", &mps);
    (void)json_get_string(response->body, "symbol_code", symbol,
                          sizeof(symbol));
    /* "partlycloudy_day" reads badly aloud; make it speakable. */
    for (i = 0; symbol[i]; ++i) {
        if (symbol[i] == '_')
            symbol[i] = ' ';
    }
    (void)snprintf(out, size,
                   "%d degrees Fahrenheit%s%s, wind %d miles per hour",
                   (int)(celsius * 9.0 / 5.0 + 32.0 + 0.5),
                   symbol[0] ? " and " : "", symbol,
                   (int)(mps * 2.23694 + 0.5));
    outcome = 1;
done:
    free(request);
    free(response);
    return outcome;
}

static int fetch_open_meteo(struct agent_state *state, char *out, size_t size)
{
    /*
     * These two structures carry a 16 KiB body each.  Declaring them on the
     * stack costs about 32 KiB per call, and this runs on a pthread whose
     * default stack under musl is 128 KiB rather than the 8 MiB a glibc
     * main thread would give it.  That overflowed the thread stack and took
     * agentd down mid-turn: the fetch logged its reading and the process
     * died before the reply was generated.  Keep them on the heap.
     */
    struct le_llm_http_request *request;
    struct le_llm_http_response *response;
    int outcome = 0;
    double temperature = 0.0, wind = 0.0, coded = 0.0;

    request = calloc(1, sizeof(*request));
    response = calloc(1, sizeof(*response));
    if (!request || !response)
        goto done;
    (void)snprintf(request->method, sizeof(request->method), "GET");
    if (snprintf(request->url, sizeof(request->url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%s&longitude=%s"
                 "&current=temperature_2m,weather_code,wind_speed_10m"
                 "&temperature_unit=fahrenheit&wind_speed_unit=mph",
                 state->config.latitude,
                 state->config.longitude) >= (int)sizeof(request->url))
        goto done;
    if (le_llm_http_execute(state->curl_path, request, NULL, NULL,
                            response) < 0 || response->status != 200)
        goto done;
    if (!json_number(response->body, "temperature_2m", &temperature) ||
        !json_number(response->body, "weather_code", &coded))
        goto done;
    (void)json_number(response->body, "wind_speed_10m", &wind);
    (void)snprintf(out, size,
                   "%d degrees Fahrenheit and %s, wind %d miles per hour",
                   (int)(temperature + (temperature < 0 ? -0.5 : 0.5)),
                   weather_description((int)coded),
                   (int)(wind + 0.5));
    outcome = 1;
done:
    free(request);
    free(response);
    return outcome;
}

static void refresh_weather(struct agent_state *state)
{
    char reading[sizeof(state->weather_text)];
    time_t now = time(NULL);
    int ok;

    if (!strcmp(state->config.weather_provider, "off"))
        return;
    if (!state->config.latitude[0] || !state->config.longitude[0])
        return;
    if (state->weather_text[0] &&
        now - state->weather_fetched < WEATHER_REFRESH_SECONDS)
        return;
    reading[0] = '\0';
    ok = !strcmp(state->config.weather_provider, "met-no")
        ? fetch_met_no(state, reading, sizeof(reading))
        : fetch_open_meteo(state, reading, sizeof(reading));
    if (!ok) {
        le_log_warn("agentd: %s weather lookup failed",
                    state->config.weather_provider);
        /*
         * Keep whatever was last known rather than clearing it.  A stale
         * reading a few minutes old answers the question far better than
         * refusing it, and the retry is only ten minutes away.
         */
        state->weather_fetched = now;
        return;
    }
    (void)snprintf(state->weather_text, sizeof(state->weather_text),
                   "%s", reading);
    state->weather_fetched = now;
    le_log_info("agentd: weather via %s for %s: %s",
                state->config.weather_provider,
                state->config.home_location[0]
                    ? state->config.home_location : "the configured location",
                state->weather_text);
}

/*
 * Build the prompt actually sent for this turn: the operator's prompt plus
 * whatever situational facts are known.  The base prompt is left untouched
 * in the config so this never accumulates across turns.
 */
static void build_turn_prompt(struct agent_state *state, char *out,
                              size_t size)
{
    char when[64] = "";
    time_t now = time(NULL);
    struct tm tm;

    (void)snprintf(out, size, "%s", state->config.prompt);
    if (localtime_r(&now, &tm))
        (void)strftime(when, sizeof(when), "%A %e %B, %H:%M", &tm);
    if (when[0])
        (void)snprintf(out + strlen(out), size - strlen(out),
                       " The current local date and time is %s.", when);
    if (state->config.home_location[0])
        (void)snprintf(out + strlen(out), size - strlen(out),
                       " The device is located in %s.",
                       state->config.home_location);
    refresh_weather(state);
    if (state->weather_text[0])
        (void)snprintf(out + strlen(out), size - strlen(out),
                       " The current weather there is %s. Answer weather "
                       "questions from this reading rather than declining.",
                       state->weather_text);
}

static int response_asks_question(const char *text)
{
    size_t length;

    if (!text)
        return 0;
    length = strlen(text);
    while (length &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r' || text[length - 1] == '\n' ||
            text[length - 1] == '"' || text[length - 1] == '\'' ||
            text[length - 1] == ')' || text[length - 1] == ']' ||
            text[length - 1] == '}'))
        --length;
    return length && text[length - 1] == '?';
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
    /* Timers are answered here rather than by the model, which cannot start
       one. Anything not a timer request falls straight through. */
    if (handle_timer_intent(state, transcript, full_text,
                            sizeof(full_text))) {
        if (escape_json(escaped, sizeof(escaped), full_text) < 0)
            return respond(fd, id, 0, "response text is too large");
        length = snprintf(payload, sizeof(payload),
                          "{\"queued\":true,\"text\":\"%s\","
                          "\"first_text_ms\":0}", escaped);
        return length > 0 && length < (int)sizeof(payload)
            ? respond(fd, id, 1, payload)
            : respond(fd, id, 0, "response text is too large");
    }
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
    char input[LE_LLM_TEXT_MAX];
    char reply[LE_LLM_TEXT_MAX];
    char error[256];
    int continuation;
    int generated = 0;

    pthread_mutex_lock(&state->control_mutex);
    /*
     * Timers are handled before the sign-in gate below, deliberately. They
     * need no language model, so a device with no LLM configured -- or no
     * internet -- can still set, cancel and silence one. Gating them behind
     * the model would make the one thing that works offline stop working.
     */
    if (handle_timer_intent(state, text, reply, sizeof(reply))) {
        state->follow_up_armed = 0;
        state->follow_up_depth = 0;
        pthread_mutex_unlock(&state->control_mutex);
        return;
    }
    if (state->config.enabled &&
        state->auth_state == AUTH_SIGNED_IN) {
        continuation = turn->follow_up && state->follow_up_armed;
        state->follow_up_armed = 0;
        if (!continuation) {
            state->follow_up_depth = 0;
            state->previous_voice_user[0] = '\0';
            state->previous_voice_reply[0] = '\0';
        }
        if (continuation &&
            snprintf(
                input, sizeof(input),
                "Previous user: %s\nPrevious assistant: %s\n"
                "User follow-up: %s",
                state->previous_voice_user,
                state->previous_voice_reply, text) >=
                (int)sizeof(input))
            continuation = 0;
        if (!continuation)
            snprintf(input, sizeof(input), "%s", text);
        le_log_info(
            "agentd: transcript follow_up=%s detection_sample=%llu "
            "audio_ms=%llu processing_ms=%llu total_ms=%llu text_chars=%lu",
            turn->follow_up ? "true" : "false",
            (unsigned long long)turn->detection_sample,
            (unsigned long long)turn->stt_audio_ms,
            (unsigned long long)turn->stt_processing_ms,
            (unsigned long long)turn->stt_total_ms,
            (unsigned long)strlen(text));
        if (generate_response(
                state, input,
                turn->endpoint && turn->transcript_received_ms >= 500
                    ? turn->transcript_received_ms - 500 : 0,
                reply, sizeof(reply),
                error, sizeof(error)) < 0)
            le_log_warn("agentd: voice response failed: %s", error);
        else
            generated = 1;
        if (generated) {
            snprintf(state->previous_voice_user,
                     sizeof(state->previous_voice_user), "%.*s",
                     (int)sizeof(state->previous_voice_user) - 1, text);
            snprintf(state->previous_voice_reply,
                     sizeof(state->previous_voice_reply), "%.*s",
                     (int)sizeof(state->previous_voice_reply) - 1, reply);
            if (response_asks_question(reply) &&
                state->follow_up_depth < FOLLOW_UP_MAX_DEPTH &&
                le_voice_playback_wait_idle(
                    &state->playback,
                    FOLLOW_UP_PLAYBACK_WAIT_MS) == 0) {
                ++state->follow_up_depth;
                state->follow_up_armed = 1;
                if (le_voice_pipeline_request_follow_up(
                        state->voice_pipeline) < 0)
                    state->follow_up_armed = 0;
            } else {
                state->follow_up_armed = 0;
                state->follow_up_depth = 0;
            }
        }
    }
    pthread_mutex_unlock(&state->control_mutex);
}

static int command_auth_start(struct agent_state *state, int fd,
                              unsigned long id)
{
    struct le_llm_http_request request;
    struct le_llm_http_response response;

    if (!state->provider->subscription_auth ||
        !state->provider->auth_start_request)
        return respond(fd, id, 0, "provider does not use device login");

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

    if (!state->provider->subscription_auth ||
        !state->provider->auth_poll_request)
        return respond(fd, id, 0, "provider does not use device login");

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
    if (parsed < 0 || (parsed > 0 &&
        strcmp(value, "openai-codex") && strcmp(value, "openai-compatible")))
        return respond(fd, id, 0, "unsupported provider");
    if (parsed > 0)
        strcpy(updated.provider, value);
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
    parsed = json_get_string(args, "home_location", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && strlen(value) >=
                       sizeof(updated.home_location)))
        return respond(fd, id, 0, "invalid home_location");
    if (parsed > 0)
        strcpy(updated.home_location, value);
    parsed = json_get_string(args, "latitude", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && !coordinate_valid(value, 90.0)))
        return respond(fd, id, 0, "latitude must be between -90 and 90");
    if (parsed > 0)
        strcpy(updated.latitude, value);
    parsed = json_get_string(args, "longitude", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && !coordinate_valid(value, 180.0)))
        return respond(fd, id, 0, "longitude must be between -180 and 180");
    if (parsed > 0)
        strcpy(updated.longitude, value);
    parsed = json_get_string(args, "weather_provider", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && strcmp(value, "open-meteo") &&
                       strcmp(value, "met-no") && strcmp(value, "off")))
        return respond(fd, id, 0,
                       "weather_provider must be open-meteo, met-no or off");
    if (parsed > 0)
        strcpy(updated.weather_provider, value);
    parsed = json_get_string(args, "base_url", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 &&
        (!endpoint_valid(value) || strlen(value) >= sizeof(state->credentials.base_url))))
        return respond(fd, id, 0, "invalid base_url");
    if (parsed > 0)
        strcpy(state->credentials.base_url, value);
    parsed = json_get_string(args, "api_key", value, sizeof(value));
    if (parsed < 0 || (parsed > 0 && strlen(value) >= sizeof(state->credentials.api_key)))
        return respond(fd, id, 0, "invalid api_key");
    if (parsed > 0) {
        memset(state->credentials.api_key, 0,
               sizeof(state->credentials.api_key));
        strcpy(state->credentials.api_key, value);
    }
    state->config = updated;
    state->provider = le_llm_provider_by_id(state->config.provider);
    if (!state->provider)
        return respond(fd, id, 0, "unsupported provider");
    if (save_config(state) < 0)
        return respond(fd, id, 0, "unable to save agent configuration");
    if (!strcmp(state->config.provider, "openai-compatible")) {
        if (state->credentials.base_url[0] &&
            le_llm_credentials_save(state->credentials_path,
                                     &state->credentials) < 0)
            return respond(fd, id, 0, "unable to save local LLM credentials");
        state->auth_state = state->credentials.base_url[0]
            ? AUTH_SIGNED_IN : AUTH_SIGNED_OUT;
    } else {
        state->auth_state = state->credentials.access_token[0]
            ? AUTH_SIGNED_IN : AUTH_SIGNED_OUT;
    }
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
    strcpy(state.timer_socket, DEFAULT_TIMER_SOCKET);
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
        } else if (!strcmp(argv[i], "--timer-socket") && value) {
            snprintf(state.timer_socket, sizeof(state.timer_socket),
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
                    "[--timer-socket PATH] "
                    "[--tts-first-pcm-file PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    le_log_init("agentd", argc, argv);
    poll_minimum = getenv("LE_AGENT_AUTH_POLL_MIN_SECONDS");
    if (poll_minimum) {
        char *end;
        unsigned long value = strtoul(poll_minimum, &end, 10);

        if (!*end && value <= 30)
            state.poll_minimum = (unsigned int)value;
    }
    load_config(&state);
    state.provider = le_llm_provider_by_id(state.config.provider);
    if (!state.provider)
        return 1;
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
        le_log_perr("agentd: listen");
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
    le_log_info("agentd: ready socket=%s provider=%s",
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
