/*
 * FLAC decode for radiod (TIDAL LOSSLESS streams), over the vendored dr_flac.
 *
 * The source is a forward-only byte stream supplied by a callback: radiod
 * never seeks a network stream, so dr_flac is given a seek that only skips
 * forward and simply carries on without a seek table. Output is interleaved
 * signed 16-bit PCM handed to a sink callback in blocks of at most
 * LE_RADIO_FLAC_BLOCK frames, at the stream's own rate; radiod resamples.
 */
#ifndef LIBREECHO_RADIO_FLAC_H
#define LIBREECHO_RADIO_FLAC_H

#include <stddef.h>

#define LE_RADIO_FLAC_BLOCK 1152      /* matches le_radio_resample's limit */

enum {
    LE_RADIO_FLAC_OK = 0,             /* decoded to the end of the stream */
    LE_RADIO_FLAC_NOT_FLAC = -1,      /* no fLaC marker / unreadable header */
    LE_RADIO_FLAC_UNSUPPORTED = -2,   /* more than two channels */
    LE_RADIO_FLAC_SINK = -3,          /* the sink refused audio */
    LE_RADIO_FLAC_SOURCE = -4         /* the source failed before the end */
};

/* Fill up to `want` bytes. Return the count, 0 at a clean end, <0 on error.
   Short reads are fine; the wrapper loops. */
typedef long (*le_radio_flac_read_fn)(void *ctx, unsigned char *dst,
                                      size_t want);
/* Take `frames` interleaved frames. Return 0, or <0 to stop decoding. */
typedef int (*le_radio_flac_sink_fn)(void *ctx, const short *pcm, int frames,
                                     int channels, int rate);

/* `frames_out`, when given, receives the PCM frames delivered. */
int le_radio_flac_play(le_radio_flac_read_fn read, void *read_ctx,
                       le_radio_flac_sink_fn sink, void *sink_ctx,
                       long long *frames_out);

/* The four-byte "fLaC" marker. */
int le_radio_flac_is_flac(const unsigned char *data, size_t length);

#endif
