#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "music_visualizer_protocol.h"

#include <string.h>

/* ------------------------- minimal JSON scanning ------------------------ */

static const char *jskip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* p points at '"'.  Returns the position just past the closing quote. */
static const char *jstring_end(const char *p)
{
    if (*p != '"')
        return NULL;
    p++;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return NULL;
}

/* Skip one JSON value at p; depth bounds nesting. */
static const char *jvalue_end(const char *p, unsigned int depth)
{
    const char *end;

    if (depth > 8U)
        return NULL;
    p = jskip_ws(p);
    if (*p == '"')
        return jstring_end(p);
    if (*p == '{' || *p == '[') {
        char close = (*p == '{') ? '}' : ']';
        p++;
        for (;;) {
            p = jskip_ws(p);
            if (*p == close)
                return p + 1;
            if (*p == '\0')
                return NULL;
            end = jvalue_end(p, depth + 1U);
            if (end == NULL)
                return NULL;
            p = jskip_ws(end);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == close)
                return p + 1;
            return NULL;
        }
    }
    /* number / true / false / null: consume until a structural delimiter. */
    end = p;
    while (*end != '\0' && *end != ',' && *end != '}' && *end != ']' &&
           *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r')
        end++;
    if (end == p)
        return NULL;
    return end;
}

static int jspan_string(const char *begin, const char *end,
                        char *out, size_t out_size)
{
    size_t used = 0;

    if (end - begin < 2 || *begin != '"' || end[-1] != '"')
        return -1;
    begin++;
    end--;
    while (begin < end) {
        char c = *begin++;
        if (c == '\\') {
            if (begin >= end)
                return -1;
            c = *begin++;
            c = (c == 'n') ? '\n' : (c == 't') ? '\t' : (c == 'r') ? '\r' : c;
        }
        if (used + 1U >= out_size)
            return -1;
        out[used++] = c;
    }
    out[used] = '\0';
    return 0;
}

/* Find "key" in object span [begin,end); returns 1 and sets the value span. */
static int jfind(const char *begin, const char *end, const char *key,
                 const char **vb, const char **ve)
{
    const char *p = jskip_ws(begin);

    if (p >= end || *p != '{')
        return 0;
    p++;
    for (;;) {
        const char *kend;
        char name[48];

        p = jskip_ws(p);
        if (p >= end || *p == '}')
            return 0;
        kend = jstring_end(p);
        if (kend == NULL || kend > end)
            return 0;
        if (jspan_string(p, kend, name, sizeof(name)) != 0)
            return 0;
        p = jskip_ws(kend);
        if (p >= end || *p != ':')
            return 0;
        p++;
        p = jskip_ws(p);
        *vb = p;
        *ve = jvalue_end(p, 1U);
        if (*ve == NULL || *ve > end)
            return 0;
        if (strcmp(name, key) == 0)
            return 1;
        p = jskip_ws(*ve);
        if (*ve < end && *p == ',') {
            p++;
            continue;
        }
        return 0;
    }
}

static int jspan_uint(const char *begin, const char *end,
                      unsigned long long *out)
{
    unsigned long long value = 0;
    const char *p = begin;

    if (p >= end || *p < '0' || *p > '9')
        return -1;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10ULL + (unsigned long long)(*p - '0');
        if (value > 0xFFFFFFFFULL)
            return -1;
        p++;
    }
    if (p != end)
        return -1;
    *out = value;
    return 0;
}

/* ----------------------------- field decode ----------------------------- */

struct field_spec {
    const char *name;
    size_t offset;
    unsigned int max;
};

#define FIELD_OFF(f) offsetof(struct le_music_features, f)

static const struct field_spec le_fields[] = {
    {"energy",          FIELD_OFF(energy),          255U},
    {"warmth",          FIELD_OFF(warmth),          255U},
    {"brightness_axis", FIELD_OFF(brightness_axis), 255U},
    {"density",         FIELD_OFF(density),         255U},
    {"transientness",   FIELD_OFF(transientness),   255U},
    {"groove",          FIELD_OFF(groove),          255U},
    {"build",           FIELD_OFF(build),           255U},
    {"spaciousness",    FIELD_OFF(spaciousness),    255U},
    {"loudness_fast",   FIELD_OFF(loudness_fast),   255U},
    {"loudness_slow",   FIELD_OFF(loudness_slow),   255U},
    {"onset_low",       FIELD_OFF(onset_low),       255U},
    {"onset_mid",       FIELD_OFF(onset_mid),       255U},
    {"onset_high",      FIELD_OFF(onset_high),      255U},
    {"beat_strength",   FIELD_OFF(beat_strength),   255U},
    {"beat_confidence", FIELD_OFF(beat_confidence), 255U},
    {"novelty",         FIELD_OFF(novelty),         255U},
    {"event_strength",  FIELD_OFF(event_strength),  255U},
    {"beat_phase",      FIELD_OFF(beat_phase),      65535U},
    {"bpm_x100",        FIELD_OFF(bpm_x100),        30000U},
    {"seq",             FIELD_OFF(seq),             0xFFFFFFFFU},
    {"timestamp_ms",    FIELD_OFF(timestamp_ms),    0xFFFFFFFFU},
    {"session",         FIELD_OFF(session),         0xFFFFFFFFU}
};

static const struct {
    const char *name;
    unsigned int bit;
} le_event_names[] = {
    {"kick", LE_MUSIC_EVENT_KICK},
    {"snare", LE_MUSIC_EVENT_SNARE},
    {"high", LE_MUSIC_EVENT_HIGH},
    {"fill", LE_MUSIC_EVENT_FILL},
    {"build", LE_MUSIC_EVENT_BUILD},
    {"reentry", LE_MUSIC_EVENT_REENTRY},
    {"breakdown", LE_MUSIC_EVENT_BREAKDOWN},
    {"section", LE_MUSIC_EVENT_SECTION},
    {"drop", LE_MUSIC_EVENT_DROP}
};

static int le_event_name_bit(const char *name, size_t len, unsigned int *bit)
{
    size_t i;

    for (i = 0U; i < sizeof(le_event_names) / sizeof(le_event_names[0]); i++) {
        if (strlen(le_event_names[i].name) == len &&
            strncmp(le_event_names[i].name, name, len) == 0) {
            *bit = le_event_names[i].bit;
            return 0;
        }
    }
    return -1;
}

/* events may be an integer bitmask, an array of ints/names, or a string. */
static int le_parse_events(const char *begin, const char *end,
                           unsigned int *out)
{
    const char *p = jskip_ws(begin);
    unsigned int mask = 0;

    if (p >= end)
        return -1;
    if (*p == '"') {
        char text[128];
        char *token;
        char *save = NULL;

        if (jspan_string(p, jstring_end(p), text, sizeof(text)) != 0)
            return -1;
        for (token = strtok_r(text, " ,|+", &save); token != NULL;
             token = strtok_r(NULL, " ,|+", &save)) {
            unsigned int bit;
            unsigned long long value;

            if (jspan_uint(token, token + strlen(token), &value) == 0) {
                if (value > 0xFFFFULL || (value & ~511ULL) != 0ULL)
                    return -1;
                mask |= (unsigned int)value;
            } else if (le_event_name_bit(token, strlen(token), &bit) == 0) {
                mask |= bit;
            } else {
                return -1;
            }
        }
        *out = mask;
        return 0;
    }
    if (*p == '[') {
        p++;
        for (;;) {
            const char *vend;
            unsigned long long value;
            unsigned int bit;

            p = jskip_ws(p);
            if (p >= end)
                return -1;
            if (*p == ']') {
                *out = mask;
                return 0;
            }
            if (*p == '"') {
                const char *send = jstring_end(p);
                char name[32];
                if (send == NULL || jspan_string(p, send, name,
                                                 sizeof(name)) != 0)
                    return -1;
                if (le_event_name_bit(name, strlen(name), &bit) != 0)
                    return -1;
                mask |= bit;
                vend = send;
            } else {
                vend = p;
                while (vend < end && *vend != ',' && *vend != ']' &&
                       *vend != ' ')
                    vend++;
                if (jspan_uint(p, vend, &value) != 0 || value > 511ULL)
                    return -1;
                mask |= (unsigned int)value;
            }
            p = jskip_ws(vend);
            if (p < end && *p == ',') {
                p++;
                continue;
            }
            if (p < end && *p == ']') {
                *out = mask;
                return 0;
            }
            return -1;
        }
    }
    {
        unsigned long long value;
        if (jspan_uint(p, end, &value) != 0 || value > 0xFFFFULL ||
            (value & ~511ULL) != 0ULL)
            return -1;
        *out = (unsigned int)value;
        return 0;
    }
}

int le_music_parse_v2(const char *json, size_t len,
                      struct le_music_features *out)
{
    struct le_music_features parsed;
    const char *begin = json;
    const char *end = json + len;
    const char *vb;
    const char *ve;
    unsigned long long version;
    size_t i;

    if (json == NULL || out == NULL)
        return -1;
    begin = jskip_ws(begin);
    end = jskip_ws(end);
    if (begin >= end || *begin != '{' || end[-1] != '}')
        return -1;

    /* feature_version is the discriminator; absent means legacy v1. */
    if (jfind(begin, end, "feature_version", &vb, &ve) != 1)
        return 0;
    if (jspan_uint(jskip_ws(vb), ve, &version) != 0)
        return -1;
    if (version != LE_MUSIC_FEATURE_VERSION)
        return -1;

    memset(&parsed, 0, sizeof(parsed));
    for (i = 0U; i < sizeof(le_fields) / sizeof(le_fields[0]); i++) {
        unsigned long long value;
        unsigned int *slot = (unsigned int *)(void *)
            ((char *)&parsed + le_fields[i].offset);

        /* Every frozen field is required in a v2 frame. */
        if (jfind(begin, end, le_fields[i].name, &vb, &ve) != 1)
            return -1;
        if (jspan_uint(jskip_ws(vb), ve, &value) != 0)
            return -1;
        if (le_fields[i].max != 0xFFFFFFFFU && value > le_fields[i].max)
            return -1;
        *slot = (unsigned int)value;
    }
    if (jfind(begin, end, "events", &vb, &ve) != 1)
        return -1;
    if (le_parse_events(vb, ve, &parsed.events) != 0)
        return -1;

    parsed.feature_version = LE_MUSIC_FEATURE_VERSION;
    if (parsed.session == 0U)
        return -1;
    *out = parsed;
    return 1;
}

void le_music_stream_reset(struct le_music_stream *stream)
{
    if (stream == NULL)
        return;
    stream->session = 0U;
    stream->last_seq = 0U;
    stream->last_timestamp_ms = 0ULL;
    stream->have_session = 0;
}

int le_music_stream_accept(struct le_music_stream *stream,
                           const struct le_music_features *features)
{
    if (stream == NULL || features == NULL)
        return 0;
    if (!stream->have_session || features->session != stream->session) {
        /* New session identity: accept and restart the sequence. */
        stream->session = features->session;
        stream->last_seq = features->seq;
        stream->last_timestamp_ms = features->timestamp_ms;
        stream->have_session = 1;
        return 1;
    }
    if (features->seq <= stream->last_seq)
        return 0;                        /* duplicate or reordered */
    if ((unsigned long long)features->timestamp_ms <
        stream->last_timestamp_ms)
        return 0;                        /* timestamp went backwards */
    stream->last_seq = features->seq;
    stream->last_timestamp_ms = features->timestamp_ms;
    return 1;
}
