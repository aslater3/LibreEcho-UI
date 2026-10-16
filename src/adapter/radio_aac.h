/*
 * Fixed-point AAC decode for the internet radio player.
 *
 * Thin wrapper over the vendored Helix HE-AAC decoder in third-party/helix-aac.
 * This is the only LibreEcho file that includes the Helix public API
 * (aacdec.h); radiod.c talks to the decoder only through these functions, and
 * the vendored headers never leak into the rest of the tree.
 *
 * Frame contract: matches mp3_take_frame in radiod.c. The decoder consumes
 * whole ADTS frames out of the caller's fixed input buffer and never discards
 * an incomplete trailing frame, so a server that delivers writes smaller than
 * one frame still produces audio (the bug the MP3 path already fixed).
 */
#ifndef LIBREECHO_RADIO_AAC_H
#define LIBREECHO_RADIO_AAC_H

#include <stddef.h>

/*
 * Interleaved output capacity for one decoded frame: 2 channels x 2048
 * samples, the SBR (HE-AAC) worst case. Plain AAC-LC frames use half of it.
 */
#define LE_RADIO_AAC_MAX_SAMPLES 4096

/* Open (or re-open) the decoder, resetting all state. 0 ok, -1 fail. */
int le_radio_aac_open(void);

/* Release the handle. Safe to call when not open. */
void le_radio_aac_close(void);

/*
 * Decode one frame from the fixed input buffer. Returns the number of
 * interleaved PCM samples (>0) and removes that frame; 0 when a complete
 * frame was rejected and dropped; -1 when more input is needed (nothing is
 * removed); -2 when the buffer is full with no decodable frame.
 *
 * The stream is resynchronised with the ADTS sync word first: bytes before
 * the sync are discarded. A whole frame is confirmed to be buffered before
 * the decoder is called, so the trailing incomplete frame is always retained.
 * *channels and *rate receive the decoded frame's channel count and output
 * sample rate. pcm must hold LE_RADIO_AAC_MAX_SAMPLES samples. Frames with
 * more than 2 channels are rejected (return 0).
 */
int le_radio_aac_take_frame(unsigned char *in, size_t *filled, size_t capacity,
                            short *pcm, int *channels, int *rate);

/* 1 if p starts with an ADTS sync word (0xFFF) and layer 0; else 0. */
int le_radio_aac_is_adts(const unsigned char *p, size_t n);

#endif /* LIBREECHO_RADIO_AAC_H */
