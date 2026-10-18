#include "wyoming_describe.h"
#include "wyoming_client.h"
#include "../json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int voice_token(const char *s)
{
    size_t i;
    if (!s[0] || strlen(s) > 63)
        return 0;
    for (i = 0; s[i]; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

/* Next top-level element of an array body ("[...]"), strings and nesting aware. */
static int next_element(const char *s, size_t len, size_t *pos, size_t *start, size_t *end)
{
    size_t i = *pos;
    int depth = 0, in_string = 0, escaped = 0;

    if (i == 0 && len && s[0] == '[')
        i = 1;
    while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t' || s[i] == ','))
        i++;
    if (i >= len || s[i] == ']')
        return 0;
    *start = i;
    if (s[i] == '"') {
        /* A string element ends at its closing quote. */
        for (i++; i < len; i++) {
            if (escaped) escaped = 0;
            else if (s[i] == '\\') escaped = 1;
            else if (s[i] == '"') { *end = i + 1; *pos = i + 1; return 1; }
        }
        return -1;
    }
    for (; i < len; i++) {
        char c = s[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (--depth == 0) { *end = i + 1; *pos = i + 1; return 1; }
            if (depth < 0) return -1;
        }
    }
    return -1;
}

static char *copy_span(const char *s, size_t start, size_t end)
{
    char *out = malloc(end - start + 1);
    if (out) {
        memcpy(out, s + start, end - start);
        out[end - start] = '\0';
    }
    return out;
}

int le_wyoming_parse_tts_voices(const char *data, struct le_wyoming_voice *out, size_t max,
                                size_t *count, int *truncated)
{
    const char *programs;
    size_t programs_len, ppos = 0, pstart, pend;
    int r;

    *count = 0;
    *truncated = 0;
    if (json_get_array_top_level(data, "tts", &programs, &programs_len) != 1)
        return 0;   /* No TTS programs: an empty catalogue, not an error. */
    while ((r = next_element(programs, programs_len, &ppos, &pstart, &pend)) == 1) {
        char *program = copy_span(programs, pstart, pend);
        const char *voices;
        size_t voices_len, vpos = 0, vstart, vend;
        int v;

        if (!program)
            return -1;
        if (json_get_array_top_level(program, "voices", &voices, &voices_len) != 1) {
            free(program);
            continue;
        }
        while ((v = next_element(voices, voices_len, &vpos, &vstart, &vend)) == 1) {
            char *voice = copy_span(voices, vstart, vend);
            struct le_wyoming_voice item;
            const char *languages;
            size_t languages_len, lpos = 0, ls, le;
            int installed = 1;

            if (!voice) { free(program); return -1; }
            memset(&item, 0, sizeof(item));
            if (json_get_string_top_level(voice, "name", item.name, sizeof(item.name)) == 1 &&
                voice_token(item.name)) {
                if (json_get_string_top_level(voice, "description", item.description,
                                              sizeof(item.description)) != 1)
                    item.description[0] = '\0';
                if (json_get_array_top_level(voice, "languages", &languages, &languages_len) == 1 &&
                    next_element(languages, languages_len, &lpos, &ls, &le) == 1 &&
                    le - ls >= 3 && le - ls - 2 < sizeof(item.language) &&
                    languages[ls] == '"' && languages[le - 1] == '"') {
                    memcpy(item.language, languages + ls + 1, le - ls - 2);
                    item.language[le - ls - 2] = '\0';
                    if (!voice_token(item.language))
                        item.language[0] = '\0';
                }
                (void)json_get_bool(voice, "installed", &installed);
                item.installed = installed ? 1 : 0;
                if (*count < max)
                    out[(*count)++] = item;
                else
                    *truncated = 1;
            }
            free(voice);
        }
        free(program);
        if (v < 0)
            return -1;
    }
    return r < 0 ? -1 : 0;
}

static int read_line(int fd, char *out, size_t size)
{
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got <= 0)
            return -1;
        if (c == '\n') { out[n] = '\0'; return 0; }
        out[n++] = c;
    }
    return -1;
}

static int read_exact(int fd, char *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

int le_wyoming_tts_voices(const char *uri, int timeout_ms, struct le_wyoming_voice *out,
                          size_t max, size_t *count, int *truncated)
{
    static const char request[] = "{\"type\":\"describe\"}\n";
    char header[4096], type[64];
    char *data = NULL;
    struct timeval timeout;
    int fd, events, result = -1;

    *count = 0;
    *truncated = 0;
    fd = le_wyoming_connect(uri, timeout_ms);
    if (fd < 0)
        return -1;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (write(fd, request, sizeof(request) - 1) != (ssize_t)(sizeof(request) - 1))
        goto done;
    /* Skip unrelated events; the reply to describe is "info". */
    for (events = 0; events < 8; events++) {
        int data_length = 0, payload_length = 0;

        if (read_line(fd, header, sizeof(header)) || !json_valid_object(header, strlen(header)) ||
            json_get_string(header, "type", type, sizeof(type)) != 1 ||
            json_get_int(header, "data_length", &data_length) < 0 ||
            json_get_int(header, "payload_length", &payload_length) < 0 ||
            data_length < 0 || data_length > LE_WYOMING_INFO_MAX ||
            payload_length < 0 || payload_length > LE_WYOMING_INFO_MAX)
            goto done;
        if (data_length) {
            data = malloc((size_t)data_length + 1);
            if (!data || read_exact(fd, data, (size_t)data_length))
                goto done;
            data[data_length] = '\0';
        }
        if (payload_length) {
            char discard[512];
            int left = payload_length;
            while (left > 0) {
                int chunk = left > (int)sizeof(discard) ? (int)sizeof(discard) : left;
                if (read_exact(fd, discard, (size_t)chunk))
                    goto done;
                left -= chunk;
            }
        }
        if (!strcmp(type, "info")) {
            /* Older servers put the data inline in the header line. */
            if (!data) {
                const char *key = strstr(header, "\"data\"");
                const char *brace = key ? strchr(key + 6, '{') : NULL;
                size_t pos = 0, start, end;
                char wrapped[sizeof(header) + 2];

                if (!brace)
                    goto done;
                snprintf(wrapped, sizeof(wrapped), "[%s", brace);
                if (next_element(wrapped, strlen(wrapped), &pos, &start, &end) != 1)
                    goto done;
                data = copy_span(wrapped, start, end);
                if (!data)
                    goto done;
            }
            result = le_wyoming_parse_tts_voices(data, out, max, count, truncated);
            goto done;
        }
        free(data);
        data = NULL;
    }
done:
    free(data);
    close(fd);
    return result;
}
