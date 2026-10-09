#define _POSIX_C_SOURCE 200809L

/*
 * Focused harness for the canonical private voice history unit.
 *
 * Compile with:
 *   cc -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
 *      -Isrc tests/test_voice_history.c src/adapter/voice_history.c -o build/test-voice-history
 * Run:
 *   ./build/test-voice-history
 *
 * The harness drives the production ring, serialization and clear paths; it
 * never edits the shared Makefile or tests/run_tests.sh.
 */

#include "adapter/voice_history.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

#define SERIAL_BUFFER 4096U

static struct le_voice_turn_record make_record(
    const char *request_id, int status, const char *transcript,
    const char *response, uint32_t stt, uint32_t assistant, uint32_t tts)
{
    struct le_voice_turn_record record;

    memset(&record, 0, sizeof(record));
    snprintf(record.request_id, sizeof(record.request_id), "%s", request_id);
    record.status = status;
    snprintf(record.transcript, sizeof(record.transcript), "%s", transcript);
    snprintf(record.response, sizeof(record.response), "%s", response);
    record.stt_ms = stt;
    record.assistant_ms = assistant;
    record.tts_ms = tts;
    return record;
}

static unsigned directory_entries(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    unsigned count = 0;

    if (!directory)
        return (unsigned)-1;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        ++count;
    }
    closedir(directory);
    return count;
}

/* Strict UTF-8 validity for a whole NUL-terminated buffer: rejects overlong
 * encodings, surrogates, out-of-range codepoints and truncated sequences. */
static int utf8_valid(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;

    while (*s) {
        unsigned char c = *s;
        unsigned int codepoint;
        size_t need;

        if (c < 0x80U) {
            ++s;
            continue;
        } else if (c >= 0xC2U && c <= 0xDFU) {
            need = 1U; codepoint = c & 0x1FU;
        } else if (c >= 0xE0U && c <= 0xEFU) {
            need = 2U; codepoint = c & 0x0FU;
        } else if (c >= 0xF0U && c <= 0xF4U) {
            need = 3U; codepoint = c & 0x07U;
        } else {
            return 0;
        }
        ++s;
        while (need--) {
            if ((*s & 0xC0U) != 0x80U)
                return 0;
            codepoint = (codepoint << 6) | (*s & 0x3FU);
            ++s;
        }
        if (codepoint < 0x80U ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
            codepoint > 0x10FFFFU)
            return 0;
    }
    return 1;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-voice-history-XXXXXX";
    struct le_voice_history history;
    char buffer[SERIAL_BUFFER];
    char long_text[1600];
    char previous_directory[512];
    unsigned long long generation;
    unsigned long long cleared_generation;
    int result = 0;
    int length;
    unsigned i;

    CHECK(getcwd(previous_directory, sizeof(previous_directory)) != NULL);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chdir(directory) == 0);

    /* ---- empty collection ---- */
    le_voice_history_init(&history);
    CHECK(le_voice_history_count(&history) == 0);
    generation = le_voice_history_generation(&history);
    length = le_voice_history_serialize(&history, buffer, sizeof(buffer));
    CHECK(length > 0 && (size_t)length < sizeof(buffer));
    CHECK(strstr(buffer, "\"count\":0") != NULL);
    CHECK(strstr(buffer, "\"turns\":[]") != NULL);
    CHECK(strstr(buffer, "\"history_generation\":1") != NULL);
    CHECK(strstr(buffer, "\"capacity\":10") != NULL);

    /* ---- every terminal outcome, once per canonical turn ---- */
    {
        struct le_voice_turn_record record;

        record = make_record("t-complete", LE_VOICE_TURN_COMPLETED,
                             "What's the weather tomorrow?",
                             "Tomorrow will be mostly cloudy.", 620, 410, 280);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        record = make_record("t-stt", LE_VOICE_TURN_STT_FAILED, "",
                             "", 0, 0, 0);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        record = make_record("t-assistant", LE_VOICE_TURN_ASSISTANT_FAILED,
                             "Tell me a story", "", 500, 0, 0);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        record = make_record("t-tts", LE_VOICE_TURN_TTS_FAILED,
                             "Say hello", "Hello!", 300, 200, 0);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        record = make_record("t-cancel", LE_VOICE_TURN_CANCELLED,
                             "Turn it up", "Sure, one moment.", 250, 180, 0);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        record = make_record("t-timeout", LE_VOICE_TURN_TIMED_OUT,
                             "", "", 0, 0, 0);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        CHECK(le_voice_history_count(&history) == 6);
    }
    length = le_voice_history_serialize(&history, buffer, sizeof(buffer));
    CHECK(length > 0 && (size_t)length < sizeof(buffer));
    CHECK(strstr(buffer, "\"count\":6") != NULL);
    CHECK(strstr(buffer, "\"status\":\"completed\"") != NULL);
    CHECK(strstr(buffer, "\"status\":\"stt_failed\"") != NULL);
    CHECK(strstr(buffer, "\"status\":\"assistant_failed\"") != NULL);
    CHECK(strstr(buffer, "\"status\":\"tts_failed\"") != NULL);
    CHECK(strstr(buffer, "\"status\":\"cancelled\"") != NULL);
    CHECK(strstr(buffer, "\"status\":\"timed_out\"") != NULL);
    CHECK(strstr(buffer, "speech not recognised") != NULL);
    CHECK(strstr(buffer, "\"error\":null") != NULL);
    /* Newest first: the timeout record is index 0. */
    CHECK(le_voice_history_at(&history, 0) != NULL);
    CHECK(!strcmp(le_voice_history_at(&history, 0)->request_id, "t-timeout"));
    CHECK(!strcmp(le_voice_history_at(&history, 5)->request_id, "t-complete"));

    /* ---- rollover at the 11th turn keeps exactly 10, newest first ---- */
    for (i = 0; i < 5; ++i) {
        struct le_voice_turn_record record;
        char id[32];

        snprintf(id, sizeof(id), "roll-%u", i);
        record = make_record(id, LE_VOICE_TURN_COMPLETED, "hello", "hi",
                             (uint32_t)(i + 1), (uint32_t)(i + 2),
                             (uint32_t)(i + 3));
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
    }
    CHECK(le_voice_history_count(&history) == 10);
    CHECK(!strcmp(le_voice_history_at(&history, 0)->request_id, "roll-4"));
    /* t-complete and t-stt were evicted by the new turns. */
    CHECK(le_voice_history_find(&history, 1) == NULL);
    length = le_voice_history_serialize(&history, buffer, sizeof(buffer));
    CHECK(length > 0 && (size_t)length < sizeof(buffer));

    /* ---- stable id and timestamp; status upgrade happens at most once ---- */
    {
        uint64_t id;
        char stamp[LE_VOICE_HISTORY_TIMESTAMP_MAX];
        const struct le_voice_turn_record *record;
        struct le_voice_turn_record complete;

        complete = make_record("t-upgrade", LE_VOICE_TURN_COMPLETED,
                               "turn on the light", "Turned on the light.",
                               210, 160, 90);
        CHECK(le_voice_history_add(&history, generation, &complete) == 1);
        record = le_voice_history_at(&history, 0);
        CHECK(record != NULL);
        id = record->id;
        snprintf(stamp, sizeof(stamp), "%s", record->timestamp);
        CHECK(stamp[0] != '\0');
        CHECK(le_voice_history_set_status(&history, generation, id,
                                          LE_VOICE_TURN_TTS_FAILED,
                                          NULL) == 1);
        record = le_voice_history_find(&history, id);
        CHECK(record != NULL);
        CHECK(record->id == id);
        CHECK(!strcmp(record->timestamp, stamp));
        CHECK(record->status == LE_VOICE_TURN_TTS_FAILED);
        /* A second upgrade of the same turn is refused. */
        CHECK(le_voice_history_set_status(&history, generation, id,
                                          LE_VOICE_TURN_CANCELLED, NULL) == 0);
        CHECK(le_voice_history_set_status(&history, generation, 9999,
                                          LE_VOICE_TURN_TTS_FAILED,
                                          NULL) == 0);
    }

    /* ---- long internal text is flagged for truncation ---- */
    memset(long_text, 'a', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    {
        struct le_voice_turn_record record;
        const struct le_voice_turn_record *stored;
        char clipped[8];
        int flagged = 0;

        /* The bounded copier reports a source it could not hold in full. */
        le_voice_history_copy_text(clipped, sizeof(clipped), long_text,
                                   &flagged);
        CHECK(flagged == 1);
        CHECK(strlen(clipped) == sizeof(clipped) - 1U);

        memset(&record, 0, sizeof(record));
        snprintf(record.request_id, sizeof(record.request_id), "t-long");
        record.status = LE_VOICE_TURN_COMPLETED;
        memset(record.transcript, 'a', sizeof(record.transcript) - 1U);
        record.transcript[sizeof(record.transcript) - 1U] = '\0';
        record.transcript_truncated = 1;
        memset(record.response, 'a', sizeof(record.response) - 1U);
        record.response[sizeof(record.response) - 1U] = '\0';
        record.response_truncated = 1;
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        stored = le_voice_history_at(&history, 0);
        CHECK(stored != NULL);
        CHECK(stored->transcript_truncated == 1);
        CHECK(stored->response_truncated == 1);
        CHECK(strlen(stored->transcript) ==
              LE_VOICE_HISTORY_TRANSCRIPT_MAX - 1U);
        CHECK(strlen(stored->response) ==
              LE_VOICE_HISTORY_RESPONSE_MAX - 1U);
        length = le_voice_history_serialize_entry(&history, stored->id,
                                                  buffer, sizeof(buffer));
        CHECK(length > 0 && (size_t)length < SERIAL_BUFFER);
        CHECK(strstr(buffer, "\"transcript_truncated\":true") != NULL);
        CHECK(strstr(buffer, "\"response_truncated\":true") != NULL);
    }

    /* ---- escaping: quotes, backslash, newline and tab survive JSON-safe ---- */
    {
        struct le_voice_turn_record record;

        record = make_record(
            "t-escape", LE_VOICE_TURN_COMPLETED,
            "He said \"hi\"\\then\nnew\ttab",
            "reply with \"quotes\" and \\slashes\\", 5, 6, 7);
        CHECK(le_voice_history_add(&history, generation, &record) == 1);
        length = le_voice_history_serialize(&history, buffer, sizeof(buffer));
        CHECK(length > 0 && (size_t)length < sizeof(buffer));
        CHECK(strstr(buffer, "\\\"hi\\\"") != NULL);
        CHECK(strstr(buffer, "\\nnew") != NULL);
        CHECK(strstr(buffer, "\\\"quotes\\\"") != NULL);
        length = le_voice_history_serialize_entry(
            &history, le_voice_history_at(&history, 0)->id,
            buffer, sizeof(buffer));
        CHECK(length > 0);
        CHECK(strstr(buffer, "\\\"quotes\\\"") != NULL);
        CHECK(strstr(buffer, "\\\\slashes\\\\") != NULL);
    }

    /* ---- worst case: 10 records whose visible text is all quotes fits ---- */
    {
        struct le_voice_history worst;
        struct le_voice_turn_record record;
        unsigned j;

        le_voice_history_init(&worst);
        generation = le_voice_history_generation(&worst);
        memset(long_text, '"', 700);
        long_text[700] = '\0';
        for (j = 0; j < LE_VOICE_HISTORY_CAPACITY; ++j) {
            char id[32];

            snprintf(id, sizeof(id), "worst-%u", j);
            record = make_record(id, LE_VOICE_TURN_ASSISTANT_FAILED,
                                 long_text, long_text,
                                 4294967295U, 4294967295U, 4294967295U);
            CHECK(le_voice_history_add(&worst, generation, &record) == 1);
        }
        length = le_voice_history_serialize(&worst, buffer, sizeof(buffer));
        CHECK(length > 0);
        CHECK((size_t)length < SERIAL_BUFFER);
        /* Seven full records of this worst case would already overflow if the
         * preview were unbounded. */
        CHECK(le_voice_history_count(&worst) == 10);
    }

    /* ---- clear scrubs immediately and stale completions cannot return ---- */
    generation = le_voice_history_generation(&history);
    {
        struct le_voice_turn_record stale =
            make_record("t-stale", LE_VOICE_TURN_COMPLETED, "too late", "no",
                        0, 0, 0);

        le_voice_history_clear(&history);
        cleared_generation = le_voice_history_generation(&history);
        CHECK(cleared_generation == generation + 1);
        CHECK(le_voice_history_count(&history) == 0);
        CHECK(le_voice_history_at(&history, 0) == NULL);
        length = le_voice_history_serialize(&history, buffer, sizeof(buffer));
        CHECK(length > 0 && (size_t)length < sizeof(buffer));
        CHECK(strstr(buffer, "\"count\":0") != NULL);
        CHECK(strstr(buffer, "\"turns\":[]") != NULL);
        /* A completion carrying the pre-clear generation is dropped. */
        CHECK(le_voice_history_add(&history, generation, &stale) == 0);
        CHECK(le_voice_history_count(&history) == 0);
        CHECK(le_voice_history_set_status(&history, generation, 1,
                                          LE_VOICE_TURN_TTS_FAILED,
                                          NULL) == 0);
        /* The current generation is accepted and starts a fresh ring. */
        CHECK(le_voice_history_add(&history, cleared_generation, &stale) == 1);
        CHECK(le_voice_history_count(&history) == 1);
        CHECK(le_voice_history_at(&history, 0)->id > 0);
    }

    /* ---- detail route bounds and lookup ---- */
    {
        uint64_t id = le_voice_history_at(&history, 0)->id;

        length = le_voice_history_serialize_entry(&history, id, buffer,
                                                  sizeof(buffer));
        CHECK(length > 0 && (size_t)length < SERIAL_BUFFER);
        CHECK(strstr(buffer, "\"transcript\":\"too late\"") != NULL);
        CHECK(le_voice_history_serialize_entry(&history, 987654, buffer,
                                               sizeof(buffer)) == -1);
        CHECK(le_voice_history_serialize_entry(&history, id, buffer,
                                               8U) == -2);
    }

    /* ---- UTF-8: no stored, preview or detail cut splits a codepoint ---- */
    {
        const char euro[] = "\xE2\x82\xAC"; /* U+20AC, three bytes */
        struct le_voice_history utf8_history;
        struct le_voice_turn_record record;
        unsigned long long utf8_generation;
        char stored_probe[LE_VOICE_HISTORY_TRANSCRIPT_MAX];
        int flagged = 0;
        size_t used;

        le_voice_history_init(&utf8_history);
        utf8_generation = le_voice_history_generation(&utf8_history);

        /* The stored transcript bound (767 usable bytes) lands inside the
         * first euro: it must be dropped whole, not split. */
        memset(long_text, 'a', 766);
        used = 766;
        memcpy(long_text + used, euro, 3); used += 3;
        memcpy(long_text + used, euro, 3); used += 3;
        long_text[used] = '\0';
        le_voice_history_copy_text(stored_probe, sizeof(stored_probe),
                                   long_text, &flagged);
        CHECK(flagged == 1);
        CHECK(utf8_valid(stored_probe));
        CHECK(strlen(stored_probe) == 766U);

        /* Invalid input policy: a stray continuation byte and a dangling lead
         * become a replacement character, so the result is always valid. */
        le_voice_history_copy_text(stored_probe, sizeof(stored_probe),
                                   "ab\x80" "cd", &flagged);
        CHECK(flagged == 0);
        CHECK(utf8_valid(stored_probe));
        CHECK(!strcmp(stored_probe, "ab?cd"));
        le_voice_history_copy_text(stored_probe, sizeof(stored_probe),
                                   "xy\xC3", &flagged);
        CHECK(flagged == 0);
        CHECK(utf8_valid(stored_probe));
        CHECK(!strcmp(stored_probe, "xy?"));

        /* 1023 ascii + euro straddles the 1024-byte detail bound; 23 ascii +
         * euro straddles the 24-byte collection preview. */
        memset(&record, 0, sizeof(record));
        snprintf(record.request_id, sizeof(record.request_id), "t-utf8");
        record.status = LE_VOICE_TURN_COMPLETED;
        memset(record.response, 'a', 1023);
        memcpy(record.response + 1023, euro, 3);
        record.response[1026] = '\0';
        memset(record.transcript, 'b', 23);
        memcpy(record.transcript + 23, euro, 3);
        record.transcript[26] = '\0';
        CHECK(le_voice_history_add(&utf8_history, utf8_generation,
                                   &record) == 1);
        {
            const struct le_voice_turn_record *stored =
                le_voice_history_at(&utf8_history, 0);

            CHECK(stored != NULL);
            CHECK(utf8_valid(stored->transcript));
            CHECK(utf8_valid(stored->response));
            CHECK(strlen(stored->response) == 1026U);
            length = le_voice_history_serialize(&utf8_history, buffer,
                                                sizeof(buffer));
            CHECK(length > 0 && (size_t)length < SERIAL_BUFFER);
            CHECK(utf8_valid(buffer));
            length = le_voice_history_serialize_entry(
                &utf8_history, stored->id, buffer, sizeof(buffer));
            CHECK(length > 0 && (size_t)length < SERIAL_BUFFER);
            CHECK(utf8_valid(buffer));
            CHECK(strstr(buffer, "\"response_truncated\":true") != NULL);
        }
    }

    /* ---- privacy: only fixed failure text is emitted, never the stored
     * provider-looking reason or any raw diagnostic. ---- */
    {
        struct le_voice_history private_history;
        struct le_voice_turn_record record;
        unsigned long long private_generation;

        le_voice_history_init(&private_history);
        private_generation = le_voice_history_generation(&private_history);
        memset(&record, 0, sizeof(record));
        snprintf(record.request_id, sizeof(record.request_id), "t-private");
        record.status = LE_VOICE_TURN_ASSISTANT_FAILED;
        snprintf(record.reason, sizeof(record.reason), "%s",
                 "upstream 500 token=SECRET payload");
        CHECK(le_voice_history_add(&private_history, private_generation,
                                   &record) == 1);
        length = le_voice_history_serialize(&private_history, buffer,
                                            sizeof(buffer));
        CHECK(length > 0);
        CHECK(strstr(buffer, "\"assistant failed\"") != NULL);
        CHECK(strstr(buffer, "SECRET") == NULL);
        CHECK(strstr(buffer, "upstream 500") == NULL);
        length = le_voice_history_serialize_entry(
            &private_history, le_voice_history_at(&private_history, 0)->id,
            buffer, sizeof(buffer));
        CHECK(length > 0);
        CHECK(strstr(buffer, "assistant failed") != NULL);
        CHECK(strstr(buffer, "SECRET") == NULL);
        CHECK(strstr(buffer, "token=") == NULL);
    }

    /* ---- no disk persistence: the unit wrote nothing in the cwd ---- */
    CHECK(directory_entries(directory) == 0);

    CHECK(chdir(previous_directory) == 0);
    CHECK(rmdir(directory) == 0);
    puts("voice history: bounded ring, outcomes, clearing and privacy: ok");
    return 0;

cleanup:
    if (chdir(previous_directory) != 0)
        result = 1;
    if (rmdir(directory) != 0)
        result = 1;
    return result;
}
