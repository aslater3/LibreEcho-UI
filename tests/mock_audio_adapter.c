#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop_handler(int signal_number)
{
    (void)signal_number;
    running = 0;
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

static unsigned long long monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void write_first_pcm_marker(const char *args)
{
    const char *path = getenv("LE_TEST_TTS_MARKER");
    const char *key = strstr(args, "\"request_id\":\"");
    char request_id[64];
    size_t length = 0;
    FILE *marker;

    if (!path || !key)
        return;
    key += strlen("\"request_id\":\"");
    while (key[length] && key[length] != '"' &&
           length + 1 < sizeof(request_id)) {
        request_id[length] = key[length];
        ++length;
    }
    request_id[length] = '\0';
    if (!length || key[length] != '"')
        return;
    marker = fopen(path, "w");
    if (!marker)
        return;
    fprintf(marker, "%s %llu\n", request_id,
            monotonic_milliseconds());
    fclose(marker);
}

int main(int argc, char **argv)
{
    int listener;

    if (argc != 3)
        return 2;
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    listener = le_adapter_listen(argv[1]);
    if (listener < 0)
        return 1;
    while (running) {
        char message[LE_ADAPTER_MSG_MAX];
        char command[64];
        char response[LE_ADAPTER_MSG_MAX];
        char *args = NULL;
        unsigned long id = 0;
        int client = le_adapter_accept(listener);
        int length;

        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (read_line(client, message, sizeof(message)) == 0 &&
            le_adapter_parse_request(
                message, command, sizeof(command), &args, &id) == 0) {
            if (!strcmp(command, "speak")) {
                FILE *capture = fopen(argv[2], "a");

                if (capture) {
                    fprintf(capture, "%s\n", args);
                    fclose(capture);
                }
                write_first_pcm_marker(args);
                length = le_adapter_respond_ok(
                    response, sizeof(response), id,
                    "{\"speaking\":true}");
            } else if (!strcmp(command, "status")) {
                length = le_adapter_respond_ok(
                    response, sizeof(response), id,
                    "{\"speaking\":false,\"engine\":\"mock\"}");
            } else {
                length = le_adapter_respond_err(
                    response, sizeof(response), id, "unsupported");
            }
            if (length > 0)
                (void)write_all(client, response, (size_t)length);
        }
        close(client);
    }
    close(listener);
    unlink(argv[1]);
    return 0;
}
