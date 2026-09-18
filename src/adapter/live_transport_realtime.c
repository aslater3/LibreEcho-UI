#define _POSIX_C_SOURCE 200809L

/*
 * Subscription-backed GPT-Live transport: WebSocket + base64 PCM.
 *
 * This is the transport docs/GPT_LIVE_TRANSPORT.md specifies, extracted from
 * openai/codex (ThreadRealtimeStartTransport::Websocket, protocol_frameless_bidi):
 *
 *   wss://<host>/v1/realtime?intent=quicksilver&model=gpt-live-1-codex
 *
 * authenticated with the ChatGPT device OAuth the Responses path already uses.
 * Audio is base64 PCM in JSON text frames; delegation is client-managed.
 *
 * Single instance by design. lived owns one conversation at a time - the
 * session state machine refuses a second wake while one is active - so the
 * connection state, the credentials and the JSON scratch live in one
 * file-scope object rather than being copied into the transport's fixed state
 * area. A second start while one is live is refused rather than silently
 * sharing the connection.
 *
 * Every wait is bounded and the socket is non-blocking throughout, so nothing
 * here can hold the daemon's poll loop open.
 */

#include "live_b64.h"
#include "live_dns.h"
#include "live_session.h"
#include "live_transport.h"
#include "llm_store.h"
#include "ws_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../json.h"

#if LE_TLS_AVAILABLE
#include "../tls.h"
#endif

#define WS_DEFAULT_HOST "api.openai.com"
#define WS_DEFAULT_PATH "/v1/realtime"
#define WS_DEFAULT_MODEL "gpt-realtime"
#define WS_DEFAULT_PORT 443
#define WS_CONNECT_TIMEOUT_MS 8000
#define WS_READ_TIMEOUT_MS 250

/* Audio the protocol carries, and the shape the device produces. */
#define WS_SOURCE_SAMPLE_RATE 16000U
#define WS_INPUT_SAMPLE_RATE 24000U
#define WS_OUTPUT_SAMPLE_RATE 24000U
#define WS_CONTEXT_CHUNK_MAX 500U

struct ws_state {
    int in_use;
    struct le_ws ws;
    struct le_ws_stream stream;
    int fd;
    int tls_active;
    int last_tls_active;
    int last_tls_verified;
#if LE_TLS_AVAILABLE
    struct le_tls *tls;
#endif
    char host[128];
    char path[LE_WS_PATH_MAX];
    unsigned int port;
    char bearer[LE_LLM_TOKEN_MAX];
    char account_id[160];
    char model[128];
    char voice[32];
    char instructions[2048];
    char initial_text[256];
    int initial_sent;
    int ignore_next_response_done;
    int session_ready;
    char ca_path[256];
    /* JSON scratch for one inbound message. */
    char message[LE_WS_MAX_PAYLOAD];
    /* A server audio delta may be larger than one session event. Decode it
       once, then emit bounded LE_LIVE_AUDIO_SAMPLES chunks across polls. */
    int16_t audio_pending[(LE_WS_MAX_PAYLOAD * 3U / 4U) / 2U];
    size_t audio_pending_count;
    size_t audio_pending_offset;
    int output_done_pending;
    int suppress_audio;
    unsigned int messages_in;
    unsigned int audio_chunks_out;
    int16_t input_previous;
    int input_have_previous;
    unsigned int input_next_third;
    char detail[LE_LIVE_TEXT_MAX];
};

static struct ws_state state;

/*
 * Escape a string into a bounded JSON literal. The delegated request is model
 * output, so it can contain quotes and backslashes; interpolating it raw would
 * produce a malformed event and the delegation would be lost.
 */
static void json_escape_into(char *out, size_t size, const char *value)
{
    size_t used = 0;

    if (!out || !size)
        return;
    for (; value && *value; ++value) {
        unsigned char c = (unsigned char)*value;

        if (c == '"' || c == '\\') {
            if (used + 2U >= size)
                break;
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20U) {
            if (used + 6U >= size)
                break;
            used += (size_t)snprintf(out + used, size - used, "\\u%04x", c);
        } else {
            if (used + 1U >= size)
                break;
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
}

static void say(char *detail, size_t size, const char *message)
{
    if (detail && size)
        snprintf(detail, size, "%s", message);
}

/* --- top-level JSON extraction ------------------------------------------ */

/*
 * Find the value of `key` declared at the top level of `json`.
 *
 * A plain substring search is not enough here: `delegation.created` carries a
 * nested "type" and a nested "text", and picking the wrong one would turn a
 * tool request into a transcript line. So this walks the object tracking depth
 * and string state, and only matches a key at depth 1.
 *
 * Returns a pointer to the first character of the value, or NULL.
 */
static const char *top_value(const char *json, const char *key)
{
    size_t key_length;
    int depth = 0;
    const char *p = json;

    if (!json || !key)
        return NULL;
    key_length = strlen(key);
    while (*p) {
        if (*p == '"') {
            const char *start = ++p;
            size_t length = 0;

            while (*p && *p != '"') {
                if (*p == '\\' && p[1])
                    ++p;
                ++p;
                ++length;
            }
            if (!*p)
                return NULL;
            if (depth == 1 && length == key_length &&
                !strncmp(start, key, key_length)) {
                const char *colon = p + 1;

                while (*colon == ' ' || *colon == '\t' || *colon == '\n' ||
                       *colon == '\r')
                    ++colon;
                if (*colon == ':') {
                    ++colon;
                    while (*colon == ' ' || *colon == '\t' || *colon == '\n' ||
                           *colon == '\r')
                        ++colon;
                    return colon;
                }
            }
            ++p;
            continue;
        }
        if (*p == '{' || *p == '[')
            ++depth;
        else if (*p == '}' || *p == ']')
            --depth;
        ++p;
    }
    return NULL;
}

/* Unescape a JSON string value into `out`. Returns 0 on success. */
static int copy_string(const char *value, char *out, size_t size)
{
    size_t used = 0;

    if (!value || *value != '"')
        return -1;
    ++value;
    while (*value && *value != '"') {
        char c = *value++;

        if (c == '\\' && *value) {
            char escape = *value++;

            switch (escape) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                /* \uXXXX: keep the low byte, which is right for ASCII and
                   bounded for anything else. Audio and ids are ASCII. */
                if (strlen(value) < 4)
                    return -1;
                c = (char)strtoul(value + 2, NULL, 16);
                value += 4;
                break;
            default: c = escape; break;
            }
        }
        if (used + 1U >= size)
            return -1;
        out[used++] = c;
    }
    if (*value != '"')
        return -1;
    out[used] = '\0';
    return 0;
}

static int top_string(const char *json, const char *key, char *out,
                      size_t size)
{
    if (out && size)
        out[0] = '\0';
    return copy_string(top_value(json, key), out, size);
}

/* --- stream over a non-blocking socket (optionally TLS) ------------------ */

static long stream_recv(void *context, void *buffer, size_t length)
{
    struct ws_state *s = context;
    ssize_t got;

#if LE_TLS_AVAILABLE
    if (s->tls_active) {
        long count = le_tls_read_deadline(s->tls, buffer, length, 50);

        if (count > 0)
            return count;
        if (count == 0)
            return 0;
        return count == -2 ? -2 : -1;
    }
#endif
    got = recv(s->fd, buffer, length, 0);
    if (got > 0)
        return (long)got;
    if (got == 0)
        return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return -2;
    return -1;
}

static long stream_send(void *context, const void *buffer, size_t length)
{
    struct ws_state *s = context;
    ssize_t wrote;

#if LE_TLS_AVAILABLE
    if (s->tls_active) {
        long count = le_tls_write_deadline(s->tls, buffer, length, 50);

        if (count > 0)
            return count;
        return -2;
    }
#endif
    wrote = send(s->fd, buffer, length, MSG_NOSIGNAL);
    if (wrote > 0)
        return (long)wrote;
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR))
        return -2;
    return -1;
}

static void teardown(void)
{
    if (!state.in_use)
        return;
    le_ws_close(&state.ws);
#if LE_TLS_AVAILABLE
    if (state.tls) {
        le_tls_close(state.tls);
        state.tls = NULL;
    }
#endif
    if (state.fd >= 0) {
        close(state.fd);
        state.fd = -1;
    }
    memset(state.bearer, 0, sizeof(state.bearer));
    memset(state.account_id, 0, sizeof(state.account_id));
    state.tls_active = 0;
    state.session_ready = 0;
    state.in_use = 0;
}

/* --- URL and connection ------------------------------------------------- */

/*
 * Split a ws:// or wss:// URL into scheme, host, port and path.
 * Plain ws:// is accepted only for loopback, because the ChatGPT bearer is
 * about to be sent over it.
 */
static int parse_url(const char *url, int *secure, char *host, size_t host_size,
                     unsigned int *port, char *path, size_t path_size,
                     char *detail, size_t detail_size)
{
    const char *rest;
    const char *slash;
    const char *colon;
    size_t host_length;

    if (!url)
        return -1;
    if (!strncmp(url, "wss://", 6)) {
        *secure = 1;
        rest = url + 6;
        *port = 443;
    } else if (!strncmp(url, "ws://", 5)) {
        *secure = 0;
        rest = url + 5;
        *port = 80;
    } else {
        say(detail, detail_size,
            "GPT-Live unavailable: the configured address is not a WebSocket "
            "URL.");
        return -1;
    }
    slash = strchr(rest, '/');
    if (!slash) {
        say(detail, detail_size,
            "GPT-Live unavailable: the configured address has no path.");
        return -1;
    }
    colon = memchr(rest, ':', (size_t)(slash - rest));
    host_length = colon ? (size_t)(colon - rest) : (size_t)(slash - rest);
    if (!host_length || host_length >= host_size) {
        say(detail, detail_size,
            "GPT-Live unavailable: the configured host is not usable.");
        return -1;
    }
    memcpy(host, rest, host_length);
    host[host_length] = '\0';
    if (colon) {
        long value = strtol(colon + 1, NULL, 10);

        if (value <= 0 || value > 65535) {
            say(detail, detail_size,
                "GPT-Live unavailable: the configured port is not usable.");
            return -1;
        }
        *port = (unsigned int)value;
    }
    if (strlen(slash) >= path_size) {
        say(detail, detail_size,
            "GPT-Live unavailable: the configured path is too long.");
        return -1;
    }
    snprintf(path, path_size, "%s", slash);
    if (!*secure && strcmp(host, "127.0.0.1") && strcmp(host, "::1") &&
        strcmp(host, "localhost")) {
        say(detail, detail_size,
            "GPT-Live unavailable: refusing to send the account token over an "
            "unencrypted connection.");
        return -1;
    }
    return 0;
}

static int connect_socket(struct ws_state *s, char *detail, size_t detail_size)
{
    struct sockaddr_in endpoint;
    struct in_addr address;
    struct pollfd descriptor;
    int error = 0;
    socklen_t error_size = sizeof(error);

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons((uint16_t)s->port);
    if (inet_pton(AF_INET, s->host, &address) != 1 &&
        le_live_dns_resolve_ipv4(s->host, NULL, &address, 3000) < 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: the server name could not be resolved.");
        return -1;
    }
    endpoint.sin_addr = address;
    s->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s->fd < 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: the connection could not be created.");
        return -1;
    }
    if (connect(s->fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) == 0)
        return 0;
    if (errno != EINPROGRESS) {
        close(s->fd);
        s->fd = -1;
        say(detail, detail_size,
            "GPT-Live unavailable: the connection could not be established.");
        return -1;
    }
    descriptor.fd = s->fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, WS_CONNECT_TIMEOUT_MS) <= 0 ||
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 ||
        error != 0) {
        close(s->fd);
        s->fd = -1;
        say(detail, detail_size,
            "GPT-Live unavailable: the connection could not be established.");
        return -1;
    }
    return 0;
}

/* --- outbound messages -------------------------------------------------- */

static int send_session_update(struct ws_state *s)
{
    char message[8192];
    char instructions[sizeof(s->instructions) * 2U];

    json_escape_into(instructions, sizeof(instructions), s->instructions);
    int length = snprintf(
        message, sizeof(message),
        "{\"type\":\"session.update\",\"session\":{"
        "\"type\":\"realtime\",\"output_modalities\":[\"audio\"],"
        "\"instructions\":\"%s\","
        "\"model\":\"%s\","
        "\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\","
        "\"rate\":%u},\"noise_reduction\":{\"type\":\"near_field\"},"
        "\"turn_detection\":{\"type\":\"server_vad\","
        "\"interrupt_response\":true,\"create_response\":true,"
        "\"silence_duration_ms\":500}},\"output\":{\"voice\":\"%s\","
        "\"format\":{\"type\":\"audio/pcm\",\"rate\":%u}}},"
        "\"tools\":["
        "{\"type\":\"function\",\"name\":\"timer_set\",\"description\":\"Set a countdown timer on this LibreEcho device\",\"parameters\":{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400},\"label\":{\"type\":\"string\",\"maxLength\":63}},\"required\":[\"seconds\"],\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"timer_cancel\",\"description\":\"Cancel one timer by id, or all timers when id is omitted\",\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1}},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"timer_dismiss\",\"description\":\"Dismiss a ringing timer\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"timer_query\",\"description\":\"Read active timer status\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"media_stop\",\"description\":\"Stop music, radio, noise, or other media playback; never use this to end the voice conversation\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"media_status\",\"description\":\"Read the current playback source\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"device_time\",\"description\":\"Read the device local time and synchronization state\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"device_volume\",\"description\":\"Read current volume and mute state\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
        "{\"type\":\"function\",\"name\":\"session_stop\",\"description\":\"Immediately end this voice conversation after a wake-gated stop, cancel, never-mind, goodbye, or end-conversation request\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}"
        "]}}",
        instructions,
        s->model, WS_INPUT_SAMPLE_RATE, s->voice, WS_OUTPUT_SAMPLE_RATE);

    if (length <= 0 || (size_t)length >= sizeof(message)) {
        say(s->detail, sizeof(s->detail),
            "GPT-Live unavailable: the session request is too large.");
        return -1;
    }
    return le_ws_send_text(&s->ws, message, (size_t)length);
}

/* --- transport operations ----------------------------------------------- */

static int ws_start(struct le_live_transport *transport,
                    const struct le_live_transport_config *config,
                    char *detail, size_t detail_size)
{
    struct le_llm_credentials credentials;
    char headers[LE_LLM_TOKEN_MAX + 320];
    char url[320];
    int secure = 1;
    int rc;

    (void)transport;
    if (detail && detail_size)
        detail[0] = '\0';
    if (state.in_use) {
        say(detail, detail_size,
            "GPT-Live unavailable: a conversation is already open.");
        return -1;
    }
    memset(&state, 0, sizeof(state));
    /*
     * Explicit sentinels. After a memset these are 0, and 0 is a valid file
     * descriptor: teardown() would close stdin, and a later socket could land
     * on that number and be closed by a second teardown.
     */
    state.fd = -1;
    state.ws.urandom = -1;
    state.in_use = 1;
    snprintf(state.model, sizeof(state.model), "%s",
             config && config->model ? config->model : WS_DEFAULT_MODEL);
    snprintf(state.voice, sizeof(state.voice), "%s",
             config && config->voice ? config->voice : "cove");
    snprintf(state.instructions, sizeof(state.instructions), "%s",
             config && config->instructions ? config->instructions :
             "You are the voice assistant built into this device. Always answer in English. Answer briefly and naturally.");
    snprintf(state.initial_text, sizeof(state.initial_text), "%s",
             config && config->initial_text ? config->initial_text : "");
    snprintf(state.ca_path, sizeof(state.ca_path), "%s",
             config && config->ca_path ? config->ca_path
                                       : "/usr/local/share/libreecho/cacert.pem");

    memset(&credentials, 0, sizeof(credentials));
    if (!config || !config->credentials_path ||
        le_llm_credentials_load(config->credentials_path, &credentials) != 0 ||
        !credentials.access_token[0]) {
        le_llm_credentials_clear(&credentials);
        say(state.detail, sizeof(state.detail),
            "GPT-Live unavailable: sign in to ChatGPT again.");
        goto fail;
    }
    snprintf(state.bearer, sizeof(state.bearer), "%s", credentials.access_token);
    snprintf(state.account_id, sizeof(state.account_id), "%s",
             credentials.account_id);
    le_llm_credentials_clear(&credentials);
    if (!state.account_id[0]) {
        say(state.detail, sizeof(state.detail),
            "GPT-Live unavailable: the saved ChatGPT account is incomplete.");
        goto fail;
    }

    snprintf(url, sizeof(url), "%s",
             config->url && config->url[0]
                 ? config->url
                 : "wss://" WS_DEFAULT_HOST WS_DEFAULT_PATH);
    if (parse_url(url, &secure, state.host, sizeof(state.host), &state.port,
                  state.path, sizeof(state.path), state.detail,
                  sizeof(state.detail)) < 0)
        goto fail;
    if (strstr(state.path, "model=") == NULL) {
        size_t used = strlen(state.path);

        if (snprintf(state.path + used, sizeof(state.path) - used, "%cmodel=%s",
                     strchr(state.path, '?') ? '&' : '?',
                     state.model) >= (int)(sizeof(state.path) - used)) {
            say(state.detail, sizeof(state.detail),
                "GPT-Live unavailable: the configured path is too long.");
            goto fail;
        }
    }

    if (connect_socket(&state, state.detail, sizeof(state.detail)) < 0)
        goto fail;

#if LE_TLS_AVAILABLE
    if (secure) {
        state.tls = le_tls_client_open_verified(state.fd, state.host,
                                                state.ca_path);
        if (!state.tls || !le_tls_client_verified(state.tls)) {
            say(state.detail, sizeof(state.detail),
                "GPT-Live unavailable: the server certificate could not be "
                "verified.");
            goto fail;
        }
        state.tls_active = 1;
        state.last_tls_active = 1;
        state.last_tls_verified = 1;
    }
#else
    if (secure) {
        say(state.detail, sizeof(state.detail),
            "GPT-Live unavailable: this build has no TLS support.");
        goto fail;
    }
#endif

    /* Only the headers this protocol needs; the token never goes anywhere
       else, and it is wiped in teardown(). */
    if (snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\n"
                 "chatgpt-account-id: %s\r\n"
                 "originator: libreecho\r\n",
                 state.bearer, state.account_id) >= (int)sizeof(headers)) {
        say(state.detail, sizeof(state.detail),
            "GPT-Live unavailable: the account token is too large to send.");
        goto fail;
    }

    state.stream.context = &state;
    state.stream.recv = stream_recv;
    state.stream.send = stream_send;
    state.ws.send_timeout_ms = 5000;
    rc = le_ws_connect(&state.ws, &state.stream, state.host, state.path,
                       headers, WS_CONNECT_TIMEOUT_MS, state.detail,
                       sizeof(state.detail));
    memset(headers, 0, sizeof(headers));
    if (rc < 0)
        goto fail;
    if (send_session_update(&state) < 0)
        goto fail;

    if (detail && detail_size)
        detail[0] = '\0';
    return 0;

fail:
    if (detail && detail_size)
        snprintf(detail, detail_size, "%s", state.detail);
    teardown();
    return -1;
}

static int ws_send_audio(struct le_live_transport *transport,
                         const int16_t *samples, size_t count)
{
    int16_t resampled[(LE_LIVE_AUDIO_SAMPLES * 3U) / 2U + 2U];
    char encoded[LE_WS_MAX_PAYLOAD / 2];
    char message[LE_WS_MAX_PAYLOAD];
    size_t encoded_length;
    size_t input;
    size_t output = 0;
    int length;

    (void)transport;
    if (!state.session_ready || !samples || !count ||
        count > LE_LIVE_AUDIO_SAMPLES)
        return -1;
    /* Stateful linear 16 -> 24 kHz conversion. The next output position is
       represented in thirds of one input interval; advancing by two produces
       exactly three output samples per two input samples without resetting
       phase at frame boundaries. */
    for (input = 0; input < count; ++input) {
        int16_t current = samples[input];

        if (!state.input_have_previous) {
            state.input_previous = current;
            state.input_have_previous = 1;
            continue;
        }
        while (state.input_next_third < 3U) {
            int difference = (int)current - (int)state.input_previous;
            int value = (int)state.input_previous +
                difference * (int)state.input_next_third / 3;

            if (output >= sizeof(resampled) / sizeof(resampled[0]))
                return -1;
            resampled[output++] = (int16_t)value;
            state.input_next_third += 2U;
        }
        state.input_next_third -= 3U;
        state.input_previous = current;
    }
    if (!output)
        return 0;
    encoded_length = le_b64_encode(resampled, output * sizeof(int16_t), encoded,
                                   sizeof(encoded));
    if (!encoded_length)
        return -1;
    length = snprintf(message, sizeof(message),
                      "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                      encoded);
    if (length <= 0 || (size_t)length >= sizeof(message))
        return -1;
    if (le_ws_send_text(&state.ws, message, (size_t)length) < 0)
        return -1;
    ++state.audio_chunks_out;
    return 0;
}

static int send_initial_prompt(void)
{
    char escaped[sizeof(state.initial_text) * 2U];
    char message[sizeof(escaped) + 192U];
    int length;

    if (!state.initial_text[0] || state.initial_sent)
        return 0;
    json_escape_into(escaped, sizeof(escaped), state.initial_text);
    length = snprintf(
        message, sizeof(message),
        "{\"type\":\"conversation.item.create\",\"item\":{"
        "\"type\":\"message\",\"role\":\"user\",\"content\":[{"
        "\"type\":\"input_text\",\"text\":\"%s\"}]}}",
        escaped);
    if (length <= 0 || (size_t)length >= sizeof(message) ||
        le_ws_send_text(&state.ws, message, (size_t)length) < 0)
        return -1;
    if (le_ws_send_text(&state.ws, "{\"type\":\"response.create\"}",
                        sizeof("{\"type\":\"response.create\"}") - 1U) < 0)
        return -1;
    state.initial_sent = 1;
    return 0;
}

static const char *function_tool_name(const char *name)
{
    static const struct { const char *remote; const char *local; } tools[] = {
        {"timer_set", "timer.set"},
        {"timer_cancel", "timer.cancel"},
        {"timer_dismiss", "timer.dismiss"},
        {"timer_query", "timer.query"},
        {"media_stop", "media.stop"},
        {"media_status", "media.status"},
        {"device_time", "device.time"},
        {"device_volume", "device.volume"},
        {"session_stop", "session.stop"}
    };
    size_t i;

    for (i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i)
        if (!strcmp(name, tools[i].remote))
            return tools[i].local;
    return NULL;
}

/* Map one inbound frameless-bidi message onto a session event. */
static int translate(const char *message, struct le_live_event *event)
{
    char type[64];
    char text[LE_LIVE_TEXT_MAX];
    char encoded[LE_WS_MAX_PAYLOAD / 2];
    size_t decoded;

    if (top_string(message, "type", type, sizeof(type)) < 0)
        return 0;

    if (!strcmp(type, "session.started") || !strcmp(type, "session.updated")) {
        state.session_ready = 1;
        if (send_initial_prompt() < 0)
            return 0;
        event->kind = LE_LIVE_EVENT_OPEN;
        return 1;
    }
    if (!strcmp(type, "input_audio_buffer.speech_started")) {
        event->kind = LE_LIVE_EVENT_INPUT_STARTED;
        return 1;
    }
    if (!strcmp(type, "output_audio.delta") ||
        !strcmp(type, "response.output_audio.delta") ||
        !strcmp(type, "response.audio.delta")) {
        if (state.suppress_audio)
            return 0;
        if (top_string(message, "audio", encoded, sizeof(encoded)) < 0 &&
            top_string(message, "delta", encoded, sizeof(encoded)) < 0)
            return 0;
        decoded = le_b64_decode(encoded, state.audio_pending,
                                sizeof(state.audio_pending));
        if (!decoded || decoded % sizeof(int16_t))
            return 0;
        state.audio_pending_count = decoded / sizeof(int16_t);
        state.audio_pending_offset = 0;
        /* Emit the first bounded chunk now; later polls drain the remainder. */
        event->kind = LE_LIVE_EVENT_AUDIO;
        event->rate = WS_OUTPUT_SAMPLE_RATE;
        event->count = state.audio_pending_count;
        if (event->count > LE_LIVE_AUDIO_SAMPLES)
            event->count = LE_LIVE_AUDIO_SAMPLES;
        memcpy(event->samples, state.audio_pending,
               event->count * sizeof(event->samples[0]));
        state.audio_pending_offset = event->count;
        return 1;
    }
    if (!strcmp(type, "input_transcript.added") ||
        !strcmp(type, "output_transcript.added")) {
        const char *item = top_value(message, "item");

        if (copy_string(top_value(item, "text"), text, sizeof(text)) < 0)
            return 0;
        event->kind = LE_LIVE_EVENT_TRANSCRIPT;
        event->speaker = type[0] == 'i' ? LE_LIVE_SPEAKER_USER
                                        : LE_LIVE_SPEAKER_MODEL;
        event->final = 0;
        snprintf(event->text, sizeof(event->text), "%s", text);
        return 1;
    }
    if (!strcmp(type, "turn.done")) {
        const char *turn = top_value(message, "turn");
        char role[24];
        char transcript[LE_LIVE_TEXT_MAX];

        if (top_string(turn, "role", role, sizeof(role)) < 0 ||
            top_string(turn, "transcript", transcript,
                       sizeof(transcript)) < 0)
            return 0;
        event->kind = LE_LIVE_EVENT_TRANSCRIPT;
        event->speaker = !strcmp(role, "user") ? LE_LIVE_SPEAKER_USER
                                               : LE_LIVE_SPEAKER_MODEL;
        event->final = 1;
        snprintf(event->text, sizeof(event->text), "%s", transcript);
        if (event->speaker == LE_LIVE_SPEAKER_USER)
            state.suppress_audio = 0;
        else if (!state.suppress_audio)
            state.output_done_pending = 1;
        return 1;
    }
    if (!strcmp(type, "response.function_call_arguments.done")) {
        char call_id[LE_LIVE_DELEGATION_ID_MAX - 3U];
        char function[64];
        char arguments[LE_LIVE_ARGUMENT_MAX];
        const char *tool;

        if (top_string(message, "call_id", call_id, sizeof(call_id)) < 0 ||
            top_string(message, "name", function, sizeof(function)) < 0 ||
            top_string(message, "arguments", arguments, sizeof(arguments)) < 0)
            return 0;
        tool = function_tool_name(function);
        if (!tool)
            return 0;
        event->kind = LE_LIVE_EVENT_DELEGATION;
        state.ignore_next_response_done = 1;
        snprintf(event->delegation_id, sizeof(event->delegation_id),
                 "fn:%s", call_id);
        snprintf(event->tool, sizeof(event->tool), "%s", tool);
        snprintf(event->arguments, sizeof(event->arguments), "%s", arguments);
        return 1;
    }
    if (!strcmp(type, "response.done")) {
        if (state.ignore_next_response_done) {
            state.ignore_next_response_done = 0;
            return 0;
        }
        if (!state.suppress_audio)
            state.output_done_pending = 1;
        return 0;
    }
    if (!strcmp(type, "delegation.created")) {
        const char *item = top_value(message, "item");
        char item_type[32];
        char target[32];
        char id[LE_LIVE_DELEGATION_ID_MAX];
        const char *content;
        const char *request;

        if (top_string(item, "type", item_type, sizeof(item_type)) < 0 ||
            strcmp(item_type, "delegation"))
            return 0;
        if (top_string(item, "target", target, sizeof(target)) < 0 ||
            strcmp(target, "client"))
            return 0;
        if (top_string(item, "id", id, sizeof(id)) < 0)
            return 0;
        content = top_value(item, "content");
        request = content ? strstr(content, "\"text\"") : NULL;
        if (!request)
            return 0;
        request += strlen("\"text\"");
        while (*request == ' ' || *request == ':')
            ++request;
        if (copy_string(request, text, sizeof(text)) < 0)
            return 0;
        event->kind = LE_LIVE_EVENT_DELEGATION;
        snprintf(event->delegation_id, sizeof(event->delegation_id), "%s", id);
        /*
         * The protocol hands over a natural-language request, not a tool name:
         * the model names an intent and the device decides what to do with it.
         * Routing that onto the local tool allow-list needs a request parser
         * this release does not have, so the request is carried as one
         * allow-listed action that answers honestly until it does. Everything
         * downstream - the allow-list, the argument validation, the
         * exactly-once cache - is exercised by the real path.
         */
        {
            char escaped[LE_LIVE_ARGUMENT_MAX / 2];

            json_escape_into(escaped, sizeof(escaped), text);
            snprintf(event->tool, sizeof(event->tool), "voice.request");
            snprintf(event->arguments, sizeof(event->arguments),
                     "{\"request\":\"%s\"}", escaped);
        }
        return 1;
    }
    if (!strcmp(type, "error")) {
        char message_text[LE_LIVE_TEXT_MAX];
        const char *error = top_value(message, "error");

        fprintf(stderr, "lived: realtime error frame=%s\n", message);
        if ((!error || top_string(error, "message", message_text,
                                  sizeof(message_text)) < 0) &&
            top_string(message, "message", message_text,
                       sizeof(message_text)) < 0)
            snprintf(message_text, sizeof(message_text),
                     "the model reported an error");
        event->kind = LE_LIVE_EVENT_ERROR;
        snprintf(event->detail, sizeof(event->detail), "%s", message_text);
        return 1;
    }
    /* Unknown types are ignored: a new server event must not break the
       device. */
    return 0;
}

static int ws_poll(struct le_live_transport *transport,
                   struct le_live_event *event, int timeout_ms)
{
    int result;

    (void)transport;
    if (!event || !state.ws.connected)
        return -1;
    memset(event, 0, sizeof(*event));
    if (state.audio_pending_offset < state.audio_pending_count) {
        size_t remaining = state.audio_pending_count - state.audio_pending_offset;

        event->kind = LE_LIVE_EVENT_AUDIO;
        event->rate = WS_OUTPUT_SAMPLE_RATE;
        event->count = remaining > LE_LIVE_AUDIO_SAMPLES
            ? LE_LIVE_AUDIO_SAMPLES : remaining;
        memcpy(event->samples,
               state.audio_pending + state.audio_pending_offset,
               event->count * sizeof(event->samples[0]));
        state.audio_pending_offset += event->count;
        return 1;
    }
    if (state.output_done_pending) {
        state.output_done_pending = 0;
        event->kind = LE_LIVE_EVENT_OUTPUT_DONE;
        return 1;
    }
    result = le_ws_read_text(&state.ws, state.message, sizeof(state.message),
                             timeout_ms > 0 ? timeout_ms : WS_READ_TIMEOUT_MS);
    if (result == 0)
        return 0;
    if (result == 2) {
        memset(event, 0, sizeof(*event));
        event->kind = LE_LIVE_EVENT_CLOSED;
        snprintf(event->detail, sizeof(event->detail),
                 "the session was closed by the server");
        return 1;
    }
    if (result < 0) {
        memset(event, 0, sizeof(*event));
        event->kind = LE_LIVE_EVENT_ERROR;
        snprintf(event->detail, sizeof(event->detail),
                 "the connection to GPT-Live was lost");
        return 1;
    }
    ++state.messages_in;
    memset(event, 0, sizeof(*event));
    if (!translate(state.message, event))
        return 0;
    return 1;
}

static int ws_interrupt(struct le_live_transport *transport)
{
    (void)transport;
    /* Server VAD stops generation. Locally discard all decoded audio from the
       interrupted assistant turn and refuse later deltas until the server
       completes the new user turn. */
    state.audio_pending_count = 0;
    state.audio_pending_offset = 0;
    state.output_done_pending = 0;
    state.suppress_audio = 1;
    return 0;
}

static int ws_complete_delegation(struct le_live_transport *transport,
                                  const char *delegation_id,
                                  const char *result)
{
    char message[LE_WS_MAX_PAYLOAD / 2];
    size_t offset = 0;
    size_t length;

    (void)transport;
    if (!delegation_id || !delegation_id[0] || !result || !state.ws.connected)
        return -1;
    if (!strncmp(delegation_id, "fn:", 3)) {
        char escaped[LE_LIVE_ARGUMENT_MAX * 2U];
        char item[sizeof(escaped) + LE_LIVE_DELEGATION_ID_MAX + 160U];
        int written;

        json_escape_into(escaped, sizeof(escaped), result);
        written = snprintf(
            item, sizeof(item),
            "{\"type\":\"conversation.item.create\",\"item\":{"
            "\"type\":\"function_call_output\",\"call_id\":\"%s\","
            "\"output\":\"%s\"}}",
            delegation_id + 3, escaped);
        if (written <= 0 || (size_t)written >= sizeof(item) ||
            le_ws_send_text(&state.ws, item, (size_t)written) < 0)
            return -1;
        if (le_ws_send_text(&state.ws, "{\"type\":\"response.create\"}",
                            sizeof("{\"type\":\"response.create\"}") - 1U) < 0)
            return -1;
        return 0;
    }
    length = strlen(result);
    /*
     * The protocol chunks context appends at 500 bytes; a longer answer is
     * several events rather than one oversized frame.
     */
    do {
        size_t chunk = length - offset;
        /* Worst case is one \u00xx escape (6 bytes) per input byte. */
        char escaped[WS_CONTEXT_CHUNK_MAX * 6U + 1U];
        size_t written = 0;
        size_t i;
        int encoded;

        if (chunk > WS_CONTEXT_CHUNK_MAX)
            chunk = WS_CONTEXT_CHUNK_MAX;
        /* Never split a UTF-8 code point between context events. */
        while (chunk > 0 && offset + chunk < length &&
               ((unsigned char)result[offset + chunk] & 0xc0U) == 0x80U)
            --chunk;
        if (!chunk)
            return -1;
        for (i = 0; i < chunk; ++i) {
            unsigned char c = (unsigned char)result[offset + i];

            if (c == '"' || c == '\\') {
                escaped[written++] = '\\';
                escaped[written++] = (char)c;
            } else if (c < 0x20U) {
                written += (size_t)snprintf(escaped + written,
                                            sizeof(escaped) - written,
                                            "\\u%04x", c);
            } else {
                escaped[written++] = (char)c;
            }
        }
        escaped[written] = '\0';
        encoded = snprintf(message, sizeof(message),
                           "{\"type\":\"delegation.context.append\","
                           "\"delegation_item_id\":\"%s\","
                           "\"content\":[{\"type\":\"input_text\","
                           "\"text\":\"%s\"}]}",
                           delegation_id, escaped);
        if (encoded <= 0 || (size_t)encoded >= sizeof(message))
            return -1;
        if (le_ws_send_text(&state.ws, message, (size_t)encoded) < 0)
            return -1;
        offset += chunk;
    } while (offset < length);
    return 0;
}

static void ws_stop(struct le_live_transport *transport)
{
    (void)transport;
    if (!state.in_use)
        return;
    if (state.ws.connected) {
        static const char close_message[] = "{\"type\":\"session.close\"}";

        (void)le_ws_send_text(&state.ws, close_message,
                              sizeof(close_message) - 1U);
        (void)le_ws_send_close(&state.ws, 1000);
        /* Give the close frame a moment to leave, then let go. */
        (void)le_ws_read_text(&state.ws, state.message, sizeof(state.message),
                              50);
    }
    teardown();
}

static void ws_metrics(const struct le_live_transport *transport, char *out,
                       size_t size)
{
    char host[sizeof(state.host) * 2U];
    char path[sizeof(state.path) * 2U];
    char model[sizeof(state.model) * 2U];
    char detail[sizeof(state.detail) * 2U];
    int written;

    (void)transport;
    if (!out || !size)
        return;
    json_escape_into(host, sizeof(host), state.host);
    json_escape_into(path, sizeof(path), state.path);
    json_escape_into(model, sizeof(model), state.model);
    json_escape_into(detail, sizeof(detail), state.detail);
    written = snprintf(
        out, size,
        "{\"implemented\":true,\"transport\":\"websocket\","
        "\"host\":\"%s\",\"path\":\"%s\",\"model\":\"%s\","
        "\"session_ready\":%s,\"messages_in\":%u,\"audio_chunks_out\":%u,"
        "\"frames_in\":%llu,\"frames_out\":%llu,\"tls\":%s,"
        "\"verified_tls\":%s,\"detail\":\"%s\"}",
        host, path, model,
        state.session_ready ? "true" : "false", state.messages_in,
        state.audio_chunks_out, (unsigned long long)state.ws.frames_in,
        (unsigned long long)state.ws.frames_out,
        state.last_tls_active ? "true" : "false",
        state.last_tls_verified ? "true" : "false", detail);
    if (written < 0 || (size_t)written >= size)
        snprintf(out, size,
                 "{\"implemented\":true,\"transport\":\"websocket\","
                 "\"detail\":\"status exceeds the adapter limit\"}");
}

static const struct le_live_transport_ops websocket_ops = {
    "realtime",
    ws_start,
    ws_send_audio,
    ws_poll,
    ws_interrupt,
    ws_complete_delegation,
    ws_stop,
    ws_metrics
};

const struct le_live_transport_ops *le_live_transport_realtime_ops(void)
{
    return &websocket_ops;
}