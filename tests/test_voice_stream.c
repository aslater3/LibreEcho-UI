#include "adapter/voice_stream.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void)
{
    int sockets[2];
    int16_t samples[160];
    struct le_voice_stream_frame frame;
    size_t i;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = (int16_t)((int)i * 17 - 900);
    CHECK(le_voice_stream_write_frame(
              sockets[0], 987654321ULL, samples,
              sizeof(samples) / sizeof(samples[0]), 3) == 0);
    memset(&frame, 0, sizeof(frame));
    CHECK(le_voice_stream_read_frame(sockets[1], &frame) == 1);
    CHECK(frame.first_sample == 987654321ULL);
    CHECK(frame.sample_count == 160);
    CHECK(frame.flags == 3);
    CHECK(memcmp(frame.samples, samples, sizeof(samples)) == 0);
    close(sockets[0]);
    CHECK(le_voice_stream_read_frame(sockets[1], &frame) == 0);
    close(sockets[1]);
    puts("voice stream framing: ok");
    return 0;
}
