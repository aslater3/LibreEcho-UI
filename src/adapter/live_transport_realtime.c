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
#include "live_session.h"
#include "live_transport.h"
#include "llm_store.h"
#include "ws_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

#define WS_DEFAULT_HOST "chatgpt.com"
#define WS_DEFAULT_PATH "/v1/realtime?intent=quicksilver"
#define WS_DEFAULT_MODEL "gpt-live-1-codex"
#define WS_DEFAULT_PORT 443
#define WS_CONNECT_TIMEOUT_MS 8000
#define WS_READ_TIMEOUT_MS 250

/* Audio the protocol carries, and the shape the device produces. */
#define WS_INPUT_SAMPLE_RATE 16000U
#define WS_OUTPUT_SAMPLE_RATE 24000U
#define WS_CONTEXT_CHUNK_MAX 500U

struct ws_state {
    int in_use;
    struct le_ws ws;
    struct le_ws_stream stream;
    int fd;
    int tls_active;
#if LE_TLS_AVAILABLE
    struct le_tls *tls;
#endif
    char host[128];
    char path[LE_WS_PATH_MAX];
    unsigned int port;
    char bearer[LE_LLM_TOKEN_MAX];
    char account_id[160];
    char model[64];
    char voice[32];
    int session_ready;
    int allow_unverified_tls;
    /* JSON scratch for one inbound message. */
    char message[LE_WS_MAX_PAYLOAD];
    unsigned int messages_in;
    unsigned int audio_chunks_out;
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
        return -2;                 /* want-read or a retryable condition */
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
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    char service[8];
    int rc;
    int last_error = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", s->port);
    rc = getaddrinfo(s->host, service, &hints, &list);
    if (rc || !list) {
        say(detail, detail_size,
            "GPT-Live unavailable: the server name could not be resolved.");
        return -1;
    }
    for (struct addrinfo *entry = list; entry; entry = entry->ai_next) {
        int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        int flags;

        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            s->fd = fd;
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd descriptor;
            int error = 0;
            socklen_t length = sizeof(error);

            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            if (poll(&descriptor, 1, WS_CONNECT_TIMEOUT_MS) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                error == 0) {
                s->fd = fd;
                break;
            }
            last_error = error ? error : ETIMEDOUT;
        } else {
            last_error = errno;
        }
        close(fd);
    }
    freeaddrinfo(list);
    if (s->fd < 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: the connection could not be established.");
        (void)last_error;
        return -1;
    }
    return 0;
}

/* --- outbound messages -------------------------------------------------- */

static int send_session_update(struct ws_state *s)
{
    char message[1024];
    int length = snprintf(
        message, sizeof(message),
        "{\"type\":\"session.update\",\"session\":{"
        "\"instructions\":\"%s\","
        "\"model\":\"%s\","
        "\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\","
        "\"rate\":%u},\"turn_detection\":{\"type\":\"server_vad\","
        "\"interrupt_response\":true}},\"output\":{\"voice\":\"%s\","
        "\"format\":{\"type\":\"audio/pcm\",\"rate\":%u}}},"
        "\"delegation\":{\"type\":\"client\"}}}",
        "You are the voice assistant built into this device. Answer briefly "
        "and naturally. When a request needs a device action, delegate it.",
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
    state.allow_unverified_tls =
        config && config->allow_unverified_tls;

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
        state.tls = le_tls_client_open(state.fd, state.host);
        if (!state.tls) {
            say(state.detail, sizeof(state.detail),
                "GPT-Live unavailable: the secure connection failed.");
            goto fail;
        }
        state.tls_active = 1;
        if (!le_tls_client_verified(state.tls) &&
            !state.allow_unverified_tls) {
            /*
             * No CA bundle ships on this image, so the certificate chain is
             * not checked. Sending an account token over an unauthenticated
             * channel is a real exposure, so this is refused unless an
             * operator has explicitly accepted it.
             */
            say(state.detail, sizeof(state.detail),
                "GPT-Live unavailable: the server certificate cannot be "
                "verified on this device.");
            goto fail;
        }
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
    char encoded[LE_WS_MAX_PAYLOAD / 2];
    char message[LE_WS_MAX_PAYLOAD];
    size_t encoded_length;
    int length;

    (void)transport;
    if (!state.session_ready || !samples || !count ||
        count > LE_LIVE_AUDIO_SAMPLES)
        return -1;
    encoded_length = le_b64_encode(samples, count * sizeof(int16_t), encoded,
                                   sizeof(encoded));
    if (!encoded_length)
        return -1;
    length = snprintf(message, sizeof(message),
                      "{\"type\":\"input_audio.append\",\"audio\":\"%s\"}",
                      encoded);
    if (length <= 0 || (size_t)length >= sizeof(message))
        return -1;
    if (le_ws_send_text(&state.ws, message, (size_t)length) < 0)
        return -1;
    ++state.audio_chunks_out;
    return 0;
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
        event->kind = LE_LIVE_EVENT_OPEN;
        return 1;
    }
    if (!strcmp(type, "output_audio.delta")) {
        if (top_string(message, "audio", encoded, sizeof(encoded)) < 0)
            return 0;
        decoded = le_b64_decode(encoded, event->samples,
                                sizeof(event->samples));
        if (!decoded)
            return 0;
        event->kind = LE_LIVE_EVENT_AUDIO;
        event->rate = WS_OUTPUT_SAMPLE_RATE;
        event->count = decoded / sizeof(int16_t);
        if (!event->count)
            return 0;
        /* The model finished this delta; a separate turn.done follows. */
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
        return 1;
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

        if (top_string(message, "message", message_text,
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
    result = le_ws_read_text(&state.ws, state.message, sizeof(state.message),
                             timeout_ms > 0 ? timeout_ms : WS_READ_TIMEOUT_MS);
    /*
     * The read outcome is the one thing that cannot be reconstructed after the
     * fact from a status snapshot, and it is what distinguishes "the server
     * went away" from "the framing is wrong".
     */
    fprintf(stderr, "lived: ws read=%d %s\n", result,
            result == 1 ? state.message : "");
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
    /*
     * Nothing to send. Barge-in is the server's job once turn_detection is
     * server_vad with interrupt_response, which the session request turns on;
     * the device's own part is to stop playing and keep feeding audio, and the
     * session does that already.
     */
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
    length = strlen(result);
    /*
     * The protocol chunks context appends at 500 bytes; a longer answer is
     * several events rather than one oversized frame.
     */
    do {
        size_t chunk = length - offset;
        char escaped[WS_CONTEXT_CHUNK_MAX * 2U + 16U];
        size_t written = 0;
        size_t i;
        int encoded;

        if (chunk > WS_CONTEXT_CHUNK_MAX)
            chunk = WS_CONTEXT_CHUNK_MAX;
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
    int written;

    (void)transport;
    if (!out || !size)
        return;
    written = snprintf(
        out, size,
        "{\"implemented\":true,\"transport\":\"websocket\","
        "\"host\":\"%s\",\"path\":\"%s\",\"model\":\"%s\","
        "\"session_ready\":%s,\"messages_in\":%u,\"audio_chunks_out\":%u,"
        "\"frames_in\":%llu,\"frames_out\":%llu,\"tls\":%s,"
        "\"verified_tls\":%s,\"detail\":\"%s\"}",
        state.host, state.path, state.model,
        state.session_ready ? "true" : "false", state.messages_in,
 state.audio_chunks_out, (unsigned long long)state.ws.frames_in,
 (unsigned long long)state.ws.frames_out,
 state.tls_active ? "true" : "false",
 /* Always false on this image: no CA bundle, so no peer can be
    authenticated. Reported rather than implied. */
 "false", state.detail);
    if (written < 0 || (size_t)written >= size)
        out[size - 1] = '\0';
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