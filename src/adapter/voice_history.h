#ifndef LIBREECHO_VOICE_HISTORY_H
#define LIBREECHO_VOICE_HISTORY_H

/*
 * Canonical, private recent voice-turn history.
 *
 * This unit is the single bounded store for the ten most recent voice turns
 * shown by the control centre.  It is deliberately RAM-only: the transcript
 * and the final human-facing reply live here and are never written through the
 * existing persisted latency history file.  Records carry a stable numeric id
 * and a stable ISO-8601 timestamp assigned once at insertion time.
 *
 * The status vocabulary is shared with the voice pipeline so a turn reported
 * by either layer is expressed the same way.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LE_VOICE_HISTORY_CAPACITY 10U
/* Canonical bounded text: matches the live pipeline's user/reply limits. */
#define LE_VOICE_HISTORY_TRANSCRIPT_MAX 768U
#define LE_VOICE_HISTORY_RESPONSE_MAX 1536U
#define LE_VOICE_HISTORY_TIMESTAMP_MAX 40U
#define LE_VOICE_HISTORY_REQUEST_ID_MAX 64U
#define LE_VOICE_HISTORY_REASON_MAX 48U
/*
 * Wire budget.  Ten records with full canonical text cannot fit a 4096-byte
 * adapter message, so the collection route exposes a short bounded preview
 * plus truncation metadata, and the per-record route returns the full bounded
 * text for one record at a time.  The preview is sized so the worst case
 * (every preview character needing a two-byte JSON escape) still fits.
 */
#define LE_VOICE_HISTORY_PREVIEW_CHARS 24U
#define LE_VOICE_HISTORY_DETAIL_TRANSCRIPT_CHARS 512U
#define LE_VOICE_HISTORY_DETAIL_RESPONSE_CHARS 1024U

enum le_voice_turn_status {
    LE_VOICE_TURN_NONE = 0,
    LE_VOICE_TURN_COMPLETED,
    LE_VOICE_TURN_STT_FAILED,
    LE_VOICE_TURN_ASSISTANT_FAILED,
    LE_VOICE_TURN_TTS_FAILED,
    LE_VOICE_TURN_CANCELLED,
    LE_VOICE_TURN_TIMED_OUT
};

struct le_voice_turn_record {
    uint64_t id;                                   /* stable, assigned on add */
    char request_id[LE_VOICE_HISTORY_REQUEST_ID_MAX];
    char timestamp[LE_VOICE_HISTORY_TIMESTAMP_MAX]; /* stable, ISO-8601 */
    char transcript[LE_VOICE_HISTORY_TRANSCRIPT_MAX];
    char response[LE_VOICE_HISTORY_RESPONSE_MAX];
    int transcript_truncated;
    int response_truncated;
    int status;
    char reason[LE_VOICE_HISTORY_REASON_MAX];      /* safe fixed text only */
    uint32_t stt_ms;
    uint32_t assistant_ms;
    uint32_t tts_ms;
};

struct le_voice_history {
    struct le_voice_turn_record records[LE_VOICE_HISTORY_CAPACITY];
    unsigned next;                 /* write cursor */
    unsigned count;                /* 0..capacity */
    uint64_t next_id;              /* next stable id */
    unsigned long long generation; /* bumped by clear */
};

/* Lifecycle. */
void le_voice_history_init(struct le_voice_history *history);

/* Current clear generation.  A turn captures this before it starts and passes
 * it back to le_voice_history_add() so a completion that lands after a clear
 * cannot resurrect the scrubbed turn. */
unsigned long long le_voice_history_generation(
    const struct le_voice_history *history);

/* Scrub every record immediately and bump the generation.  Idempotent. */
void le_voice_history_clear(struct le_voice_history *history);

/* Append a terminal turn.
 * Returns 1 when stored, 0 when 'generation' is stale (the turn predates the
 * most recent clear), -1 on an invalid status or malformed record.  The record
 * id is assigned here; the caller's id field is ignored.  Timestamp is taken
 * from the record when set, otherwise stamped from the wall clock. */
int le_voice_history_add(struct le_voice_history *history,
                         unsigned long long generation,
                         const struct le_voice_turn_record *record);

/* Upgrade the status of an already-stored record exactly once (e.g. a reply
 * that was later reported as a speech failure).  Only id/timestamp are
 * immutable. Returns 1 when updated, 0 when the id is unknown or stale. */
int le_voice_history_set_status(struct le_voice_history *history,
                                unsigned long long generation, uint64_t id,
                                int status, const char *reason);

unsigned le_voice_history_count(const struct le_voice_history *history);

/* Newest-first access: index 0 is the most recent record, NULL if absent. */
const struct le_voice_turn_record *le_voice_history_at(
    const struct le_voice_history *history, unsigned newest_index);
const struct le_voice_turn_record *le_voice_history_find(
    const struct le_voice_history *history, uint64_t id);

/* Text helpers (bounded, control-character safe). */
void le_voice_history_copy_text(char *destination, size_t size,
                                const char *source, int *truncated);
void le_voice_history_timestamp(char *out, size_t size, time_t when);

/* Status vocabulary. */
const char *le_voice_turn_status_name(int status);
/* Fixed, human-safe reason for a failed status, or NULL for completed/none.
 * Never carries provider diagnostics. */
const char *le_voice_turn_status_reason(int status);
int le_voice_turn_status_valid(int status);

/* Serialization.  Both return the response length (excluding NUL) on success.
 * The collection route never overflows: a length of -1 means the caller's
 * buffer was too small for even the bounded output. */
int le_voice_history_serialize(const struct le_voice_history *history,
                               char *out, size_t size);
/* -1 when the id is unknown, -2 when the buffer cannot hold the bounded
 * record. */
int le_voice_history_serialize_entry(const struct le_voice_history *history,
                                     uint64_t id, char *out, size_t size);

#endif
