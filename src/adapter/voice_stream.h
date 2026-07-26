#ifndef LIBREECHO_VOICE_STREAM_H
#define LIBREECHO_VOICE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_STREAM_MAGIC 0x3153564cU /* "LVS1", little endian */
#define LE_VOICE_STREAM_VERSION 1U
#define LE_VOICE_STREAM_HEADER_BYTES 24U
#define LE_VOICE_STREAM_MAX_SAMPLES 1280U

struct le_voice_stream_frame {
    uint64_t first_sample;
    uint32_t sample_count;
    uint16_t flags;
    int16_t samples[LE_VOICE_STREAM_MAX_SAMPLES];
};

/*
 * Write one sample-indexed S16_LE mono frame. The socket should be
 * nonblocking when this is called from the real-time capture path.
 * Returns 0 only when the complete frame was accepted.
 */
int le_voice_stream_write_frame(int fd, uint64_t first_sample,
                                const int16_t *samples, size_t count,
                                uint16_t flags);

/*
 * Read one complete frame from a blocking descriptor.
 * Returns 1 for a frame, 0 for clean EOF, and -1 for I/O/protocol errors.
 */
int le_voice_stream_read_frame(int fd, struct le_voice_stream_frame *frame);

#endif
