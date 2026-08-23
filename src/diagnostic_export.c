#include "api.h"
#include "json.h"
#include "version.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LE_DIAG_MAX_BYTES 24576
#define LE_DIAG_LOGS 8
#define LE_DIAG_TEXT 256

struct diag_writer { struct api_response *response; size_t used; int failed; };

static int diag_append(struct diag_writer *w, const char *format, ...)
{
    va_list ap;
    int n;
    if (!w || w->failed || w->used >= LE_DIAG_MAX_BYTES)
        return -1;
    va_start(ap, format);
    n = vsnprintf(w->response->body + w->used,
                  sizeof(w->response->body) - w->used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(w->response->body) - w->used ||
        w->used + (size_t)n >= LE_DIAG_MAX_BYTES) {
        w->failed = 1;
        return -1;
    }
    w->used += (size_t)n;
    return 0;
}

static void bounded_copy(char *out, size_t out_size, const char *value)
{
    size_t n;
    if (!out || !out_size)
        return;
    if (!value) {
        out[0] = '\0';
        return;
    }
    n = strnlen(value, out_size - 1);
    memcpy(out, value, n);
    out[n] = '\0';
}

static void json_value(char *out, size_t out_size, const char *value)
{
    char bounded[LE_DIAG_TEXT];
    bounded_copy(bounded, sizeof(bounded), value);
    json_escape(out, out_size, bounded);
}

static const char *json_bool(int value)
{
    return value ? "true" : "false";
}

static int safe_token(char *out, size_t out_size, const char *value)
{
    size_t i, n;
    if (!out || !out_size || !value)
        return 0;
    n = strnlen(value, out_size);
    if (!n || n >= out_size)
        return 0;
    for (i = 0; i < n; ++i)
        if (!isalnum((unsigned char)value[i]) && value[i] != '.' &&
            value[i] != '_' && value[i] != '-' && value[i] != ':')
            return 0;
    memcpy(out, value, n + 1);
    return 1;
}

static int read_key(const char *path, const char *key, char *out, size_t out_size)
{
    int fd;
    struct stat st;
    char data[4097];
    ssize_t count;
    size_t key_len, offset = 0;
    if (!path || !key || !out || !out_size)
        return 0;
    out[0] = '\0';
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    count = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (count <= 0 || count >= (ssize_t)sizeof(data) - 1)
        return 0;
    data[count] = '\0';
    key_len = strlen(key);
    while (offset < (size_t)count) {
        char *line = data + offset;
        char *end = strchr(line, '\n');
        char *value;
        size_t line_len, n;
        if (!end)
            end = data + count;
        line_len = (size_t)(end - line);
        if (line_len > 0 && line[line_len - 1] == '\r')
            --line_len;
        if (line_len > key_len && !strncmp(line, key, key_len) &&
            line[key_len] == '=') {
            value = line + key_len + 1;
            value[line_len - key_len - 1] = '\0';
            n = strnlen(value, out_size);
            if (n >= out_size || !safe_token(out, out_size, value)) {
                out[0] = '\0';
                return 0;
            }
            return 1;
        }
        offset = end < data + count ? (size_t)(end - data) + 1 : (size_t)count;
    }
    return 0;
}

static int redact_text(char *out, size_t out_size, const char *input)
{
    static const char *sensitive[] = {
        "password", "passwd", "psk", "secret", "token", "cookie",
        "authorization", "bearer", "ssid", "bssid", "username", "address",
        "mac", "hostname", "path", NULL
    };
    size_t i = 0, used = 0;
    if (!out || !out_size)
        return 0;
    out[0] = '\0';
    if (!input)
        return 1;
    while (input[i] && used + 1 < out_size) {
        size_t k;
        int matched = 0;
        if (input[i] == '/') {
            const char *p = "[path-redacted]";
            size_t n = strlen(p);
            if (used + n >= out_size)
                break;
            memcpy(out + used, p, n);
            used += n;
            while (input[i] && !isspace((unsigned char)input[i]))
                ++i;
            continue;
        }
        for (k = 0; sensitive[k]; ++k) {
            size_t n = strlen(sensitive[k]);
            size_t j;
            if (strncasecmp(input + i, sensitive[k], n) ||
                (i && (isalnum((unsigned char)input[i - 1]) || input[i - 1] == '_')))
                continue;
            j = i + n;
            while (input[j] == ' ' || input[j] == '\t' || input[j] == '=' ||
                   input[j] == ':')
                ++j;
            if (j == i + n)
                continue;
            {
                const char *p = "[redacted]";
                size_t pn = strlen(p);
                if (used + n + 1 + pn >= out_size)
                    return 0;
                memcpy(out + used, input + i, n);
                used += n;
                out[used++] = '=';
                memcpy(out + used, p, pn);
                used += pn;
            }
            i = j;
            if (input[i] == '"') {
                ++i;
                while (input[i] && input[i] != '"')
                    ++i;
                if (input[i] == '"')
                    ++i;
            } else {
                while (input[i] && !isspace((unsigned char)input[i]) &&
                       input[i] != ',' && input[i] != ';' && input[i] != '}' &&
                       input[i] != ']')
                    ++i;
            }
            matched = 1;
            break;
        }
        if (matched)
            continue;
        if (!strncasecmp(input + i, "Bearer ", 7)) {
            const char *p = "Bearer [redacted]";
            size_t n = strlen(p);
            if (used + n >= out_size)
                break;
            memcpy(out + used, p, n);
            used += n;
            i += 7;
            while (input[i] && !isspace((unsigned char)input[i]) && input[i] != '"')
                ++i;
            continue;
        }
        out[used++] = input[i++];
    }
    out[used] = '\0';
    return input[i] == '\0';
}

static void redacted_json(char *out, size_t out_size, const char *value)
{
    char redacted[LE_DIAG_TEXT];
    if (!redact_text(redacted, sizeof(redacted), value))
        snprintf(redacted, sizeof(redacted), "[redacted]");
    json_escape(out, out_size, redacted);
}

static int fixed_file_summary(const char *path, size_t *bytes, int *warning)
{
    int fd;
    struct stat st;
    char buffer[1024];
    ssize_t n;
    size_t total = 0;
    int found_warning = 0;
    if (bytes)
        *bytes = 0;
    if (warning)
        *warning = 0;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    while (total < 4096 && (n = read(fd, buffer, sizeof(buffer))) > 0) {
        size_t i;
        size_t used = (size_t)n;
        if (used > 4096 - total)
            used = 4096 - total;
        for (i = 0; i + 5 <= used; ++i)
            if (!strncasecmp(buffer + i, "error", 5) ||
                !strncasecmp(buffer + i, "panic", 5) ||
                !strncasecmp(buffer + i, "reset", 5))
                found_warning = 1;
        total += used;
    }
    close(fd);
    if (bytes)
        *bytes = total;
    if (warning)
        *warning = found_warning;
    return 1;
}

static int append_logs(struct diag_writer *w, const struct api_context *c)
{
    size_t count = c->log_count < LE_DIAG_LOGS ? c->log_count : LE_DIAG_LOGS;
    size_t first = c->log_next >= count ? c->log_next - count : 0;
    size_t i;
    if (diag_append(w, "\"logs\":{\"available\":%s,\"source\":\"web-memory\",\"bounded\":true,\"entries\":[",
                    count ? "true" : "false"))
        return -1;
    for (i = 0; i < count; ++i) {
        const char *line = c->logs[(first + i) % LE_MAX_LOGS];
        char level[32], message[LE_DIAG_TEXT], redacted[LE_DIAG_TEXT * 2];
        int boot_seconds = 0;
        int parsed = json_get_string(line, "level", level, sizeof(level)) > 0 &&
                     json_get_string(line, "message", message, sizeof(message)) > 0;
        if (parsed)
            json_get_int(line, "boot_seconds", &boot_seconds);
        else {
            snprintf(level, sizeof(level), "unknown");
            snprintf(message, sizeof(message), "unavailable");
        }
        redacted_json(redacted, sizeof(redacted), message);
        if (diag_append(w, "%s{\"level\":\"%s\",\"boot_seconds\":%d,\"message\":\"%s\"}",
                        i ? "," : "", level, boot_seconds, redacted))
            return -1;
    }
    return diag_append(w, "],\"malformed_entries\":%s},", c->log_count > LE_MAX_LOGS ? "true" : "false");
}

static void append_unavailable(struct diag_writer *w, const char *name)
{
    (void)diag_append(w, "\"%s\":{\"available\":false,\"reason\":\"unavailable\"},", name);
}

void diagnostics_export_json(struct api_context *c, struct api_response *r)
{
    struct le_system_status status;
    struct le_device_info device;
    struct le_network_state network;
    struct le_audio_state audio;
    struct le_wake_word_state wake;
    struct le_bluetooth_state bluetooth;
    struct le_playback_state playback;
    char value[128], escaped[LE_DIAG_TEXT * 2];
    char channel[16] = "stable", active_slot[16] = "unavailable";
    char pending_slot[16] = "unavailable", update_state[64] = "idle";
    char pending_version[96] = "unavailable";
    size_t reset_bytes = 0, pstore_bytes = 0, file_bytes = 0;
    int reset_warning = 0, pstore_warning = 0, file_warning = 0;
    int status_rc, device_rc, network_rc, audio_rc, wake_rc, bluetooth_rc, playback_rc;
    struct diag_writer w = {r, 0, 0};

    memset(&status, 0, sizeof(status));
    memset(&device, 0, sizeof(device));
    memset(&network, 0, sizeof(network));
    memset(&audio, 0, sizeof(audio));
    memset(&wake, 0, sizeof(wake));
    memset(&bluetooth, 0, sizeof(bluetooth));
    memset(&playback, 0, sizeof(playback));
    status_rc = le_get_system_status(c->backend, &status);
    device_rc = le_get_device_info(c->backend, &device);
    /* The Linux backend's adapter calls use synchronous socket timeouts. Keep
       the single-threaded HTTP event loop non-blocking during diagnostics;
       target-side adapters can be reported by a future cached/async path. */
    if (!strcmp(le_backend_mode(c->backend), "linux")) {
        network_rc = audio_rc = wake_rc = bluetooth_rc = playback_rc =
            LE_NOT_SUPPORTED;
    } else {
        network_rc = le_get_network_state(c->backend, &network);
        audio_rc = le_get_audio_state(c->backend, &audio);
        wake_rc = le_get_wake_word_state(c->backend, &wake);
        bluetooth_rc = le_get_bluetooth_state(c->backend, &bluetooth);
        playback_rc = le_get_playback_state(c->backend, &playback);
    }

    if (!read_key("/data/libreecho/update/check-status", "channel", value, sizeof(value)) ||
        (strcmp(value, "stable") && strcmp(value, "dev")))
        snprintf(channel, sizeof(channel), "stable");
    else
        bounded_copy(channel, sizeof(channel), value);
    if (!read_key("/data/libreecho/update/active-slot", "slot", value, sizeof(value)) ||
        (strcmp(value, "a") && strcmp(value, "b")))
        snprintf(active_slot, sizeof(active_slot), "unavailable");
    else
        bounded_copy(active_slot, sizeof(active_slot), value);
    if (!read_key("/data/libreecho/update/pending", "slot", value, sizeof(value)) ||
        (strcmp(value, "a") && strcmp(value, "b")))
        snprintf(pending_slot, sizeof(pending_slot), "unavailable");
    else
        bounded_copy(pending_slot, sizeof(pending_slot), value);
    if (!read_key("/data/libreecho/update/state", "state", value, sizeof(value)))
        snprintf(update_state, sizeof(update_state), "idle");
    else
        bounded_copy(update_state, sizeof(update_state), value);
    if (!read_key("/data/libreecho/update/pending", "version", value, sizeof(value)))
        snprintf(pending_version, sizeof(pending_version), "unavailable");
    else
        bounded_copy(pending_version, sizeof(pending_version), value);

    (void)fixed_file_summary("/run/libreecho/reset-reason", &reset_bytes, &reset_warning);
    if (fixed_file_summary("/sys/fs/pstore/console-ramoops-0", &file_bytes, &file_warning)) {
        pstore_bytes += file_bytes;
        pstore_warning |= file_warning;
    }
    if (fixed_file_summary("/sys/fs/pstore/dmesg-ramoops-0", &file_bytes, &file_warning)) {
        pstore_bytes += file_bytes;
        pstore_warning |= file_warning;
    }

    r->status = 200;
    snprintf(r->type, sizeof(r->type), "application/json; charset=utf-8");
    (void)diag_append(&w, "{\"ok\":true,\"data\":{\"schema_version\":1,\"format\":\"libreecho-diagnostic-bundle\",\"bounded\":true,\"max_bytes\":%d,", LE_DIAG_MAX_BYTES);
    if (network_rc || !safe_token(value, sizeof(value), network.connectivity))
        snprintf(value, sizeof(value), "unknown");
    (void)diag_append(&w, "\"summary\":\"backend=%s;network=%s;audio=%s;bluetooth=%s\",",
                      le_backend_mode(c->backend),
                      network_rc ? "unavailable" : value,
                      audio_rc ? "unavailable" : (audio.output_available ? "available" : "degraded"),
                      bluetooth_rc ? "unavailable" : (bluetooth.enabled ? "enabled" : "disabled"));
    (void)diag_append(&w, "\"release_identity\":{");
    (void)diag_append(&w, "\"product_version\":\"%s\",\"channel\":\"%s\",\"active_slot\":\"%s\",\"pending_slot\":\"%s\",\"ota_state\":\"%s\",\"pending_version\":\"%s\",",
                      LE_OS_VERSION_STRING, channel, active_slot, pending_slot, update_state, pending_version);
    if (device_rc)
        (void)diag_append(&w, "\"boot_image\":\"unavailable\",\"kernel\":\"unavailable\",\"dtb\":\"unavailable\",\"ui\":\"%s\",\"platform\":\"unavailable\",",
                          LE_OS_VERSION_STRING);
    else {
        char platform[LE_DIAG_TEXT * 2];
        json_value(escaped, sizeof(escaped), device.kernel);
        json_value(platform, sizeof(platform), device.hardware_revision);
        (void)diag_append(&w, "\"boot_image\":\"unavailable\",\"kernel\":\"%s\",\"dtb\":\"unavailable\",\"ui\":\"%s\",\"platform\":\"%s\",",
                          escaped, LE_OS_VERSION_STRING, platform);
    }
    (void)diag_append(&w, "\"os_version\":\"%s\",\"source_commit\":\"%s\",\"source_dirty\":%s,\"source_digest\":\"%s\",\"running_service\":{\"name\":\"libreecho-web\",\"version\":\"%s\",\"backend\":\"%s\"}},",
                      LE_OS_VERSION_STRING, LE_SOURCE_COMMIT,
                      !strcmp(LE_SOURCE_DIRTY, "1") ? "true" : "false",
                      LE_SOURCE_DIGEST, LE_OS_VERSION_STRING, le_backend_mode(c->backend));
    (void)diag_append(&w, "\"runtime\":{");
    if (status_rc)
        append_unavailable(&w, "system");
    else
    {
        json_value(escaped, sizeof(escaped), status.storage_state);
        (void)diag_append(&w, "\"system\":{\"available\":true,\"uptime_seconds\":%d,\"cpu_percent\":%d,\"memory_percent\":%d,\"memory_used_mb\":%d,\"memory_total_mb\":%d,\"storage_percent\":%d,\"storage_used_mb\":%d,\"storage_total_mb\":%d,\"storage_available\":%s,\"storage_state\":\"%s\",\"temperature_c\":%d},",
                          status.uptime < 0 ? 0 : (int)status.uptime, status.cpu, status.memory,
                          status.memory_used_mb, status.memory_total_mb, status.storage,
                          status.storage_used_mb, status.storage_total_mb,
                          json_bool(status.storage_available), escaped,
                          status.temperature);
    }
    (void)diag_append(&w, "\"last_reset\":{\"available\":%s,\"bytes\":%zu,\"warning\":%s},\"pstore\":{\"available\":%s,\"bytes\":%zu,\"warning\":%s}},",
                      reset_bytes ? "true" : "false", reset_bytes, json_bool(reset_warning),
                      pstore_bytes ? "true" : "false", pstore_bytes, json_bool(pstore_warning));
    if (network_rc)
        append_unavailable(&w, "network");
    else {
        char nstate[LE_DIAG_TEXT * 2], nconnectivity[LE_DIAG_TEXT * 2], nrecovery[LE_DIAG_TEXT * 2];
        json_value(nstate, sizeof(nstate), network.state);
        json_value(nconnectivity, sizeof(nconnectivity), network.connectivity);
        json_value(nrecovery, sizeof(nrecovery), network.recovery_stage);
        (void)diag_append(&w, "\"network\":{\"available\":true,\"state\":\"%s\",\"connectivity\":\"%s\",\"recovery_stage\":\"%s\",\"gateway_reachable\":%s,\"internet\":%s,\"dhcp\":%s,\"signal\":%d,\"rssi_dbm\":%d,\"liveness_failures\":%d},",
                          nstate, nconnectivity, nrecovery,
                          network.gateway_reachable < 0 ? "null" : json_bool(network.gateway_reachable),
                          json_bool(network.internet), json_bool(network.dhcp), network.signal,
                          network.rssi_dbm, network.liveness_failures);
    }
    if (audio_rc)
        append_unavailable(&w, "audio");
    else
        (void)diag_append(&w, "\"audio\":{\"available\":true,\"output_available\":%s,\"amplifier_on\":%s,\"microphone_muted\":%s,\"volume\":%d,\"microphone_gain\":%d,\"noise_active\":%s},",
                          json_bool(audio.output_available), json_bool(audio.amplifier_on),
                          json_bool(audio.muted), audio.volume, audio.microphone_gain,
                          json_bool(audio.noise_active));
    if (wake_rc)
        append_unavailable(&w, "wake_word");
    else {
        json_value(escaped, sizeof(escaped), wake.model_status);
        (void)diag_append(&w, "\"wake_word\":{\"available\":true,\"enabled\":%s,\"model_status\":\"%s\",\"sensitivity\":%d,\"detected_count\":%d},",
                          json_bool(wake.enabled), escaped, wake.sensitivity, wake.detected_count);
    }
    if (bluetooth_rc)
        append_unavailable(&w, "bluetooth");
    else {
        char bstate[LE_DIAG_TEXT * 2], btransport[LE_DIAG_TEXT * 2], bhci[LE_DIAG_TEXT * 2];
        char bprofile[LE_DIAG_TEXT * 2], bprofile_error[LE_DIAG_TEXT * 2];
        json_value(bstate, sizeof(bstate), bluetooth.state);
        json_value(btransport, sizeof(btransport), bluetooth.transport);
        json_value(bhci, sizeof(bhci), bluetooth.hci);
        json_value(bprofile, sizeof(bprofile), bluetooth.profile_state);
        redacted_json(escaped, sizeof(escaped), bluetooth.last_error);
        redacted_json(bprofile_error, sizeof(bprofile_error), bluetooth.profile_error);
        (void)diag_append(&w, "\"bluetooth\":{\"available\":%s,\"enabled\":%s,\"activation_attempted\":%s,\"state\":\"%s\",\"transport\":\"%s\",\"hci\":\"%s\",\"last_error\":\"%s\",\"profile_state\":\"%s\",\"profile_error\":\"%s\",\"discovered_count\":%zu,\"known_count\":%zu,\"pairing\":%s,\"pairing_mode\":%s},",
                          json_bool(bluetooth.available), json_bool(bluetooth.enabled),
                          json_bool(bluetooth.activation_attempted), bstate,
                          btransport, bhci, escaped, bprofile,
                          bprofile_error, bluetooth.discovered_count, bluetooth.known_count,
                          json_bool(bluetooth.pairing), json_bool(bluetooth.pairing_mode));
    }
    if (playback_rc)
        append_unavailable(&w, "playback");
    else {
        char pstate[LE_DIAG_TEXT * 2], psource[LE_DIAG_TEXT * 2];
        json_value(pstate, sizeof(pstate), playback.state);
        json_value(psource, sizeof(psource), playback.source);
        (void)diag_append(&w, "\"playback\":{\"available\":true,\"state\":\"%s\",\"source\":\"%s\",\"media_active\":%s,\"system_active\":%s,\"announcement_active\":%s,\"alarm_active\":%s,\"metadata_available\":%s},",
                          pstate, psource, json_bool(playback.media_active),
                          json_bool(playback.system_active), json_bool(playback.announcement_active),
                          json_bool(playback.alarm_active), json_bool(playback.metadata_available));
    }
    (void)diag_append(&w, "\"privacy\":{\"local_only\":%s,\"audio_retention\":%s,\"diagnostic_telemetry\":%s,\"crash_reports\":%s,\"hardware_mute_capable\":true},",
                      json_bool(c->privacy_local_only), json_bool(c->privacy_audio_retention),
                      json_bool(c->privacy_telemetry), json_bool(c->privacy_crash_reports));
    (void)append_logs(&w, c);
    (void)diag_append(&w, "\"manifest\":{\"sections\":[\"release_identity\",\"runtime\",\"network\",\"audio\",\"wake_word\",\"bluetooth\",\"playback\",\"privacy\",\"logs\"],\"redactions\":[\"wifi_credentials\",\"ssid_bssid\",\"ip_addresses\",\"bluetooth_addresses\",\"owner_identifiers\",\"tokens_cookies\",\"private_paths\",\"media_metadata\"],\"omitted\":[\"private_keys\",\"ota_signing_material\",\"raw_pstore\",\"arbitrary_request_paths\"]}},\"error\":null}");
    if (w.failed) {
        snprintf(r->body, sizeof(r->body), "{\"ok\":true,\"data\":{\"schema_version\":1,\"format\":\"libreecho-diagnostic-bundle\",\"bounded\":true,\"partial\":true,\"manifest\":{\"redactions\":[\"all-private-identifiers\"]}},\"error\":null}");
    }
    r->length = strlen(r->body);
}
