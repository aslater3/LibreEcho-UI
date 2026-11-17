#ifndef LIBREECHO_VOICE_CLIP_STORE_H
#define LIBREECHO_VOICE_CLIP_STORE_H

/*
 * Owner-enabled, bounded store for short voice audio clips (wake near misses).
 *
 * Clips are 16 kHz mono S16_LE WAV files, written under one directory with
 * mode 0600 through a temporary name and rename, so a reader never sees a
 * partial file. Nothing is kept unless the owner has switched capture on: the
 * settings file is absent by default and a missing or unreadable settings
 * file means capture is off.
 *
 * File names are "<kind>-<10-digit unix seconds>-<4-digit sequence>.wav". A
 * fixed-width name sorts chronologically, so the oldest clip is the smallest
 * name and eviction needs no timestamps.
 *
 * The directory defaults to the device path below. The environment
 * variable LIBREECHO_VOICE_CLIP_DIR overrides it; that is the test hook used
 * to exercise the store against /tmp on a device without a /data write.
 */

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_CLIP_DEFAULT_DIR "/data/libreecho/voice-clips"
#define LE_VOICE_CLIP_ENV "LIBREECHO_VOICE_CLIP_DIR"
#define LE_VOICE_CLIP_SAMPLE_RATE 16000U
/* Hard per-clip ceiling: 32768 samples is 64 KiB of PCM, which with the WAV
 * header still fits the 128 KiB API response body. */
#define LE_VOICE_CLIP_MAX_SAMPLES 32768U
#define LE_VOICE_CLIP_MAX_COUNT 32U
#define LE_VOICE_CLIP_MAX_BYTES (4U * 1024U * 1024U)
#define LE_VOICE_CLIP_ID_MAX 40U
#define LE_VOICE_CLIP_KIND_MAX 8U

struct le_voice_clip_settings {
    int near_miss_enabled;
};

struct le_voice_clip_entry {
    char id[LE_VOICE_CLIP_ID_MAX];
    char kind[LE_VOICE_CLIP_KIND_MAX];
    long long created;      /* unix seconds from the file name */
    unsigned long sequence; /* per-second sequence from the file name */
    unsigned long bytes;    /* total file size, WAV header included */
};

/* Upper bound on clips a single list or clear pass will examine. The writer
 * keeps the store at LE_VOICE_CLIP_MAX_COUNT, so this is only a guard against
 * stray files in the directory. */
#define LE_VOICE_CLIP_LIST_MAX 256U

/* Directory in use: LIBREECHO_VOICE_CLIP_DIR when set and non-empty, otherwise
 * the device default. Never NULL. */
const char *le_voice_clip_directory(void);

/* Settings. Load returns 0 on success, or on absence/corruption with the safe
 * defaults (capture off). Save writes atomically at mode 0600 and returns 0
 * on success, -1 on failure. */
int le_voice_clip_settings_load(const char *directory,
                                struct le_voice_clip_settings *settings);
int le_voice_clip_settings_save(const char *directory,
                                const struct le_voice_clip_settings *settings);

/* Validate a clip id: 1..LE_VOICE_CLIP_ID_MAX-1 bytes of [a-z0-9-], starting
 * with a letter, in the canonical "<kind>-<seconds>-<seq>" shape. */
int le_voice_clip_id_valid(const char *id);

/* Store one clip, evicting the oldest clips of any kind as needed so that the
 * store stays within LE_VOICE_CLIP_MAX_COUNT and LE_VOICE_CLIP_MAX_BYTES.
 * Returns 1 when stored, -1 on a bad argument, an oversize clip or I/O error.
 * Sample count is clamped to LE_VOICE_CLIP_MAX_SAMPLES by the caller's
 * choice; anything above it is rejected rather than truncated. */
int le_voice_clip_write(const char *directory, const char *kind,
                        const int16_t *samples, size_t count, long long now);

/* List up to `capacity` clips, oldest first. Returns the number written, or
 * -1 on error. A missing directory lists as empty. */
int le_voice_clip_list(const char *directory, struct le_voice_clip_entry *entries,
                       unsigned capacity, unsigned *total_bytes);

/* Resolve an id to its full path in `path`. Returns 0, or -1 for an invalid id
 * or a missing file. */
int le_voice_clip_path(const char *directory, const char *id,
                       char *path, size_t size);

/* Delete one clip. 1 when removed, 0 when absent, -1 on error or bad id. */
int le_voice_clip_delete(const char *directory, const char *id);

/* Delete every clip. Returns the number removed, or -1 on error. */
int le_voice_clip_clear(const char *directory);

/* Serialize a 44-byte RIFF/WAVE header for `count` mono S16 samples. */
void le_voice_clip_wav_header(unsigned char header[44], size_t count);

#endif
