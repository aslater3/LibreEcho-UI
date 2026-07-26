#define _POSIX_C_SOURCE 200809L

#include "voice_stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *p, uint64_t value)
{
    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char *p)
{
    return (uint64_t)get_u32(p) |
           ((uint64_t)get_u32(p + 4) << 32);
}

static int read_all(int fd, void *buffer, size_t size, int allow_eof)
{
    unsigned char *p = buffer;
    size_t used = 0;

    while (used < size) {
        ssize_t count = read(fd, p + used, size - used);

        if (count < 0 && errno == EINTR)
            continue;
        if (count == 0 && allow_eof && used == 0)
            return 0;
        if (count <= 0)
            return -1;
        used += (size_t)count;
    }
    return 1;
}

int le_voice_stream_write_frame(int fd, uint64_t first_sample,
                                const int16_t *samples, size_t count,
                                uint16_t flags)
{
    unsigned char packet[
        LE_VOICE_STREAM_HEADER_BYTES +
        LE_VOICE_STREAM_MAX_SAMPLES * sizeof(int16_t)];
    size_t payload_bytes;
    size_t packet_bytes;
    ssize_t written;

    if (fd < 0 || !samples || count == 0 ||
        count > LE_VOICE_STREAM_MAX_SAMPLES)
        return -1;
    payload_bytes = count * sizeof(int16_t);
    packet_bytes = LE_VOICE_STREAM_HEADER_BYTES + payload_bytes;
    put_u32(packet, LE_VOICE_STREAM_MAGIC);
    put_u16(packet + 4, LE_VOICE_STREAM_VERSION);
    put_u16(packet + 6, flags);
    put_u64(packet + 8, first_sample);
    put_u32(packet + 16, (uint32_t)count);
    put_u32(packet + 20, 0);
    memcpy(packet + LE_VOICE_STREAM_HEADER_BYTES,
           samples, payload_bytes);

    do {
        written = send(fd, packet, packet_bytes,
                       MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)packet_bytes ? 0 : -1;
}

int le_voice_stream_read_frame(int fd, struct le_voice_stream_frame *frame)
{
    unsigned char header[LE_VOICE_STREAM_HEADER_BYTES];
    uint32_t sample_count;
    int result;

    if (fd < 0 || !frame)
        return -1;
    result = read_all(fd, header, sizeof(header), 1);
    if (result <= 0)
        return result;
    if (get_u32(header) != LE_VOICE_STREAM_MAGIC ||
        get_u16(header + 4) != LE_VOICE_STREAM_VERSION ||
        get_u32(header + 20) != 0)
        return -1;
    sample_count = get_u32(header + 16);
    if (sample_count == 0 ||
        sample_count > LE_VOICE_STREAM_MAX_SAMPLES)
        return -1;
    frame->flags = get_u16(header + 6);
    frame->first_sample = get_u64(header + 8);
    frame->sample_count = sample_count;
    result = read_all(fd, frame->samples,
                      sample_count * sizeof(frame->samples[0]), 0);
    return result == 1 ? 1 : -1;
}
