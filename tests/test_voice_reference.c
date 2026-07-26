#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_reference.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", \
                __FILE__, __LINE__, #x, errno); \
        result = 1; \
        goto out; \
    } \
} while (0)

struct test_packet {
    struct le_voice_reference_header header;
    int16_t samples[LE_VOICE_REFERENCE_MAX_PACKET_FRAMES];
};

static int send_packet(int fd, const char *path, uint32_t sequence)
{
    struct sockaddr_un address;
    struct test_packet packet;
    size_t bytes;
    size_t i;

    memset(&packet, 0, sizeof(packet));
    packet.header.magic = LE_VOICE_REFERENCE_MAGIC;
    packet.header.version = LE_VOICE_REFERENCE_VERSION;
    packet.header.header_bytes = sizeof(packet.header);
    packet.header.sequence = sequence;
    packet.header.sample_rate = LE_VOICE_REFERENCE_INPUT_RATE;
    packet.header.channels = 1;
    packet.header.frames = LE_VOICE_REFERENCE_MAX_PACKET_FRAMES;
    packet.header.activity_mask = 1;
    packet.header.monotonic_ns = 1000000000ULL;
    for (i = 0; i < packet.header.frames; ++i)
        packet.samples[i] = (int16_t)((i % 200U) - 100);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    bytes = sizeof(packet.header) +
            packet.header.frames * sizeof(packet.samples[0]);
    return sendto(fd, &packet, bytes, 0,
                  (struct sockaddr *)&address,
                  (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                              strlen(path) + 1U)) == (ssize_t)bytes
        ? 0 : -1;
}

int main(void)
{
    char root[] = "/tmp/libreecho-voice-reference.XXXXXX";
    char path[108];
    struct le_voice_reference reference;
    int16_t output[768];
    int sender = -1;
    int result = 0;
    size_t before;

    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/aec-reference.sock", root) > 0);
    CHECK(le_voice_reference_open(&reference, path) == 0);
    sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(sender >= 0);
    CHECK(send_packet(sender, path, 0) == 0);
    CHECK(le_voice_reference_drain(&reference) == 1);
    CHECK(reference.packets == 1);
    CHECK(reference.count >= 680 && reference.count <= 683);
    CHECK(le_voice_reference_active(&reference, 1100000000ULL,
                                    200000000ULL));
    CHECK(!le_voice_reference_active(&reference, 1300000000ULL,
                                     200000000ULL));
    before = reference.count;
    le_voice_reference_read(&reference, output, 160);
    CHECK(reference.count == before - 160);
    CHECK(reference.underflows == 0);

    CHECK(send_packet(sender, path, 2) == 0);
    CHECK(le_voice_reference_drain(&reference) == 1);
    CHECK(reference.discontinuities == 1);
    CHECK(reference.count >= 680 && reference.count <= 683);
    le_voice_reference_read(&reference, output, reference.count);
    le_voice_reference_read(&reference, output, 160);
    CHECK(reference.underflows == 1);
    puts("voice_reference: 48-to-16 kHz ring and discontinuity handling ok");

out:
    if (sender >= 0)
        close(sender);
    le_voice_reference_close(&reference);
    rmdir(root);
    return result;
}
