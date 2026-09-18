#define _POSIX_C_SOURCE 200809L

#include "live_tools.h"
#include "adapter.h"
#include "timer_intent.h"
#include "../json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMER_SOCKET "/run/libreecho/timer.sock"
#define DEFAULT_MEDIA_STATUS "/run/libreecho-audio/status.json"
#define DEFAULT_TIME_STATUS "/run/libreecho/time.status"
#define DEFAULT_TIMEOUT_MS 1500
#define MAX_TIMER_SECONDS 86400LL
#define MAX_URL_LENGTH 384

/*
 * Reject arguments that carry a filesystem path, a shell metacharacter or a
 * control character.  None of the 0.14 tools needs any of these, and a tool
 * that does will say so explicitly rather than loosening this check globally.
 */
static int text_is_plain(const char *value)
{
    const unsigned char *position = (const unsigned char *)value;

    for (; *position; ++position) {
        unsigned char c = *position;

        if (c < 0x20U || c == 0x7fU)
            return 0;
        if (c == '/' || c == '\\' || c == ';' || c == '|' || c == '&' ||
            c == '$' || c == '`' || c == '<' || c == '>' || c == '*' ||
            c == '?' || c == '~' || c == '{' || c == '}')
            return 0;
    }
    return 1;
}

/*
 * Bounded JSON string escaping.  The shared json_escape() returns void, and a
 * silently truncated tool argument would be forwarded to a daemon as if it
 * were complete, so this reports failure instead.
 */
static int escape_into(char *out, size_t size, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    if (!out || !size)
        return -1;
    for (; value && *value; ++value) {
        unsigned char c = (unsigned char)*value;

        if (c == '"' || c == '\\') {
            if (used + 2 >= size)
                return -1;
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20U || c == 0x7fU) {
            if (used + 6 >= size)
                return -1;
            out[used++] = '\\';
            out[used++] = 'u';
            out[used++] = '0';
            out[used++] = '0';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15U];
        } else {
            if (used + 1 >= size)
                return -1;
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    return 0;
}

static int respond_ok(char *result, size_t size, const char *fields)
{
    int written;

    if (!result || !size)
        return -1;
    written = snprintf(result, size, "{\"ok\":true%s%s}",
                       fields && fields[0] ? "," : "",
                       fields && fields[0] ? fields : "");
    if (written < 0 || (size_t)written >= size) {
        result[size - 1] = '\0';
        return -1;
    }
    return 0;
}

static int respond_error(char *result, size_t size, const char *message)
{
    int written;

    if (!result || !size)
        return -1;
    written = snprintf(result, size, "{\"ok\":false,\"error\":\"");
    if (written < 0 || (size_t)written >= size) {
        result[size - 1] = '\0';
        return -1;
    }
    if (escape_into(result + written, size - (size_t)written, message) < 0) {
        result[size - 1] = '\0';
        return -1;
    }
    written = (int)strlen(result);
    if ((size_t)written + 3 > size) {
        result[size - 1] = '\0';
        return -1;
    }
    memcpy(result + written, "\"}", 3);
    return -1;
}

static int adapter_call(const struct le_live_tool_environment *environment,
                        const char *socket_path, const char *command,
                        const char *args, char *out, size_t out_size)
{
    struct le_adapter *adapter;
    int result;

    adapter = le_adapter_connect(socket_path, environment->timeout_ms);
    if (!adapter)
        return LE_ADAPTER_ERR_CONNECT;
    result = le_adapter_call(adapter, command, args, out, out_size);
    le_adapter_close(adapter);
    return result;
}

static int file_head(const char *path, char *out, size_t size)
{
    int fd;
    ssize_t count;

    if (!out || !size)
        return -1;
    out[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    count = read(fd, out, size - 1);
    close(fd);
    if (count <= 0)
        return -1;
    out[count] = '\0';
    return (int)count;
}

/* --- handlers ----------------------------------------------------------- */

static int tool_timer_set(const struct le_live_tool_environment *environment,
                          const char *arguments, char *result, size_t size)
{
    long long seconds = 0;
    char label[64] = "";
    char escaped_label[128];
    char args[192];
    char response[LE_ADAPTER_MSG_MAX];
    char fields[160];

    if (json_get_int64(arguments, "seconds", &seconds) != 1)
        return respond_error(result, size, "A timer needs a duration.");
    /*
     * Refuse rather than clamp: a model that asked for a week should be told
     * no, not silently given something else it will then describe as done.
     */
    if (seconds < 1 || seconds > MAX_TIMER_SECONDS)
        return respond_error(result, size,
                             "The timer length must be between 1 second and "
                             "24 hours.");
    if (json_get_string(arguments, "label", label, sizeof(label)) > 0 &&
        !text_is_plain(label))
        return respond_error(result, size, "That timer label is not allowed.");
    if (escape_into(escaped_label, sizeof(escaped_label), label) < 0)
        return respond_error(result, size, "That timer label is too long.");
    if (snprintf(args, sizeof(args), "{\"seconds\":%lld,\"label\":\"%s\"}",
                 seconds, escaped_label) >= (int)sizeof(args))
        return respond_error(result, size, "That timer request is too long.");
    if (adapter_call(environment, environment->timer_socket, "add", args,
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return respond_error(result, size, "The timer could not be set.");
    if (snprintf(fields, sizeof(fields), "\"seconds\":%lld,\"label\":\"%s\"",
                 seconds, escaped_label) >= (int)sizeof(fields))
        return respond_error(result, size, "That timer request is too long.");
    return respond_ok(result, size, fields);
}

static int tool_timer_cancel(const struct le_live_tool_environment *environment,
                             const char *arguments, char *result, size_t size)
{
    unsigned int id = 0;
    char args[64];
    char response[LE_ADAPTER_MSG_MAX];
    const char *command = "cancel_all";

    if (json_get_uint(arguments, "id", &id) > 0) {
        command = "cancel";
        if (snprintf(args, sizeof(args), "{\"id\":%u}", id) >=
            (int)sizeof(args))
            return respond_error(result, size, "That timer id is invalid.");
    } else {
        snprintf(args, sizeof(args), "{}");
    }
    if (adapter_call(environment, environment->timer_socket, command, args,
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return respond_error(result, size, "No matching timer was cancelled.");
    return respond_ok(result, size, "\"cancelled\":true");
}

static int tool_timer_dismiss(const struct le_live_tool_environment *environment,
                              const char *arguments, char *result, size_t size)
{
    char response[LE_ADAPTER_MSG_MAX];

    (void)arguments;
    if (adapter_call(environment, environment->timer_socket, "dismiss", "{}",
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return respond_error(result, size, "Nothing is ringing.");
    return respond_ok(result, size, "\"dismissed\":true");
}

static int tool_timer_query(const struct le_live_tool_environment *environment,
                            const char *arguments, char *result, size_t size)
{
    char response[LE_ADAPTER_MSG_MAX];
    long long remaining = 0;
    int active = 0;

    (void)arguments;
    if (adapter_call(environment, environment->timer_socket, "status", "{}",
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return respond_error(result, size, "Timer status is unavailable.");
    /*
     * Only bounded, non-identifying facts are echoed back: how many timers are
     * pending and how long the next one has left.  Labels stay on the device.
     */
    (void)json_get_int(response, "pending", &active);
    (void)json_get_int64(response, "next_seconds", &remaining);
    {
        char fields[128];

        if (snprintf(fields, sizeof(fields),
                     "\"pending\":%d,\"next_seconds\":%lld", active,
                     remaining) >= (int)sizeof(fields))
            return respond_error(result, size, "Timer status is too large.");
        return respond_ok(result, size, fields);
    }
}

static int tool_media_stop(const struct le_live_tool_environment *environment,
                           const char *arguments, char *result, size_t size)
{
    char response[LE_ADAPTER_MSG_MAX];
    int stopped = 0;

    (void)arguments;
    if (adapter_call(environment, environment->radio_socket, "stop", NULL,
                     response, sizeof(response)) == LE_ADAPTER_OK)
        stopped = 1;
    if (adapter_call(environment, environment->audio_socket, "stop_speech",
                     NULL, response, sizeof(response)) == LE_ADAPTER_OK)
        stopped = 1;
    if (adapter_call(environment, environment->audio_socket, "noise_stop",
                     NULL, response, sizeof(response)) == LE_ADAPTER_OK)
        stopped = 1;
    if (!stopped)
        return respond_error(result, size, "Playback control is unavailable.");
    return respond_ok(result, size, "\"stopped\":true");
}

static int tool_media_status(const struct le_live_tool_environment *environment,
                             const char *arguments, char *result, size_t size)
{
    char media[768];
    char source[32] = "idle";
    char fields[128];

    (void)arguments;
    if (file_head(environment->media_status_path, media, sizeof(media)) > 0)
        (void)json_get_string_top_level(media, "active", source,
                                        sizeof(source));
    if (!source[0])
        snprintf(source, sizeof(source), "idle");
    if (snprintf(fields, sizeof(fields), "\"active\":\"%s\"", source) >=
        (int)sizeof(fields))
        return respond_error(result, size, "Media status is too large.");
    return respond_ok(result, size, fields);
}

static int tool_radio_play(const struct le_live_tool_environment *environment,
                           const char *arguments, char *result, size_t size)
{
    char url[MAX_URL_LENGTH];
    char args[MAX_URL_LENGTH + 32];
    char response[LE_ADAPTER_MSG_MAX];

    if (json_get_string(arguments, "url", url, sizeof(url)) < 1 || !url[0])
        return respond_error(result, size, "A station address is required.");
    /*
     * Defence in depth: radiod already insists on http/https or a bundled
     * local path, and that local-path form is exactly what a model must not be
     * able to name.
     */
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
        return respond_error(result, size,
                             "Only internet radio addresses may be played.");
    if (escape_into(args, sizeof(args), url) < 0)
        return respond_error(result, size, "That address is not usable.");
    {
        char body[MAX_URL_LENGTH + 32];

        if (snprintf(body, sizeof(body), "{\"url\":\"%s\"}", args) >=
            (int)sizeof(body))
            return respond_error(result, size, "That address is too long.");
        if (adapter_call(environment, environment->radio_socket, "play", body,
                         response, sizeof(response)) != LE_ADAPTER_OK)
            return respond_error(result, size, "Playback could not be started.");
    }
    return respond_ok(result, size, "\"playing\":true");
}

static int tool_device_time(const struct le_live_tool_environment *environment,
                            const char *arguments, char *result, size_t size)
{
    struct tm broken_down;
    time_t now = time(NULL);
    char stamp[64];
    char fields[160];
    char status[512] = "";
    int synchronized = 0;

    (void)arguments;
    if (!localtime_r(&now, &broken_down) ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z",
                 &broken_down) == 0)
        return respond_error(result, size, "The device clock is unavailable.");
    if (file_head(environment->time_status_path, status, sizeof(status)) > 0 &&
        strstr(status, "synchronized=1"))
        synchronized = 1;
    if (snprintf(fields, sizeof(fields), "\"time\":\"%s\",\"synchronized\":%s",
                 stamp, synchronized ? "true" : "false") >=
        (int)sizeof(fields))
        return respond_error(result, size, "The device clock is unavailable.");
    return respond_ok(result, size, fields);
}

static int tool_device_volume(const struct le_live_tool_environment *environment,
                              const char *arguments, char *result, size_t size)
{
    char response[LE_ADAPTER_MSG_MAX];
    int volume = -1;
    int muted = 0;
    char fields[96];

    (void)arguments;
    if (adapter_call(environment, environment->audio_socket, "status", NULL,
                     response, sizeof(response)) != LE_ADAPTER_OK)
        return respond_error(result, size, "Volume is unavailable.");
    (void)json_get_int(response, "volume", &volume);
    (void)json_get_bool(response, "muted", &muted);
    if (volume < 0)
        return respond_error(result, size, "Volume is unavailable.");
    if (snprintf(fields, sizeof(fields), "\"volume\":%d,\"muted\":%s", volume,
                 muted ? "true" : "false") >= (int)sizeof(fields))
        return respond_error(result, size, "Volume is unavailable.");
    return respond_ok(result, size, fields);
}

static int tool_device_weather(const struct le_live_tool_environment *environment,
                               const char *arguments, char *result, size_t size)
{
    (void)environment;
    (void)arguments;
    /*
     * No weather source is wired into 0.14, and inventing one would be worse
     * than saying so: the model reads this result out loud.
     */
    return respond_error(result, size,
                         "Weather is not available on this device yet.");
}

/*
 * The WebSocket protocol hands over a natural-language request rather than a
 * tool name, so this is the single entry the transport emits. It answers
 * honestly: routing a spoken request onto the device's tools needs a request
 * parser this release does not have, and a message the model reads out loud
 * must not claim an action that did not happen.
 */
static int tool_voice_request(const struct le_live_tool_environment *environment,
                              const char *arguments, char *result, size_t size)
{
    struct le_timer_intent intent;
    char request[LE_LIVE_ARGUMENT_MAX];
    char routed[192];
    enum le_timer_intent_kind kind;

    if (json_get_string(arguments, "request", request, sizeof(request)) < 1 ||
        !request[0])
        return respond_error(result, size, "That request was empty.");

    /* Stop is safety-critical and must not be captured by a narrower intent
       grammar such as timer dismissal. */
    if (!strcmp(request, "stop") || strstr(request, "stop playback") ||
        strstr(request, "stop the music")) {
        if (tool_media_stop(environment, "{}", result, size) < 0)
            return -1;
        return respond_ok(result, size,
                          "\"stopped\":true,\"end_session\":true");
    }

    /* Reuse the deterministic timer grammar the local assistant already uses:
       the same spoken request must not mean something different in Live mode. */
    memset(&intent, 0, sizeof(intent));
    kind = le_timer_intent_parse(request, &intent);
    if (kind == LE_TIMER_INTENT_SET) {
        if (snprintf(routed, sizeof(routed), "{\"seconds\":%lld}",
                     intent.seconds) >= (int)sizeof(routed))
            return respond_error(result, size, "That timer request is too long.");
        return tool_timer_set(environment, routed, result, size);
    }
    if (kind == LE_TIMER_INTENT_CANCEL)
        return tool_timer_cancel(environment, "{}", result, size);
    if (kind == LE_TIMER_INTENT_DISMISS)
        return tool_timer_dismiss(environment, "{}", result, size);
    if (kind == LE_TIMER_INTENT_QUERY)
        return tool_timer_query(environment, "{}", result, size);

    /* Small, explicit read-only/action vocabulary. No fuzzy model-controlled
       service name ever reaches a daemon. */
    if (strstr(request, "volume") || strstr(request, "Volume"))
        return tool_device_volume(environment, "{}", result, size);
    if (strstr(request, "what time") || strstr(request, "What time") ||
        strstr(request, "current time"))
        return tool_device_time(environment, "{}", result, size);
    if (strstr(request, "weather") || strstr(request, "Weather"))
        return tool_device_weather(environment, "{}", result, size);

    return respond_error(result, size,
                         "I cannot do that on the device yet.");
}

static int tool_session_stop(const struct le_live_tool_environment *environment,
                             const char *arguments, char *result, size_t size)
{
    (void)environment;
    (void)arguments;
    return respond_ok(result, size,
                      "\"stopped\":true,\"end_session\":true");
}

struct le_live_tool_entry {
    const char *name;
    int (*run)(const struct le_live_tool_environment *environment,
               const char *arguments, char *result, size_t size);
};

static const struct le_live_tool_entry tool_table[] = {
    {"timer.set", tool_timer_set},
    {"timer.cancel", tool_timer_cancel},
    {"timer.dismiss", tool_timer_dismiss},
    {"timer.query", tool_timer_query},
    {"media.stop", tool_media_stop},
    {"media.status", tool_media_status},
    {"radio.play", tool_radio_play},
    {"device.time", tool_device_time},
    {"device.volume", tool_device_volume},
    {"device.weather", tool_device_weather},
    {"session.stop", tool_session_stop},
    {"voice.request", tool_voice_request}
};

#define TOOL_TABLE_COUNT (sizeof(tool_table) / sizeof(tool_table[0]))

const char *le_live_tools_name(unsigned int index)
{
    return index < TOOL_TABLE_COUNT ? tool_table[index].name : NULL;
}

int le_live_tools_supported(const char *tool)
{
    unsigned int i;

    if (!tool || !tool[0] || strlen(tool) >= LE_LIVE_TOOL_NAME_MAX)
        return 0;
    for (i = 0; i < TOOL_TABLE_COUNT; ++i)
        if (!strcmp(tool_table[i].name, tool))
            return 1;
    return 0;
}

void le_live_tools_init(struct le_live_tool_environment *environment)
{
    if (!environment)
        return;
    memset(environment, 0, sizeof(*environment));
    snprintf(environment->timer_socket, sizeof(environment->timer_socket), "%s",
             DEFAULT_TIMER_SOCKET);
    snprintf(environment->radio_socket, sizeof(environment->radio_socket), "%s",
             LE_ADAPTER_RADIO_SOCK);
    snprintf(environment->audio_socket, sizeof(environment->audio_socket), "%s",
             LE_ADAPTER_AUDIO_SOCK);
    snprintf(environment->media_status_path,
             sizeof(environment->media_status_path), "%s", DEFAULT_MEDIA_STATUS);
    snprintf(environment->time_status_path,
             sizeof(environment->time_status_path), "%s", DEFAULT_TIME_STATUS);
    environment->timeout_ms = DEFAULT_TIMEOUT_MS;
}

int le_live_tools_dispatch(const struct le_live_tool_environment *environment,
                           const char *tool, const char *arguments,
                           char *result, size_t size)
{
    unsigned int i;

    if (result && size)
        result[0] = '\0';
    if (!environment || !tool || !result || size < 32)
        return -1;
    if (!le_live_tools_supported(tool))
        return respond_error(result, size,
                             "That action is not available to the voice "
                             "assistant.");
    /*
     * A tool receives a JSON object or nothing.  Anything else is a malformed
     * delegation and is refused before a handler sees it, so no handler has to
     * defend against a half-parsed document.
     */
    if (!arguments || !arguments[0])
        arguments = "{}";
    if (!json_valid_object(arguments, strlen(arguments)))
        return respond_error(result, size, "That request was malformed.");
    for (i = 0; i < TOOL_TABLE_COUNT; ++i) {
        if (strcmp(tool_table[i].name, tool))
            continue;
        return tool_table[i].run(environment, arguments, result, size);
    }
    return respond_error(result, size, "That action is not available.");
}
