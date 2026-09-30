/*
 * Ogg Opus decode for the internet-radio / local-file player.
 *
 * Thin wrapper over the pinned libopusfile + libopus + libogg (see the
 * Platform build helper tools/mt8163-arm32/ui/build_opus.sh).  radiod talks to
 * the decoder only through these functions, and the libopusfile headers never
 * leak into the rest of the tree.
 *
 * Why Opus is special-cased rather than fed to minimp3: Opus is natively
 * 48 kHz and stereo, exactly the media bus rate, so a decoded block is written
 * straight to the bus with no resampler and no format negotiation.  libopusfile
 * also does the things a decoder must not get wrong here: it parses OpusHead
 * (so a plain Ogg Vorbis file is rejected as "not Opus" rather than fed in), it
 * reports the real channel count (mono upmixes to stereo), and it walks chained
 * Opus streams link by link.
 *
 * Build gating: the decode path is compiled only when LE_RADIOD_ENABLE_OPUS is
 * defined (the image build passes it together with the three static archives).
 * Without it the functions are honest stubs -- le_radio_opus_available()
 * returns 0 and le_radio_opus_play_fd() refuses -- so a build that cannot
 * decode Opus says so instead of staying silent.  The default `make` does not
 * set the macro, so it needs neither the headers nor the libraries.
 */
#ifndef LIBREECHO_RADIO_OPUS_H
#define LIBREECHO_RADIO_OPUS_H

#include <stddef.h>

/* Ogg capture pattern; the media-bus container sniff radiod applies first. */
static inline int le_radio_opus_is_ogg(const unsigned char *p, size_t n)
{
    return n >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S';
}

/* 1 when this build can decode Ogg Opus; 0 for the honest stub. */
int le_radio_opus_available(void);

/*
 * Decode a seekable Ogg Opus file to the 48 kHz stereo bus.
 *
 * file_fd is opened by the caller (radiod's confined USB open) and is rewound
 * here; bus_fd is the media bus.  On success the decoded frames are counted in
 * *played (when non-NULL) and *complete is set to 1 only when the whole stream
 * reached its end (a truncated or malformed file never claims completion).
 *
 * Returns 0 on a clean decode to EOF, and one of the negative codes below
 * otherwise; it never partially reports success for a bad file.
 */
#define LE_RADIO_OPUS_OK 0
#define LE_RADIO_OPUS_UNSUPPORTED (-1)   /* not compiled in */
#define LE_RADIO_OPUS_NOT_OPUS (-2)      /* not Ogg Opus / bad header / bad link */
#define LE_RADIO_OPUS_IO (-3)            /* read or seek failed */
#define LE_RADIO_OPUS_BUS (-4)           /* the media bus went away */
int le_radio_opus_play_fd(int bus_fd, int file_fd, long *played, int *complete);

#endif /* LIBREECHO_RADIO_OPUS_H */
