#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"
#include "adapter/voice_stream.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static int accept_command(int listener, const char *expected,
                          const char *response)
{
    char request[LE_ADAPTER_MSG_MAX];
    char message[LE_ADAPTER_MSG_MAX];
    int client = le_adapter_accept(listener);
    int length;

    if (client < 0 ||
        read_line(client, request, sizeof(request)) < 0 ||
        !strstr(request, expected))
        return -1;
    length = le_adapter_respond_ok(
        message, sizeof(message), 1, response);
    if (length < 0 ||
        write_all(client, message, (size_t)length) < 0) {
        close(client);
        return -1;
    }
    return client;
}

int main(int argc, char **argv)
{
    struct timespec frame_delay = {0, 10000000L};
    char event[LE_ADAPTER_MSG_MAX];
    int16_t speech[160];
    int16_t silence[160];
    int listener;
    int wake;
    int audio;
    int length;
    unsigned int frame;

    if (argc != 2 && argc != 3)
        return 2;
    signal(SIGPIPE, SIG_IGN);
    memset(speech, 0, sizeof(speech));
    memset(silence, 0, sizeof(silence));
    for (frame = 0; frame < 160; ++frame)
        speech[frame] = 1200;
    listener = le_adapter_listen(argv[1]);
    if (listener < 0)
        return 1;
    wake = accept_command(
        listener, "\"cmd\":\"subscribe\"",
        "{\"subscribed\":true}");
    audio = accept_command(
        listener, "\"cmd\":\"stream_audio\"",
        "{\"streaming\":true,\"format\":\"pcm_s16_le\","
        "\"sample_rate\":16000,\"channels\":1,"
        "\"frame_header_bytes\":24,\"sample_indexed\":true}");
    if (wake < 0 || audio < 0)
        return 1;
    if (argc == 3) {
        struct timespec trigger_delay = {0, 10000000L};

        for (frame = 0;
             frame < 1000 && access(argv[2], F_OK) != 0;
             ++frame)
            nanosleep(&trigger_delay, NULL);
        if (access(argv[2], F_OK) != 0)
            return 1;
    }
    for (frame = 0; frame < 10; ++frame)
        if (le_voice_stream_write_frame(
                audio, (uint64_t)frame * 160U,
                speech, 160, 0) < 0)
            return 1;
    length = le_adapter_format_event(
        event, sizeof(event), "wake_detected",
        "{\"detection_sample\":640,\"score\":0.8,"
        "\"vad_score\":1.0,\"playback_active\":false,"
        "\"model\":\"mock\"}");
    if (length < 0 ||
        write_all(wake, event, (size_t)length) < 0)
        return 1;
    for (; frame < 24; ++frame) {
        if (le_voice_stream_write_frame(
                audio, (uint64_t)frame * 160U,
                speech, 160, 0) < 0)
            return 1;
        nanosleep(&frame_delay, NULL);
    }
    for (; frame < 44; ++frame) {
        if (le_voice_stream_write_frame(
                audio, (uint64_t)frame * 160U,
                silence, 160, 0) < 0)
            return 1;
        nanosleep(&frame_delay, NULL);
    }
    nanosleep(&frame_delay, NULL);
    close(audio);
    close(wake);
    close(listener);
    unlink(argv[1]);
    return 0;
}
