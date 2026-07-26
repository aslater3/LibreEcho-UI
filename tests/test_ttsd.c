#include "adapter/adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

int main(void)
{
    char directory[128];
    char socket_path[160];
    char fifo_path[160];
    struct le_adapter *adapter = NULL;
    struct timespec pause_time = {0, 20000000};
    char response[LE_ADAPTER_MSG_MAX];
    pid_t child = -1;
    int fifo_fd = -1;
    int result = 0;
    int i;

    snprintf(directory, sizeof(directory), "/tmp/libreecho-ttsd-test-%ld",
             (long)getpid());
    snprintf(socket_path, sizeof(socket_path), "%s/tts.sock", directory);
    snprintf(fifo_path, sizeof(fifo_path), "%s/announcement.pcm", directory);
    CHECK(mkdir(directory, 0700) == 0);
    CHECK(mkfifo(fifo_path, 0600) == 0);
  fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    CHECK(fifo_fd >= 0);

    /* Start ttsd in foreground mode with a custom socket path.
     * The announcement bus FIFO may not exist; the speech child will
     * fail to write but the protocol responses are still valid. */
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)setenv("LE_TTS_STREAMING", "1", 1);
        (void)setenv("LE_TTS_ANNOUNCEMENT_BUS", fifo_path, 1);
        execl("./build/libreecho-ttsd", "libreecho-ttsd",
              "--socket", socket_path, "--foreground", (char *)NULL);
        _exit(127);
    }

    /* Wait for the socket to appear. */
    for (i = 0; i < 50; ++i) {
        if (access(socket_path, F_OK) == 0)
            break;
        nanosleep(&pause_time, NULL);
    }
    CHECK(access(socket_path, F_OK) == 0);

    /* Connect as a client. */
    adapter = le_adapter_connect(socket_path, 2000);
    CHECK(adapter != NULL);

    /* 1. status should report idle (not speaking). */
    CHECK(le_adapter_call(adapter, "status", NULL,
                          response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"speaking\":false") != NULL);
    CHECK(strstr(response, "\"engine\":\"mock\"") != NULL);

    /* 2. speak with valid text should succeed (return 0, data has
     *    speaking:true). */
    CHECK(le_adapter_call(adapter, "speak",
                          "{\"text\":\"Now playing test song by test artist\"}",
                          response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"speaking\":true") != NULL);
    {
        unsigned char audio[4096];
        ssize_t audio_bytes = -1;

        for (i = 0; i < 100; ++i) {
            audio_bytes = read(fifo_fd, audio, sizeof(audio));
            if (audio_bytes > 0)
                break;
            CHECK(audio_bytes < 0 &&
                  (errno == EAGAIN || errno == EWOULDBLOCK));
            nanosleep(&pause_time, NULL);
        }
        CHECK(audio_bytes > 0);
    }

    /* 3. stop_speech should succeed. */
    CHECK(le_adapter_call(adapter, "stop_speech", NULL,
                          response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"speaking\":false") != NULL);

    /* 4. speak with empty text should be rejected (non-zero return). */
    CHECK(le_adapter_call(adapter, "speak", "{\"text\":\"\"}",
                          response, sizeof(response)) != 0);

    /* 5. speak with missing text field should be rejected. */
    CHECK(le_adapter_call(adapter, "speak", "{}",
                          response, sizeof(response)) != 0);

    /* 6. unknown command should be rejected. */
    CHECK(le_adapter_call(adapter, "bogus", NULL,
                          response, sizeof(response)) != 0);

    printf("ttsd: speak, stop, status and validation: ok\n");

cleanup:
    if (adapter)
        le_adapter_close(adapter);
    if (fifo_fd >= 0)
        close(fifo_fd);
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    unlink(socket_path);
    unlink(fifo_path);
    rmdir(directory);
    return result;
}
