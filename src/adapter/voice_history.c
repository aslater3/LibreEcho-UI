#define _POSIX_C_SOURCE 200809L

#include "voice_history.h"

#include <stdio.h>
#include <string.h>

static uint64_t now_seconds(void)
{
    return (uint64_t)time(NULL);
}

void le_voice_history_init(struct le_voice_history *history)
{
    if (!history)
        return;
    memset(history, 0, sizeof(*history));
    history->next_id = 1;
    history->generation = 1;
}

unsigned long long le_voice_history_generation(
    const struct le_voice_history *history)
{
    return history ? history->generation : 0;
}

void le_voice_history_clear(struct le_voice_history *history)
{
    unsigned long long next;

    if (!history)
        return;
    memset(history->records, 0, sizeof(history->records));
    history->next = 0;
    history->count = 0;
    next = history->generation + 1;
    if (!next)
        next = 1;
    history->generation = next;
}

unsigned le_voice_history_count(const struct le_voice_history *history)
{
    return history ? history->count : 0;
}

const struct le_voice_turn_record *le_voice_history_at(
    const struct le_voice_history *history, unsigned newest_index)
{
    unsigned index;

    if (!history || newest_index >= history->count)
        return NULL;
    index = (history->next + LE_VOICE_HISTORY_CAPACITY - 1U - newest_index) %
            LE_VOICE_HISTORY_CAPACITY;
    return &history->records[index];
}

const struct le_voice_turn_record *le_voice_history_find(
    const struct le_voice_history *history, uint64_t id)
{
    unsigned i;

    for (i = 0; i < le_voice_history_count(history); ++i) {
        const struct le_voice_turn_record *record =
            le_voice_history_at(history, i);

        if (record && record->id == id)
            return record;
    }
    return NULL;
}

int le_voice_turn_status_valid(int status)
{
    return status >= LE_VOICE_TURN_COMPLETED &&
           status <= LE_VOICE_TURN_TIMED_OUT;
}

const char *le_voice_turn_status_name(int status)
{
    switch (status) {
    case LE_VOICE_TURN_COMPLETED: return "completed";
    case LE_VOICE_TURN_STT_FAILED: return "stt_failed";
    case LE_VOICE_TURN_ASSISTANT_FAILED: return "assistant_failed";
    case LE_VOICE_TURN_TTS_FAILED: return "tts_failed";
    case LE_VOICE_TURN_CANCELLED: return "cancelled";
    case LE_VOICE_TURN_TIMED_OUT: return "timed_out";
    default: return "unknown";
    }
}

const char *le_voice_turn_status_reason(int status)
{
    switch (status) {
    case LE_VOICE_TURN_STT_FAILED: return "speech not recognised";
    case LE_VOICE_TURN_ASSISTANT_FAILED: return "assistant failed";
    case LE_VOICE_TURN_TTS_FAILED: return "speech playback failed";
    case LE_VOICE_TURN_CANCELLED: return "cancelled";
    case LE_VOICE_TURN_TIMED_OUT: return "timed out";
    default: return NULL;
    }
}

/*
 * Length of the valid UTF-8 sequence starting at 'bytes' (a NUL-terminated
 * buffer), or 0 when the lead byte is invalid, a continuation byte is missing,
 * or the sequence is cut short by the end of the string. Overlong, surrogate
 * and out-of-range encodings are rejected so a decoded length is always safe.
 */
static size_t utf8_sequence_length(const unsigned char *bytes)
{
    unsigned char lead = bytes[0];
    unsigned int codepoint, minimum;
    size_t length, i;

    if (lead < 0x80U)
        return 1U;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U; codepoint = lead & 0x1FU; minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3U; codepoint = lead & 0x0FU; minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4U; codepoint = lead & 0x07U; minimum = 0x10000U;
    } else {
        return 0U;
    }
    for (i = 1U; i < length; ++i) {
        unsigned char continuation = bytes[i];

        if ((continuation & 0xC0U) != 0x80U)
            return 0U;
        codepoint = (codepoint << 6) | (continuation & 0x3FU);
    }
    if (codepoint < minimum ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU)
        return 0U;
    return length;
}

void le_voice_history_copy_text(char *destination, size_t size,
                                const char *source, int *truncated)
{
    size_t in = 0;
    size_t out = 0;

    if (truncated)
        *truncated = 0;
    if (!destination || size == 0)
        return;
    destination[0] = '\0';
    if (!source)
        return;
    while (source[in]) {
        unsigned char c = (unsigned char)source[in];
        size_t length;

        if (c < 0x80U) {
            if (out + 1U >= size) {
                if (truncated)
                    *truncated = 1;
                break;
            }
            /* Human text only: fold stray control characters to a space so the
             * JSON writer only ever has to double quotes, backslashes and the
             * three escapes it names. */
            destination[out++] =
                (c < 0x20U && c != '\n' && c != '\r' && c != '\t')
                    ? ' ' : (char)c;
            ++in;
            continue;
        }
        length = utf8_sequence_length((const unsigned char *)source + in);
        if (length == 0U) {
            /* Invalid input policy: a malformed byte is replaced so the stored
             * text is always valid UTF-8, never a split JSON string. */
            if (out + 1U >= size) {
                if (truncated)
                    *truncated = 1;
                break;
            }
            destination[out++] = '?';
            ++in;
            continue;
        }
        if (out + length >= size) {
            /* The whole codepoint cannot fit: drop it rather than emit a
             * partial (invalid) sequence. */
            if (truncated)
                *truncated = 1;
            break;
        }
        memcpy(destination + out, source + in, length);
        out += length;
        in += length;
    }
    destination[out] = '\0';
}

void le_voice_history_timestamp(char *out, size_t size, time_t when)
{
    struct tm tm;
    char buffer[LE_VOICE_HISTORY_TIMESTAMP_MAX + 8U];
    char offset[8];
    size_t length;

    if (!out || size == 0)
        return;
    out[0] = '\0';
    if (size < 21U)
        return;
    if (when == (time_t)0)
        when = (time_t)now_seconds();
    if (!localtime_r(&when, &tm)) {
        snprintf(out, size, "1970-01-01T00:00:00+00:00");
        return;
    }
    offset[0] = '\0';
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &tm) == 0) {
        snprintf(out, size, "1970-01-01T00:00:00+00:00");
        return;
    }
    length = strlen(buffer);
    if (length >= 5U &&
        (buffer[length - 5U] == '+' || buffer[length - 5U] == '-')) {
        /* %z is +HHMM; present it as +HH:MM. */
        snprintf(offset, sizeof(offset), "%c%.2s:%.2s",
                 buffer[length - 5U], buffer + length - 4U,
                 buffer + length - 2U);
        buffer[length - 5U] = '\0';
        if (snprintf(out, size, "%s%s", buffer, offset) < 0)
            out[0] = '\0';
    } else {
        snprintf(out, size, "%s+00:00", buffer);
    }
}

int le_voice_history_add(struct le_voice_history *history,
                         unsigned long long generation,
                         const struct le_voice_turn_record *record)
{
    struct le_voice_turn_record *slot;

    if (!history || !record || !le_voice_turn_status_valid(record->status))
        return -1;
    /* A completion that predates the most recent clear must not resurrect a
     * scrubbed turn into the freshly emptied ring. */
    if (generation != history->generation)
        return 0;
    slot = &history->records[history->next];
    memset(slot, 0, sizeof(*slot));
    slot->id = history->next_id;
    if (++history->next_id == 0)
        history->next_id = 1;
    snprintf(slot->request_id, sizeof(slot->request_id), "%s",
             record->request_id);
    if (record->timestamp[0])
        snprintf(slot->timestamp, sizeof(slot->timestamp), "%s",
                 record->timestamp);
    else
        le_voice_history_timestamp(slot->timestamp, sizeof(slot->timestamp),
                                   (time_t)0);
    le_voice_history_copy_text(slot->transcript, sizeof(slot->transcript),
                               record->transcript,
                               &slot->transcript_truncated);
    /* A caller that already knows the canonical text exceeded the storage
     * bound (for example a longer STT utterance) marks the record itself. */
    slot->transcript_truncated |= record->transcript_truncated ? 1 : 0;
    le_voice_history_copy_text(slot->response, sizeof(slot->response),
                               record->response, &slot->response_truncated);
    slot->response_truncated |= record->response_truncated ? 1 : 0;
    slot->status = record->status;
    if (record->reason[0])
        snprintf(slot->reason, sizeof(slot->reason), "%s", record->reason);
    else {
        const char *reason = le_voice_turn_status_reason(slot->status);

        if (reason)
            snprintf(slot->reason, sizeof(slot->reason), "%s", reason);
    }
    slot->stt_ms = record->stt_ms;
    slot->assistant_ms = record->assistant_ms;
    slot->tts_ms = record->tts_ms;
    history->next = (history->next + 1U) % LE_VOICE_HISTORY_CAPACITY;
    if (history->count < LE_VOICE_HISTORY_CAPACITY)
        ++history->count;
    return 1;
}

int le_voice_history_set_status(struct le_voice_history *history,
                                unsigned long long generation, uint64_t id,
                                int status, const char *reason)
{
    unsigned i;

    if (!history || !le_voice_turn_status_valid(status))
        return -1;
    if (generation != history->generation)
        return 0;
    for (i = 0; i < history->count; ++i) {
        struct le_voice_turn_record *record =
            &history->records[(history->next + LE_VOICE_HISTORY_CAPACITY - 1U -
                               i) % LE_VOICE_HISTORY_CAPACITY];

        if (record->id != id)
            continue;
        /* Upgrade a completed turn at most once; id/timestamp never move. */
        if (record->status != LE_VOICE_TURN_COMPLETED ||
            status == LE_VOICE_TURN_COMPLETED)
            return 0;
        record->status = status;
        memset(record->reason, 0, sizeof(record->reason));
        if (reason && reason[0])
            snprintf(record->reason, sizeof(record->reason), "%s", reason);
        else {
            const char *fixed = le_voice_turn_status_reason(status);

            if (fixed)
                snprintf(record->reason, sizeof(record->reason), "%s", fixed);
        }
        return 1;
    }
    return 0;
}

/* Escape at most 'max_input' bytes of source (0 means all), writing into the
 * tail of out at *used. Returns -1 when out cannot hold the result. The input
 * bound is backed off to the last complete UTF-8 sequence so a bounded preview
 * or detail field never ends on a split codepoint. */
static int append_escaped(char *out, size_t out_size, size_t *used,
                          const char *source, size_t max_input)
{
    size_t limit = 0;
    size_t i;

    while (source[limit] && (max_input == 0U || limit < max_input)) {
        unsigned char c = (unsigned char)source[limit];
        size_t length;

        if (c < 0x80U) {
            ++limit;
            continue;
        }
        length = utf8_sequence_length((const unsigned char *)source + limit);
        if (length == 0U) {
            ++limit;   /* malformed byte: copied raw, one byte at a time */
            continue;
        }
        if (max_input != 0U && limit + length > max_input)
            break;
        limit += length;
    }
    for (i = 0; i < limit; ++i) {
        unsigned char c = (unsigned char)source[i];

        if (c == '"' || c == '\\') {
            if (*used + 2U >= out_size)
                return -1;
            out[(*used)++] = '\\';
            out[(*used)++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (*used + 2U >= out_size)
                return -1;
            out[(*used)++] = '\\';
            out[(*used)++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c < 0x20U) {
            int written;

            if (*used + 6U >= out_size)
                return -1;
            written = snprintf(out + *used, out_size - *used,
                               "\\u%04x", c);
            if (written != 6)
                return -1;
            *used += (size_t)written;
        } else {
            if (*used + 1U >= out_size)
                return -1;
            out[(*used)++] = (char)c;
        }
    }
    out[*used] = '\0';
    return (int)*used;
}

static int escape_into(char *destination, size_t size, const char *source,
                       size_t max_input)
{
    size_t used = 0;

    return append_escaped(destination, size, &used, source, max_input);
}

static int serialize_with_preview(const struct le_voice_history *history,
                                  char *out, size_t size, unsigned preview)
{
    size_t used;
    unsigned i;
    int written;

    if (!history || !out || size == 0U)
        return -1;
    written = snprintf(
        out, size,
        "{\"history_generation\":%llu,\"capacity\":%u,\"count\":%u,"
        "\"preview_chars\":%u,\"turns\":[",
        history->generation, LE_VOICE_HISTORY_CAPACITY, history->count,
        preview);
    if (written < 0 || (size_t)written >= size)
        return -1;
    used = (size_t)written;
    for (i = 0; i < history->count; ++i) {
        const struct le_voice_turn_record *record =
            le_voice_history_at(history, i);
        char stamp[LE_VOICE_HISTORY_TIMESTAMP_MAX * 2U + 1U];
        char transcript[LE_VOICE_HISTORY_PREVIEW_CHARS * 2U + 1U];
        char response[LE_VOICE_HISTORY_PREVIEW_CHARS * 2U + 1U];
        const char *reason;
        char error[LE_VOICE_HISTORY_REASON_MAX * 2U + 2U];

        if (!record)
            return -1;
        transcript[0] = '\0';
        response[0] = '\0';
        if (escape_into(stamp, sizeof(stamp), record->timestamp, 0) < 0 ||
            (preview && escape_into(transcript, sizeof(transcript),
                                    record->transcript, preview) < 0) ||
            (preview && escape_into(response, sizeof(response),
                                    record->response, preview) < 0))
            return -1;
        reason = record->status == LE_VOICE_TURN_COMPLETED
            ? NULL : le_voice_turn_status_reason(record->status);
        if (reason) {
            if (escape_into(error + 1, sizeof(error) - 1U, reason, 0) < 0)
                return -1;
            error[0] = '\"';
            strcat(error, "\"");
        } else {
            snprintf(error, sizeof(error), "null");
        }
        written = snprintf(
            out + used, size - used,
            "%s{\"id\":%llu,\"timestamp\":\"%s\",\"status\":\"%s\","
            "\"transcript_preview\":\"%s\",\"response_preview\":\"%s\","
            "\"transcript_truncated\":%s,\"response_truncated\":%s,"
            "\"transcript_length\":%u,\"response_length\":%u,"
            "\"stt_ms\":%u,\"assistant_ms\":%u,\"tts_ms\":%u,"
            "\"error\":%s}",
            i ? "," : "", (unsigned long long)record->id, stamp,
            le_voice_turn_status_name(record->status), transcript, response,
            record->transcript_truncated ? "true" : "false",
            record->response_truncated ? "true" : "false",
            (unsigned)strlen(record->transcript),
            (unsigned)strlen(record->response),
            (unsigned)record->stt_ms, (unsigned)record->assistant_ms,
            (unsigned)record->tts_ms, error);
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
    }
    written = snprintf(out + used, size - used, "]}");
    if (written < 0 || (size_t)written >= size - used)
        return -1;
    used += (size_t)written;
    return (int)used;
}

int le_voice_history_serialize(const struct le_voice_history *history,
                               char *out, size_t size)
{
    int length;

    /*
     * Ten records can share the bounded adapter/HTTP message. A preview is
     * included when the worst-case-escaped whole response still fits; when the
     * text is unusually escape-heavy the collection degrades to metadata only
     * (preview_chars 0) rather than dropping a record. The full bounded text of
     * any record is always available from le_voice_history_serialize_entry().
     */
    length = serialize_with_preview(history, out, size,
                                    LE_VOICE_HISTORY_PREVIEW_CHARS);
    if (length < 0)
        length = serialize_with_preview(history, out, size, 0);
    return length;
}

int le_voice_history_serialize_entry(const struct le_voice_history *history,
                                     uint64_t id, char *out, size_t size)
{
    const struct le_voice_turn_record *record;
    char stamp[LE_VOICE_HISTORY_TIMESTAMP_MAX * 2U + 1U];
    char transcript[LE_VOICE_HISTORY_DETAIL_TRANSCRIPT_CHARS * 2U + 1U];
    char response[LE_VOICE_HISTORY_DETAIL_RESPONSE_CHARS * 2U + 1U];
    const char *reason;
    char error[LE_VOICE_HISTORY_REASON_MAX * 2U + 2U];
    int transcript_truncated;
    int response_truncated;
    int written;

    if (!history || !out || size == 0U)
        return -2;
    record = le_voice_history_find(history, id);
    if (!record)
        return -1;
    if (escape_into(stamp, sizeof(stamp), record->timestamp, 0) < 0 ||
        escape_into(transcript, sizeof(transcript), record->transcript,
                    LE_VOICE_HISTORY_DETAIL_TRANSCRIPT_CHARS) < 0 ||
        escape_into(response, sizeof(response), record->response,
                    LE_VOICE_HISTORY_DETAIL_RESPONSE_CHARS) < 0)
        return -2;
    transcript_truncated = record->transcript_truncated ||
        strlen(record->transcript) > LE_VOICE_HISTORY_DETAIL_TRANSCRIPT_CHARS;
    response_truncated = record->response_truncated ||
        strlen(record->response) > LE_VOICE_HISTORY_DETAIL_RESPONSE_CHARS;
    reason = record->status == LE_VOICE_TURN_COMPLETED
        ? NULL : le_voice_turn_status_reason(record->status);
    if (reason) {
        if (escape_into(error + 1, sizeof(error) - 1U, reason, 0) < 0)
            return -2;
        error[0] = '\"';
        strcat(error, "\"");
    } else {
        snprintf(error, sizeof(error), "null");
    }
    written = snprintf(
        out, size,
        "{\"id\":%llu,\"timestamp\":\"%s\",\"status\":\"%s\","
        "\"transcript\":\"%s\",\"response\":\"%s\","
        "\"transcript_truncated\":%s,\"response_truncated\":%s,"
        "\"transcript_length\":%u,\"response_length\":%u,"
        "\"stt_ms\":%u,\"assistant_ms\":%u,\"tts_ms\":%u,"
        "\"error\":%s,\"history_generation\":%llu}",
        (unsigned long long)record->id, stamp,
        le_voice_turn_status_name(record->status), transcript, response,
        transcript_truncated ? "true" : "false",
        response_truncated ? "true" : "false",
        (unsigned)strlen(record->transcript),
        (unsigned)strlen(record->response),
        (unsigned)record->stt_ms, (unsigned)record->assistant_ms,
        (unsigned)record->tts_ms, error, history->generation);
    if (written < 0 || (size_t)written >= size)
        return -2;
    return written;
}
