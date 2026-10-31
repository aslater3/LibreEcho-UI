#ifndef LIBREECHO_SOUND_STORE_H
#define LIBREECHO_SOUND_STORE_H

/*
 * Owner-uploaded event sounds (#410): WAV validation and conversion, and the
 * small file store audiod plays from. See adapter/event_sound.h for the
 * events and the on-disk layout.
 */

#include "adapter/event_sound.h"

#include <stddef.h>
#include <stdint.h>

/* The largest WAV upload accepted: five seconds of 48 kHz 16-bit stereo plus
   generous room for header chunks. Fixed; not raisable by a client. */
#define LE_SOUND_UPLOAD_MAX_BYTES 1048576u

/* le_sound_wav_convert results. */
#define LE_SOUND_OK 0
#define LE_SOUND_NOT_WAV 1
#define LE_SOUND_UNSUPPORTED 2
#define LE_SOUND_EMPTY 3
#define LE_SOUND_TOO_LONG 4

/*
 * Parse a RIFF/WAVE upload and convert it to mono S16LE at 48 kHz.
 * Accepted: integer PCM (format 1, or WAVE_FORMAT_EXTENSIBLE carrying PCM),
 * 16-bit, one or two channels, 8000-48000 Hz. Stereo is averaged and other
 * rates are linearly resampled. The converted sound must be no longer than
 * max_ms, and out must hold at least LE_EVENT_SOUND_MAX_SAMPLES samples.
 */
int le_sound_wav_convert(const unsigned char *in, size_t length,
                         unsigned max_ms, int16_t *out, size_t *out_samples);
/* A short, stable reason for a le_sound_wav_convert result. */
const char *le_sound_result_message(int result);

/* Atomically store samples as the event's selected sound (mode 0600). */
int le_sound_store_write(const char *dir, int index, const int16_t *samples,
                         size_t count);
/* custom != 0 selects the owner's sound, 0 the built-in one. Returns 0, or
   1 when there is no uploaded sound to select, or -1 on I/O failure. */
int le_sound_store_select(const char *dir, int index, int custom);
/* Nonzero when the owner has uploaded a sound for the event (selected or kept). */
int le_sound_store_uploaded(const char *dir, int index);
/* Remove the owner's sound for an event, selected or kept. */
int le_sound_store_delete(const char *dir, int index);
/* {"sounds":[...],...} for the API; returns -1 when it does not fit. */
int le_sound_store_json(const char *dir, char *out, size_t size);

#endif
