#define _POSIX_C_SOURCE 200809L

#include "live_tools.h"
#include "adapter.h"
#include "live_ha.h"
#include "timer_intent.h"
#include "../json.h"
#include "../timezone.h"

#include <ctype.h>
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
#define DEFAULT_HA_TOKEN_PATH "/data/libreecho/secrets/home-assistant-token"
#define HA_TIMEOUT_MS 3000
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

static int ha_control(const struct le_live_tool_environment *environment,
                      const char *entity, const char *service,
                      char *result, size_t size);
static int ha_voice_parse(const char *request, char *entity, size_t entity_size,
                          char *service, size_t service_size);

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
    (void)le_timezone_follow(NULL);
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

    {
        char entity[64];
        char service[24];

        if (ha_voice_parse(request, entity, sizeof(entity), service,
                           sizeof(service)))
            return ha_control(environment, entity, service, result, size);
    }

    return respond_error(result, size,
                         "I cannot do that on the device yet.");
}

/* --- Home Assistant ----------------------------------------------------- */

/*
 * Entities the model may read and services it may call.  Reads cover the same
 * domains a spoken request can name; writes are limited to switchable
 * domains.  Anything else is refused before Home Assistant is contacted.
 */
static int ha_domain_readable(const char *domain, size_t length)
{
    static const char *const domains[] = {
        "light", "switch", "input_boolean", "fan", "sensor", "binary_sensor"
    };
    size_t i;

    for (i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i)
        if (strlen(domains[i]) == length && !strncmp(domains[i], domain, length))
            return 1;
    return 0;
}

static int ha_domain_switchable(const char *domain, size_t length)
{
    static const char *const domains[] = {
        "light", "switch", "input_boolean", "fan"
    };
    size_t i;

    for (i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i)
        if (strlen(domains[i]) == length && !strncmp(domains[i], domain, length))
            return 1;
    return 0;
}

/* "domain.object_id" with a readable domain and a lowercase object id. */
static int ha_entity_valid(const char *entity, int switchable)
{
    const char *dot;
    size_t domain_length;
    size_t i;
    size_t object_length;

    if (!entity || strlen(entity) > 64U)
        return 0;
    dot = strchr(entity, '.');
    if (!dot || dot == entity)
        return 0;
    domain_length = (size_t)(dot - entity);
    if (switchable ? !ha_domain_switchable(entity, domain_length)
                   : !ha_domain_readable(entity, domain_length))
        return 0;
    object_length = strlen(dot + 1);
    if (!object_length || object_length > 48U)
        return 0;
    for (i = 0; i < object_length; ++i) {
        unsigned char c = (unsigned char)dot[1 + i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int ha_service_valid(const char *service)
{
    return !strcmp(service, "turn_on") || !strcmp(service, "turn_off") ||
           !strcmp(service, "toggle");
}

enum ha_outcome {
    HA_OK = 0,
    HA_NOT_CONFIGURED,
    HA_NO_TOKEN,
    HA_UNREACHABLE,
    HA_NOT_FOUND,
    HA_REFUSED
};

static int ha_outcome_result(enum ha_outcome outcome, char *result, size_t size)
{
    switch (outcome) {
    case HA_NOT_CONFIGURED:
        return respond_error(result, size, "Home Assistant is not set up on this device.");
    case HA_NO_TOKEN:
        return respond_error(result, size, "Home Assistant access is not configured.");
    case HA_NOT_FOUND:
        return respond_error(result, size, "I could not find that in Home Assistant.");
    case HA_REFUSED:
        return respond_error(result, size, "Home Assistant refused that request.");
    case HA_UNREACHABLE:
    default:
        return respond_error(result, size, "Home Assistant could not be reached.");
    }
}

/* One REST call, with the token loaded for that call only and wiped after. */
static enum ha_outcome ha_call(const struct le_live_tool_environment *environment,
                               const char *method, const char *path,
                               const char *body, struct le_live_ha_response *response)
{
    char token[LE_LIVE_HA_TOKEN_MAX];
    enum ha_outcome outcome;

    if (!environment->ha_url[0])
        return HA_NOT_CONFIGURED;
    if (le_live_ha_load_token(environment->ha_token_path, token, sizeof(token)) < 0)
        return HA_NO_TOKEN;
    if (le_live_ha_request(environment->ha_url, token, method, path, body,
                           HA_TIMEOUT_MS, response) < 0)
        outcome = HA_UNREACHABLE;
    else if (response->status == 404)
        outcome = HA_NOT_FOUND;
    else if (response->status == 401 || response->status == 403)
        outcome = HA_NO_TOKEN;
    else if (response->status < 200 || response->status >= 300)
        outcome = HA_REFUSED;
    else
        outcome = HA_OK;
    memset(token, 0, sizeof(token));
    return outcome;
}

static enum ha_outcome ha_entity_state(const struct le_live_tool_environment *environment,
                                       const char *entity, char *state, size_t size)
{
    char path[96];
    struct le_live_ha_response response;
    enum ha_outcome outcome;

    if (!ha_entity_valid(entity, 0) || size < 2U)
        return HA_REFUSED;
    if (snprintf(path, sizeof(path), "/api/states/%s", entity) >= (int)sizeof(path))
        return HA_REFUSED;
    outcome = ha_call(environment, "GET", path, NULL, &response);
    if (outcome != HA_OK)
        return outcome;
    if (json_get_string_top_level(response.body, "state", state, size) < 1)
        return HA_UNREACHABLE;
    return HA_OK;
}

static int tool_ha_state(const struct le_live_tool_environment *environment,
                         const char *arguments, char *result, size_t size)
{
    char entity[80];
    char state[64];
    char escaped[128];
    char fields[256];
    enum ha_outcome outcome;

    if (json_get_string(arguments, "entity_id", entity, sizeof(entity)) < 1 ||
        !ha_entity_valid(entity, 0))
        return respond_error(result, size, "That is not a Home Assistant entity I can read.");
    outcome = ha_entity_state(environment, entity, state, sizeof(state));
    if (outcome != HA_OK)
        return ha_outcome_result(outcome, result, size);
    if (escape_into(escaped, sizeof(escaped), state) < 0 ||
        snprintf(fields, sizeof(fields), "\"entity_id\":\"%s\",\"state\":\"%s\"",
                 entity, escaped) >= (int)sizeof(fields))
        return respond_error(result, size, "That state is too long to read out.");
    return respond_ok(result, size, fields);
}

/*
 * Switch one entity.  The entity is checked to exist before the service is
 * called, so a spoken name that matches nothing fails instead of reaching a
 * different device by accident.
 */
static int ha_control(const struct le_live_tool_environment *environment,
                      const char *entity, const char *service,
                      char *result, size_t size)
{
    char state[64];
    char escaped[128];
    char path[128];
    char body[96];
    char fields[256];
    struct le_live_ha_response response;
    enum ha_outcome outcome;

    if (!ha_entity_valid(entity, 1) || !ha_service_valid(service))
        return respond_error(result, size, "That Home Assistant action is not allowed.");
    outcome = ha_entity_state(environment, entity, state, sizeof(state));
    if (outcome != HA_OK)
        return ha_outcome_result(outcome, result, size);
    {
        const char *dot = strchr(entity, '.');

        if (snprintf(path, sizeof(path), "/api/services/%.*s/%s",
                     (int)(dot - entity), entity, service) >= (int)sizeof(path))
            return respond_error(result, size, "That Home Assistant action is too long.");
    }
    if (snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity) >=
        (int)sizeof(body))
        return respond_error(result, size, "That entity name is too long.");
    outcome = ha_call(environment, "POST", path, body, &response);
    if (outcome != HA_OK)
        return ha_outcome_result(outcome, result, size);
    if (escape_into(escaped, sizeof(escaped), state) < 0 ||
        snprintf(fields, sizeof(fields),
                 "\"entity_id\":\"%s\",\"service\":\"%s\",\"state_before\":\"%s\"",
                 entity, service, escaped) >= (int)sizeof(fields))
        return respond_error(result, size, "That result is too long to read out.");
    return respond_ok(result, size, fields);
}

static int tool_ha_call(const struct le_live_tool_environment *environment,
                        const char *arguments, char *result, size_t size)
{
    char entity[80];
    char service[24];

    if (json_get_string(arguments, "entity_id", entity, sizeof(entity)) < 1 ||
        json_get_string(arguments, "service", service, sizeof(service)) < 1)
        return respond_error(result, size, "A Home Assistant action needs an entity and a service.");
    return ha_control(environment, entity, service, result, size);
}

/*
 * Spoken form for the realtime path: "turn on|off the <name> light|lights|
 * switch".  Only that shape is recognised, the name becomes an object id, and
 * the result must still pass ha_entity_valid() and the existence check.
 * Returns 1 when the request matched, 0 otherwise.
 */
static int ha_voice_parse(const char *request, char *entity, size_t entity_size,
                          char *service, size_t service_size)
{
    char lowered[LE_LIVE_ARGUMENT_MAX];
    const char *rest;
    const char *domain;
    size_t length;
    size_t name_length;
    size_t i;
    size_t used = 0;

    if (!request || strlen(request) >= sizeof(lowered))
        return 0;
    for (i = 0; request[i]; ++i)
        lowered[i] = (char)tolower((unsigned char)request[i]);
    lowered[i] = '\0';

    if (!strncmp(lowered, "turn on the ", 12)) {
        rest = lowered + 12;
        snprintf(service, service_size, "turn_on");
    } else if (!strncmp(lowered, "turn off the ", 13)) {
        rest = lowered + 13;
        snprintf(service, service_size, "turn_off");
    } else {
        return 0;
    }
    length = strlen(rest);
    if (length > 6U && !strcmp(rest + length - 7, " lights")) {
        domain = "light";
        name_length = length - 7U;
    } else if (length > 5U && !strcmp(rest + length - 6, " light")) {
        domain = "light";
        name_length = length - 6U;
    } else if (length > 6U && !strcmp(rest + length - 7, " switch")) {
        domain = "switch";
        name_length = length - 7U;
    } else {
        return 0;
    }
    if (!name_length || name_length > 40U)
        return 0;
    /* Name words are letters and single spaces; they become "_" separators. */
    if (rest[0] == ' ' || rest[name_length - 1U] == ' ')
        return 0;
    for (i = 0; i < name_length; ++i) {
        unsigned char c = (unsigned char)rest[i];

        if (c == ' ') {
            if (rest[i + 1U] == ' ')
                return 0;
            continue;
        }
        if (!(c >= 'a' && c <= 'z'))
            return 0;
    }
    if (snprintf(entity, entity_size, "%s.", domain) >= (int)entity_size)
        return 0;
    used = strlen(entity);
    for (i = 0; i < name_length; ++i) {
        if (used + 1U >= entity_size)
            return 0;
        entity[used++] = rest[i] == ' ' ? '_' : rest[i];
    }
    entity[used] = '\0';
    return 1;
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
    {"ha.state", tool_ha_state},
    {"ha.call", tool_ha_call},
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
    snprintf(environment->ha_token_path, sizeof(environment->ha_token_path), "%s",
             DEFAULT_HA_TOKEN_PATH);
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
