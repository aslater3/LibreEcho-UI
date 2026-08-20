#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

static int connect_socket(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;

    while (size) {
        ssize_t count = write(fd, p, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        p += count;
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
            buffer[used] = '\0';
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-sttd-test-XXXXXX";
    char socket_path[256];
    char response[8192];
    struct le_adapter *adapter = NULL;
    pid_t child = -1;
    int fd = -1;
    int result = 0;
    int saw_partial = 0;
    int16_t pcm[6400];
    struct timespec delay = {0, 10000000L};
    size_t i;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(socket_path, sizeof(socket_path), "%s/stt.sock", directory);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl(
            "./build/libreecho-sttd", "./build/libreecho-sttd",
            "--socket", socket_path, "--model-dir", "mock",
            "--threads", "2", (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 200 && access(socket_path, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(socket_path, F_OK) == 0);
    adapter = le_adapter_connect(socket_path, 1000);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(
              adapter, "status", NULL,
              response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"streaming\":true") != NULL);
    CHECK(strstr(response, "\"engine\":\"mock\"") != NULL);
    le_adapter_close(adapter);
    adapter = NULL;

    fd = connect_socket(socket_path);
    CHECK(fd >= 0);
    CHECK(write_all(
              fd,
              "{\"v\":1,\"id\":7,\"cmd\":\"recognize_stream\","
              "\"args\":{}}\n",
              strlen(
                  "{\"v\":1,\"id\":7,\"cmd\":\"recognize_stream\","
                  "\"args\":{}}\n")) == 0);
    CHECK(read_line(fd, response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    for (i = 0; i < 3200; ++i)
        pcm[i] = 1200;
    for (; i < 6400; ++i)
        pcm[i] = 0;
    CHECK(write_all(fd, pcm, sizeof(pcm)) == 0);
    do {
        CHECK(read_line(fd, response, sizeof(response)) == 0);
        if (strstr(response, "\"event\":\"transcript_partial\""))
            saw_partial = 1;
    } while (strstr(response, "\"event\":\"transcript\"") == NULL);
    CHECK(saw_partial);
    CHECK(strstr(response, "\"text\":\"mock transcription\"") != NULL);
    CHECK(strstr(response, "\"endpoint\":true") != NULL);
    CHECK(strstr(response, "\"processing_ms\":") != NULL);
    CHECK(strstr(response, "\"total_ms\":") != NULL);
    puts("sttd: status, streaming PCM and endpoint transcript: ok");

cleanup:
    if (adapter)
        le_adapter_close(adapter);
    if (fd >= 0)
        close(fd);
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    unlink(socket_path);
    rmdir(directory);
    return result;
}
