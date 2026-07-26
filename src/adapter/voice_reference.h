#ifndef LIBREECHO_VOICE_REFERENCE_H
#define LIBREECHO_VOICE_REFERENCE_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_REFERENCE_MAGIC 0x5241454cU
#define LE_VOICE_REFERENCE_VERSION 1U
#define LE_VOICE_REFERENCE_INPUT_RATE 48000U
#define LE_VOICE_REFERENCE_OUTPUT_RATE 16000U
#define LE_VOICE_REFERENCE_MAX_PACKET_FRAMES 2048U
#define LE_VOICE_REFERENCE_RING_SAMPLES 8192U

struct le_voice_reference_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t frames;
    uint32_t activity_mask;
    uint64_t render_sample;
    uint64_t monotonic_ns;
};

struct le_voice_reference {
    int fd;
    void *resampler;
    char socket_path[108];
    int16_t ring[LE_VOICE_REFERENCE_RING_SAMPLES];
    size_t head;
    size_t count;
    uint32_t expected_sequence;
    uint64_t last_packet_ns;
    unsigned int activity_mask;
    unsigned int packets;
    unsigned int malformed_packets;
    unsigned int discontinuities;
    unsigned int underflows;
    unsigned int overruns;
    int have_sequence;
};

int le_voice_reference_open(struct le_voice_reference *reference,
                            const char *socket_path);
int le_voice_reference_fd(const struct le_voice_reference *reference);
int le_voice_reference_drain(struct le_voice_reference *reference);
void le_voice_reference_read(struct le_voice_reference *reference,
                             int16_t *output,
                             size_t samples);
int le_voice_reference_active(const struct le_voice_reference *reference,
                              uint64_t now_ns,
                              uint64_t timeout_ns);
void le_voice_reference_close(struct le_voice_reference *reference);

#endif
