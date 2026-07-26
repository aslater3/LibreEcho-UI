#ifndef LIBREECHO_VOICE_PIPELINE_H
#define LIBREECHO_VOICE_PIPELINE_H

#include <stdint.h>

struct le_voice_pipeline;

struct le_voice_pipeline_turn {
    uint64_t detection_sample;
    uint64_t transcript_received_ms;
    uint64_t stt_audio_ms;
    uint64_t stt_processing_ms;
    int endpoint;
};

struct le_voice_pipeline_metrics {
    int running;
    int wake_connected;
    int audio_connected;
    int recognizing;
    int dispatching;
    unsigned long wake_events;
    unsigned long completed_transcripts;
    unsigned long dropped_turns;
    uint64_t last_stt_audio_ms;
    uint64_t last_stt_processing_ms;
};

typedef void (*le_voice_pipeline_transcript_fn)(
    void *context, const char *text,
    const struct le_voice_pipeline_turn *turn);

struct le_voice_pipeline *le_voice_pipeline_start(
    const char *wake_socket, const char *stt_socket,
    le_voice_pipeline_transcript_fn transcript, void *context);

void le_voice_pipeline_get_metrics(
    struct le_voice_pipeline *pipeline,
    struct le_voice_pipeline_metrics *metrics);

void le_voice_pipeline_stop(struct le_voice_pipeline *pipeline);

#endif
