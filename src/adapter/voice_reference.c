#define _POSIX_C_SOURCE 200809L

#include "voice_reference.h"

#include <speex/speex_resampler.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct reference_packet {
    struct le_voice_reference_header header;
    int16_t samples[LE_VOICE_REFERENCE_MAX_PACKET_FRAMES];
};

typedef char le_voice_reference_header_size_must_be_40[
    sizeof(struct le_voice_reference_header) == 40 ? 1 : -1];

static void ring_clear(struct le_voice_reference *reference)
{
    reference->head = 0;
    reference->count = 0;
}

static void ring_append(struct le_voice_reference *reference,
                        const int16_t *samples,
                        size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        size_t tail;

        if (reference->count == LE_VOICE_REFERENCE_RING_SAMPLES) {
            reference->head =
                (reference->head + 1) % LE_VOICE_REFERENCE_RING_SAMPLES;
            --reference->count;
            ++reference->overruns;
        }
        tail = (reference->head + reference->count) %
               LE_VOICE_REFERENCE_RING_SAMPLES;
        reference->ring[tail] = samples[i];
        ++reference->count;
    }
}

static int process_packet(struct le_voice_reference *reference,
                          const struct reference_packet *packet,
                          size_t packet_bytes)
{
    int16_t downsampled[768];
    spx_uint32_t input_frames;
    spx_uint32_t output_frames =
        (spx_uint32_t)(sizeof(downsampled) / sizeof(downsampled[0]));
    size_t expected_bytes;
    int rc;

    if (packet_bytes < sizeof(packet->header) ||
        packet->header.magic != LE_VOICE_REFERENCE_MAGIC ||
        packet->header.version != LE_VOICE_REFERENCE_VERSION ||
        packet->header.header_bytes != sizeof(packet->header) ||
        packet->header.sample_rate != LE_VOICE_REFERENCE_INPUT_RATE ||
        packet->header.channels != 1 ||
        packet->header.frames == 0 ||
        packet->header.frames > LE_VOICE_REFERENCE_MAX_PACKET_FRAMES) {
        ++reference->malformed_packets;
        return -1;
    }
    expected_bytes = sizeof(packet->header) +
                     packet->header.frames * sizeof(packet->samples[0]);
    if (packet_bytes != expected_bytes) {
        ++reference->malformed_packets;
        return -1;
    }
    if (reference->have_sequence &&
        packet->header.sequence != reference->expected_sequence) {
        ++reference->discontinuities;
        ring_clear(reference);
        speex_resampler_reset_mem(
            (SpeexResamplerState *)reference->resampler);
    }
    reference->have_sequence = 1;
    reference->expected_sequence = packet->header.sequence + 1U;
    reference->activity_mask = packet->header.activity_mask;
    reference->last_packet_ns = packet->header.monotonic_ns;

    input_frames = packet->header.frames;
    rc = speex_resampler_process_int(
        (SpeexResamplerState *)reference->resampler, 0,
        packet->samples, &input_frames, downsampled, &output_frames);
    if (rc != RESAMPLER_ERR_SUCCESS ||
        input_frames != packet->header.frames) {
        ++reference->malformed_packets;
        return -1;
    }
    ring_append(reference, downsampled, output_frames);
    ++reference->packets;
    return 0;
}

int le_voice_reference_open(struct le_voice_reference *reference,
                            const char *socket_path)
{
    struct sockaddr_un address;
    SpeexResamplerState *resampler;
    int resampler_error = RESAMPLER_ERR_SUCCESS;
    int receive_buffer = 32768;
    int flags;
    size_t path_length;

    if (!reference || !socket_path)
        return -1;
    path_length = strlen(socket_path);
    if (path_length == 0 ||
        path_length >= sizeof(reference->socket_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(reference, 0, sizeof(*reference));
    reference->fd = -1;
    strcpy(reference->socket_path, socket_path);
    resampler = speex_resampler_init(
        1, LE_VOICE_REFERENCE_INPUT_RATE,
        LE_VOICE_REFERENCE_OUTPUT_RATE, 3, &resampler_error);
    if (!resampler || resampler_error != RESAMPLER_ERR_SUCCESS)
        return -1;
    reference->resampler = resampler;
    reference->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (reference->fd < 0)
        goto fail;
    (void)setsockopt(reference->fd, SOL_SOCKET, SO_RCVBUF,
                     &receive_buffer, sizeof(receive_buffer));
    flags = fcntl(reference->fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(reference->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
    flags = fcntl(reference->fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(reference->fd, F_SETFD, flags | FD_CLOEXEC);

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);
    unlink(socket_path);
    if (bind(reference->fd, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         path_length + 1U)) < 0)
        goto fail;
    return 0;

fail:
    le_voice_reference_close(reference);
    return -1;
}

int le_voice_reference_fd(const struct le_voice_reference *reference)
{
    return reference ? reference->fd : -1;
}

int le_voice_reference_drain(struct le_voice_reference *reference)
{
    int processed = 0;

    if (!reference || reference->fd < 0)
        return -1;
    for (;;) {
        struct reference_packet packet;
        ssize_t received = recv(reference->fd, &packet, sizeof(packet),
                                MSG_DONTWAIT);

        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (received < 0)
            return -1;
        if (process_packet(reference, &packet, (size_t)received) == 0)
            ++processed;
    }
    return processed;
}

void le_voice_reference_read(struct le_voice_reference *reference,
                             int16_t *output,
                             size_t samples)
{
    size_t copied = 0;

    if (!reference || !output)
        return;
    while (copied < samples && reference->count > 0) {
        output[copied++] = reference->ring[reference->head];
        reference->head =
            (reference->head + 1) % LE_VOICE_REFERENCE_RING_SAMPLES;
        --reference->count;
    }
    if (copied < samples) {
        memset(output + copied, 0,
               (samples - copied) * sizeof(output[0]));
        ++reference->underflows;
    }
}

int le_voice_reference_active(const struct le_voice_reference *reference,
                              uint64_t now_ns,
                              uint64_t timeout_ns)
{
    return reference && reference->activity_mask != 0 &&
           reference->last_packet_ns != 0 &&
           now_ns >= reference->last_packet_ns &&
           now_ns - reference->last_packet_ns <= timeout_ns;
}

void le_voice_reference_close(struct le_voice_reference *reference)
{
    if (!reference)
        return;
    if (reference->fd >= 0)
        close(reference->fd);
    if (reference->socket_path[0] != '\0')
        unlink(reference->socket_path);
    if (reference->resampler)
        speex_resampler_destroy(
            (SpeexResamplerState *)reference->resampler);
    reference->fd = -1;
    reference->resampler = NULL;
}
