#define _POSIX_C_SOURCE 200809L

#include "voice_pipeline.h"

#include "adapter.h"
#include "voice_stream.h"
#include "voice_listening_led.h"
#include "../json.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PCM_RING_SAMPLES (16000U * 2U)
#define TRANSCRIPT_MAX 4096U

struct le_voice_pipeline {
    pthread_t capture_thread;
    pthread_t dispatch_thread;
    pthread_mutex_t mutex;
    pthread_cond_t dispatch_ready;
    int running;
    char wake_socket[LE_ADAPTER_PATH_MAX];
    char stt_socket[LE_ADAPTER_PATH_MAX];
    le_voice_pipeline_transcript_fn transcript_callback;
    void *callback_context;
    int16_t pcm_ring[PCM_RING_SAMPLES];
    uint64_t ring_first_sample;
    size_t ring_count;
    char pending_transcript[TRANSCRIPT_MAX];
    struct le_voice_pipeline_turn pending_turn;
    int transcript_pending;
    int dispatching;
    struct le_voice_pipeline_metrics metrics;
};

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL +
           (uint64_t)now.tv_nsec / 1000000ULL;
}

static int pipeline_running(struct le_voice_pipeline *pipeline)
{
    int value;

    pthread_mutex_lock(&pipeline->mutex);
    value = pipeline->running;
    pthread_mutex_unlock(&pipeline->mutex);
    return value;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *position = buffer;

    while (size) {
        ssize_t count = write(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        ssize_t count = read(fd, buffer + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

static int connect_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;

    if (!path || strlen(path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int subscribe(const char *path, const char *command)
{
    char request[192];
    char response[LE_ADAPTER_MSG_MAX];
    int fd = connect_socket(path);
    int length;

    if (fd < 0)
        return -1;
    length = snprintf(
        request, sizeof(request),
        "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":{}}\n",
        command);
    if (length <= 0 || length >= (int)sizeof(request) ||
        write_all(fd, request, (size_t)length) < 0 ||
        read_line(fd, response, sizeof(response)) < 0 ||
        !strstr(response, "\"ok\":true")) {
        close(fd);
        return -1;
    }
    return fd;
}

static unsigned long long json_unsigned(const char *json,
                                        const char *key,
                                        int *found)
{
    char needle[96];
    const char *position;
    char *end;
    unsigned long long value;

    *found = 0;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >=
        (int)sizeof(needle))
        return 0;
    position = strstr(json, needle);
    if (!position || !(position = strchr(position + strlen(needle), ':')))
        return 0;
    errno = 0;
    value = strtoull(position + 1, &end, 10);
    if (errno || end == position + 1)
        return 0;
    *found = 1;
    return value;
}

static void ring_append(struct le_voice_pipeline *pipeline,
                        const struct le_voice_stream_frame *frame)
{
    uint64_t expected =
        pipeline->ring_first_sample + pipeline->ring_count;
    size_t i;

    if (!pipeline->ring_count ||
        frame->first_sample != expected) {
        pipeline->ring_first_sample = frame->first_sample;
        pipeline->ring_count = 0;
    }
    for (i = 0; i < frame->sample_count; ++i) {
        uint64_t sample_index;

        if (pipeline->ring_count == PCM_RING_SAMPLES) {
            ++pipeline->ring_first_sample;
            --pipeline->ring_count;
        }
        sample_index =
            pipeline->ring_first_sample + pipeline->ring_count;
        pipeline->pcm_ring[sample_index % PCM_RING_SAMPLES] =
            frame->samples[i];
        ++pipeline->ring_count;
    }
}

static int ring_write_from(struct le_voice_pipeline *pipeline, int fd,
                           uint64_t first_sample)
{
    int16_t buffer[1024];
    uint64_t available_first = pipeline->ring_first_sample;
    uint64_t available_end = available_first + pipeline->ring_count;

    if (first_sample < available_first)
        first_sample = available_first;
    while (first_sample < available_end) {
        size_t count = (size_t)(available_end - first_sample);
        size_t i;

        if (count > sizeof(buffer) / sizeof(buffer[0]))
            count = sizeof(buffer) / sizeof(buffer[0]);
        for (i = 0; i < count; ++i)
            buffer[i] = pipeline->pcm_ring[
                (first_sample + i) % PCM_RING_SAMPLES];
        if (write_all(fd, buffer, count * sizeof(buffer[0])) < 0)
            return -1;
        first_sample += count;
    }
    return 0;
}

static int queue_transcript(
    struct le_voice_pipeline *pipeline, const char *text,
    const struct le_voice_pipeline_turn *turn)
{
    int result = -1;

    pthread_mutex_lock(&pipeline->mutex);
    if (!pipeline->transcript_pending && !pipeline->dispatching &&
        strlen(text) < sizeof(pipeline->pending_transcript)) {
        strcpy(pipeline->pending_transcript, text);
        pipeline->pending_turn = *turn;
        pipeline->transcript_pending = 1;
        ++pipeline->metrics.completed_transcripts;
        pipeline->metrics.last_stt_audio_ms = turn->stt_audio_ms;
        pipeline->metrics.last_stt_processing_ms =
            turn->stt_processing_ms;
        pthread_cond_signal(&pipeline->dispatch_ready);
        result = 0;
    } else {
        ++pipeline->metrics.dropped_turns;
    }
    pthread_mutex_unlock(&pipeline->mutex);
    return result;
}

static int start_recognition(struct le_voice_pipeline *pipeline,
                             uint64_t detection_sample)
{
    int fd = subscribe(pipeline->stt_socket, "recognize_stream");

    if (fd < 0)
        return -1;
    if (ring_write_from(pipeline, fd, detection_sample) < 0) {
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->metrics.recognizing = 1;
    pthread_mutex_unlock(&pipeline->mutex);
    le_voice_listening_led_set(1);
    return fd;
}

static void close_recognition(struct le_voice_pipeline *pipeline,
                              int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->metrics.recognizing = 0;
    pthread_mutex_unlock(&pipeline->mutex);
    le_voice_listening_led_set(0);
}

static int handle_stt_line(struct le_voice_pipeline *pipeline,
                           int stt_fd, const char *line,
                           uint64_t detection_sample)
{
    struct le_voice_pipeline_turn turn;
    char text[TRANSCRIPT_MAX];
    int found;

    (void)stt_fd;
    if (!strstr(line, "\"event\":\"transcript\""))
        return 0;
    if (json_get_string(line, "text", text, sizeof(text)) < 1)
        return -1;
    memset(&turn, 0, sizeof(turn));
    turn.detection_sample = detection_sample;
    turn.transcript_received_ms = monotonic_milliseconds();
    turn.stt_audio_ms = json_unsigned(
        line, "audio_ms", &found);
    if (!found)
        turn.stt_audio_ms = 0;
    turn.stt_processing_ms = json_unsigned(
        line, "processing_ms", &found);
    if (!found)
        turn.stt_processing_ms = 0;
    turn.endpoint = strstr(line, "\"endpoint\":true") != NULL;
    if (text[0])
        (void)queue_transcript(pipeline, text, &turn);
    return 1;
}

static void set_connection_metric(struct le_voice_pipeline *pipeline,
                                  int wake, int connected)
{
    pthread_mutex_lock(&pipeline->mutex);
    if (wake)
        pipeline->metrics.wake_connected = connected;
    else
        pipeline->metrics.audio_connected = connected;
    pthread_mutex_unlock(&pipeline->mutex);
}

static void close_connection(struct le_voice_pipeline *pipeline,
                             int *fd, int wake)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
    set_connection_metric(pipeline, wake, 0);
}

static void *capture_worker(void *opaque)
{
    struct le_voice_pipeline *pipeline = opaque;
    int wake_fd = -1;
    int audio_fd = -1;
    int stt_fd = -1;
    uint64_t detection_sample = 0;

    while (pipeline_running(pipeline)) {
        struct pollfd descriptors[3];
        int poll_result;

        if (wake_fd < 0) {
            wake_fd = subscribe(pipeline->wake_socket, "subscribe");
            set_connection_metric(pipeline, 1, wake_fd >= 0);
        }
        if (audio_fd < 0) {
            audio_fd = subscribe(
                pipeline->wake_socket, "stream_audio");
            set_connection_metric(pipeline, 0, audio_fd >= 0);
        }
        if (wake_fd < 0 || audio_fd < 0) {
            struct timespec delay = {0, 250000000L};

            nanosleep(&delay, NULL);
            continue;
        }
        descriptors[0].fd = wake_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = audio_fd;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = stt_fd;
        descriptors[2].events = stt_fd >= 0 ? POLLIN : 0;
        descriptors[2].revents = 0;
        poll_result = poll(descriptors, stt_fd >= 0 ? 3 : 2, 250);
        if (poll_result < 0 && errno == EINTR)
            continue;
        if (poll_result < 0)
            break;
        if ((descriptors[0].revents &
             (POLLERR | POLLHUP | POLLNVAL)) &&
            !(descriptors[0].revents & POLLIN)) {
            close_connection(pipeline, &wake_fd, 1);
            continue;
        }
        if ((descriptors[1].revents &
             (POLLERR | POLLHUP | POLLNVAL)) &&
            !(descriptors[1].revents & POLLIN)) {
            close_connection(pipeline, &audio_fd, 0);
            continue;
        }
        if (stt_fd >= 0 &&
            (descriptors[2].revents &
             (POLLERR | POLLHUP | POLLNVAL)) &&
            !(descriptors[2].revents & POLLIN)) {
            close_recognition(pipeline, &stt_fd);
            continue;
        }
        if (descriptors[1].revents & POLLIN) {
            struct le_voice_stream_frame frame;

            if (le_voice_stream_read_frame(audio_fd, &frame) != 1) {
                close_connection(pipeline, &audio_fd, 0);
                continue;
            }
            ring_append(pipeline, &frame);
            if (stt_fd >= 0 &&
                frame.first_sample + frame.sample_count >
                    detection_sample) {
                size_t offset = frame.first_sample < detection_sample
                    ? (size_t)(detection_sample - frame.first_sample) : 0;

                if (offset < frame.sample_count &&
                    write_all(stt_fd, frame.samples + offset,
                              (frame.sample_count - offset) *
                                  sizeof(frame.samples[0])) < 0)
                    close_recognition(pipeline, &stt_fd);
            }
        }
        if (descriptors[0].revents & POLLIN) {
            char line[LE_ADAPTER_MSG_MAX];
            int found;
            uint64_t sample;

            if (read_line(wake_fd, line, sizeof(line)) < 0) {
                close_connection(pipeline, &wake_fd, 1);
                continue;
            }
            if (!strstr(line, "\"event\":\"wake_detected\""))
                continue;
            sample = json_unsigned(
                line, "detection_sample", &found);
            if (!found)
                continue;
            pthread_mutex_lock(&pipeline->mutex);
            ++pipeline->metrics.wake_events;
            if (pipeline->dispatching ||
                pipeline->transcript_pending || stt_fd >= 0) {
                ++pipeline->metrics.dropped_turns;
                pthread_mutex_unlock(&pipeline->mutex);
                continue;
            }
            pthread_mutex_unlock(&pipeline->mutex);
            detection_sample = sample;
            stt_fd = start_recognition(
                pipeline, detection_sample);
            if (stt_fd < 0) {
                pthread_mutex_lock(&pipeline->mutex);
                ++pipeline->metrics.dropped_turns;
                pthread_mutex_unlock(&pipeline->mutex);
            }
        }
        if (stt_fd >= 0 && descriptors[2].revents & POLLIN) {
            char line[LE_ADAPTER_MSG_MAX * 2U];
            int result;

            if (read_line(stt_fd, line, sizeof(line)) < 0) {
                close_recognition(pipeline, &stt_fd);
                continue;
            }
            result = handle_stt_line(
                pipeline, stt_fd, line, detection_sample);
            if (result != 0)
                close_recognition(pipeline, &stt_fd);
        }
    }
    close_connection(pipeline, &wake_fd, 1);
    close_connection(pipeline, &audio_fd, 0);
    close_recognition(pipeline, &stt_fd);
    return NULL;
}

static void *dispatch_worker(void *opaque)
{
    struct le_voice_pipeline *pipeline = opaque;

    pthread_mutex_lock(&pipeline->mutex);
    while (pipeline->running) {
        char text[TRANSCRIPT_MAX];
        struct le_voice_pipeline_turn turn;

        while (pipeline->running && !pipeline->transcript_pending)
            pthread_cond_wait(
                &pipeline->dispatch_ready, &pipeline->mutex);
        if (!pipeline->running)
            break;
        strcpy(text, pipeline->pending_transcript);
        turn = pipeline->pending_turn;
        pipeline->transcript_pending = 0;
        pipeline->dispatching = 1;
        pipeline->metrics.dispatching = 1;
        pthread_mutex_unlock(&pipeline->mutex);
        pipeline->transcript_callback(
            pipeline->callback_context, text, &turn);
        pthread_mutex_lock(&pipeline->mutex);
        pipeline->dispatching = 0;
        pipeline->metrics.dispatching = 0;
    }
    pthread_mutex_unlock(&pipeline->mutex);
    return NULL;
}

struct le_voice_pipeline *le_voice_pipeline_start(
    const char *wake_socket, const char *stt_socket,
    le_voice_pipeline_transcript_fn transcript, void *context)
{
    struct le_voice_pipeline *pipeline;

    if (!wake_socket || !stt_socket || !transcript ||
        strlen(wake_socket) >= LE_ADAPTER_PATH_MAX ||
        strlen(stt_socket) >= LE_ADAPTER_PATH_MAX)
        return NULL;
    pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline)
        return NULL;
    strcpy(pipeline->wake_socket, wake_socket);
    strcpy(pipeline->stt_socket, stt_socket);
    pipeline->transcript_callback = transcript;
    pipeline->callback_context = context;
    if (pthread_mutex_init(&pipeline->mutex, NULL) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&pipeline->dispatch_ready, NULL) != 0)
        goto fail_condition;
    pipeline->running = 1;
    pipeline->metrics.running = 1;
    if (pthread_create(
            &pipeline->dispatch_thread, NULL,
            dispatch_worker, pipeline) != 0)
        goto fail_threads;
    if (pthread_create(
            &pipeline->capture_thread, NULL,
            capture_worker, pipeline) != 0) {
        pthread_mutex_lock(&pipeline->mutex);
        pipeline->running = 0;
        pthread_cond_broadcast(&pipeline->dispatch_ready);
        pthread_mutex_unlock(&pipeline->mutex);
        pthread_join(pipeline->dispatch_thread, NULL);
        goto fail_threads;
    }
    return pipeline;

fail_threads:
    pthread_cond_destroy(&pipeline->dispatch_ready);
fail_condition:
    pthread_mutex_destroy(&pipeline->mutex);
fail_mutex:
    free(pipeline);
    return NULL;
}

void le_voice_pipeline_get_metrics(
    struct le_voice_pipeline *pipeline,
    struct le_voice_pipeline_metrics *metrics)
{
    if (!pipeline || !metrics)
        return;
    pthread_mutex_lock(&pipeline->mutex);
    *metrics = pipeline->metrics;
    pthread_mutex_unlock(&pipeline->mutex);
}

void le_voice_pipeline_stop(struct le_voice_pipeline *pipeline)
{
    if (!pipeline)
        return;
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->running = 0;
    pipeline->metrics.running = 0;
    pthread_cond_broadcast(&pipeline->dispatch_ready);
    pthread_mutex_unlock(&pipeline->mutex);
    pthread_join(pipeline->capture_thread, NULL);
    pthread_join(pipeline->dispatch_thread, NULL);
    pthread_cond_destroy(&pipeline->dispatch_ready);
    pthread_mutex_destroy(&pipeline->mutex);
    free(pipeline);
}
