#define _POSIX_C_SOURCE 200809L

/*
 * Focused regression for the private voice-history clear/generation race.
 *
 * A turn captures the voice-history generation when its recognition begins.
 * If the owner clears the history while that recognition is still in flight,
 * the terminal record -- a transcript-bearing turn or a pre-transcript
 * outcome/failure -- must be dropped, not written into the freshly cleared
 * ring. Reading the generation only when the delayed terminal event arrives
 * (or when the transcript callback starts rather than when recognition
 * started) let a cleared in-flight turn reappear, which is a privacy bug.
 *
 * This drives the real agentd against a mock wake/STT pair so the test can
 * place the clear exactly between "recognition started" and the terminal
 * event, then asserts the cleared turn is absent from voice_history.
 *
 * Compile with:
 *   cc -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
 *      -Isrc tests/test_agentd_voice_history_race.c \
 *      src/adapter/adapter_client.c src/adapter/adapter_server.c src/log.c \
 *      -lpthread -o build/test-agentd-voice-history-race
 */

#include "adapter/adapter.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

static int call(const char *socket_path, const char *command,
                const char *args, char *response, size_t size)
{
    struct le_adapter *adapter =
        le_adapter_connect(socket_path, 3000);
    int result;

    if (!adapter)
        return -1;
    result = le_adapter_call(adapter, command, args, response, size);
    le_adapter_close(adapter);
    return result;
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
    length = le_adapter_respond_ok(message, sizeof(message), 1, response);
    if (length < 0 || write_all(client, message, (size_t)length) < 0) {
        close(client);
        return -1;
    }
    return client;
}

static void millisleep(unsigned milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
}

/*
 * Mock waked + sttd. Accepts the pipeline's subscriptions, fires one wake, and
 * waits for the recogniser subscription. Only then does it report "recognition
 * started" to the parent and block until the parent has cleared the history,
 * so the terminal event is guaranteed to land after the clear.
 *
 * mode 'o': close the recogniser with no transcript -> an STT_FAILED outcome.
 * mode 't': deliver a transcript -> a transcript-bearing turn.
 */
static int mock_server(char mode, const char *wake_path,
                       const char *stt_path, int ready_fd, int release_fd)
{
    char event[LE_ADAPTER_MSG_MAX];
    char data[512];
    char release;
    int wake_listener, stt_listener;
    int wake, audio, stt;
    int length;

    signal(SIGPIPE, SIG_IGN);
    wake_listener = le_adapter_listen(wake_path);
    if (wake_listener < 0)
        return 1;
    stt_listener = le_adapter_listen(stt_path);
    if (stt_listener < 0)
        return 1;
    wake = accept_command(wake_listener, "\"cmd\":\"subscribe\"",
                          "{\"subscribed\":true}");
    audio = accept_command(wake_listener, "\"cmd\":\"stream_audio\"",
                           "{\"streaming\":true,\"format\":\"pcm_s16_le\","
                           "\"sample_rate\":16000,\"channels\":1,"
                           "\"frame_header_bytes\":24,\"sample_indexed\":true}");
    if (wake < 0 || audio < 0)
        return 1;
    length = le_adapter_format_event(
        event, sizeof(event), "wake_detected",
        "{\"detection_sample\":640,\"score\":0.8,\"vad_score\":1.0,"
        "\"playback_active\":false,\"model\":\"mock\"}");
    if (length < 0 || write_all(wake, event, (size_t)length) < 0)
        return 1;
    stt = accept_command(stt_listener, "\"cmd\":\"recognize_stream\"",
                         "{\"streaming\":true,\"sample_rate\":16000,"
                         "\"channels\":1,\"format\":\"pcm_s16_le\","
                         "\"max_seconds\":20}");
    if (stt < 0)
        return 1;
    /* The turn has begun: agentd has captured its history generation. */
    if (write_all(ready_fd, "r", 1) < 0)
        return 1;
    close(ready_fd);
    if (read(release_fd, &release, 1) != 1)
        return 1;
    close(release_fd);
    if (mode == 't') {
        snprintf(data, sizeof(data),
                 "{\"text\":\"hello\",\"final\":true,\"endpoint\":true,"
                 "\"audio_ms\":200,\"processing_ms\":10,\"total_ms\":210}");
        length = le_adapter_format_event(event, sizeof(event), "transcript",
                                         data);
        if (length < 0 || write_all(stt, event, (size_t)length) < 0)
            return 1;
    } else {
        close(stt);
        stt = -1;
    }
    millisleep(400);
    if (stt >= 0)
        close(stt);
    close(wake);
    close(audio);
    close(wake_listener);
    close(stt_listener);
    unlink(wake_path);
    unlink(stt_path);
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-agentd-voice-race-XXXXXX";
    char socket_path[256];
    char config_path[256];
    char credentials_path[256];
    char capture_path[256];
    char audio_socket[256];
    char wake_socket[256];
    char stt_socket[256];
    char response[LE_ADAPTER_MSG_MAX];
    struct timespec delay = {0, 10000000L};
    const struct {
        char mode;
        int clear;              /* clear the history while the turn is in flight */
        const char *name;
    } cases[] = {
        { 'o', 1, "outcome" },
        { 't', 1, "transcript" },
        { 't', 0, "transcript without clear" }
    };
    pid_t mock_child = -1;
    pid_t agentd_child = -1;
    size_t i;
    unsigned m;
    int result = 0;

    signal(SIGPIPE, SIG_IGN);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(socket_path, sizeof(socket_path), "%s/agent.sock", directory);
    snprintf(config_path, sizeof(config_path), "%s/agent.json", directory);
    snprintf(credentials_path, sizeof(credentials_path),
             "%s/oauth.json", directory);
    snprintf(capture_path, sizeof(capture_path), "%s/curl.conf", directory);
    snprintf(audio_socket, sizeof(audio_socket),
             "%s/audio.sock", directory);
    snprintf(wake_socket, sizeof(wake_socket), "%s/wake.sock", directory);
    snprintf(stt_socket, sizeof(stt_socket), "%s/stt.sock", directory);
    CHECK(setenv("LE_TEST_CURL_CAPTURE", capture_path, 1) == 0);
    CHECK(setenv("LE_AGENT_AUTH_POLL_MIN_SECONDS", "0", 1) == 0);

    for (m = 0; m < sizeof(cases) / sizeof(cases[0]); ++m) {
        int ready_pipe[2] = {-1, -1};
        int release_pipe[2] = {-1, -1};
        char ready;

        unlink(socket_path);
        unlink(wake_socket);
        unlink(stt_socket);
        CHECK(pipe(ready_pipe) == 0);
        CHECK(pipe(release_pipe) == 0);
        mock_child = fork();
        CHECK(mock_child >= 0);
        if (mock_child == 0) {
            close(ready_pipe[0]);
            close(release_pipe[1]);
            _exit(mock_server(cases[m].mode, wake_socket, stt_socket,
                              ready_pipe[1], release_pipe[0]));
        }
        close(ready_pipe[1]);
        ready_pipe[1] = -1;
        close(release_pipe[0]);
        release_pipe[0] = -1;
        for (i = 0; i < 300 &&
             (access(wake_socket, F_OK) != 0 ||
              access(stt_socket, F_OK) != 0); ++i)
            nanosleep(&delay, NULL);
        CHECK(access(wake_socket, F_OK) == 0);
        CHECK(access(stt_socket, F_OK) == 0);

        agentd_child = fork();
        CHECK(agentd_child >= 0);
        if (agentd_child == 0) {
            execl("./build/libreecho-agentd", "./build/libreecho-agentd",
                  "--socket", socket_path,
                  "--config", config_path,
                  "--credentials", credentials_path,
                  "--curl", "./build/mock-llm-curl",
                  "--audio-socket", audio_socket,
                  "--tts-socket", audio_socket,
                  "--wake-socket", wake_socket,
                  "--stt-socket", stt_socket,
                  (char *)NULL);
            _exit(127);
        }
        for (i = 0; i < 300 && access(socket_path, F_OK) != 0; ++i)
            nanosleep(&delay, NULL);
        CHECK(access(socket_path, F_OK) == 0);

        /* Sign in and enable so a recognised transcript reaches the history. */
        CHECK(call(socket_path, "auth_start", NULL,
                   response, sizeof(response)) == 0);
        CHECK(call(socket_path, "auth_poll", NULL,
                   response, sizeof(response)) == 0);
        CHECK(strstr(response, "\"authenticated\":true") != NULL);
        CHECK(call(socket_path, "configure",
                   "{\"enabled\":true,\"model\":\"gpt-5.4\","
                   "\"prompt\":\"Answer briefly for spoken playback.\"}",
                   response, sizeof(response)) == 0);
        CHECK(strstr(response, "\"enabled\":true") != NULL);

        /* Wait for recognition to begin: the turn's generation is captured. */
        CHECK(read(ready_pipe[0], &ready, 1) == 1);
        close(ready_pipe[0]);
        ready_pipe[0] = -1;

        /* Owner clears the private voice history while the turn is in flight. */
        if (cases[m].clear) {
            CHECK(call(socket_path, "history_clear", NULL,
                       response, sizeof(response)) == 0);
        }

        /* Now let the turn deliver its terminal event. */
        CHECK(write(release_pipe[1], "g", 1) == 1);
        close(release_pipe[1]);
        release_pipe[1] = -1;
        millisleep(800);

        CHECK(call(socket_path, "voice_history", NULL,
                   response, sizeof(response)) == 0);
        if (cases[m].clear) {
            CHECK(strstr(response, "\"history_generation\":2") != NULL);
            /* The cleared in-flight turn must not reappear. */
            if (strstr(response, "\"turns\":[]") == NULL) {
                fprintf(stderr,
                        "check failed: cleared in-flight %s turn reappeared "
                        "in voice history: %s\n", cases[m].name, response);
                result = 1;
                goto cleanup;
            }
        } else {
            /*
             * Control: without a clear the same turn must be recorded, so a
             * fix that simply drops every record is not accepted.
             */
            CHECK(strstr(response, "\"history_generation\":1") != NULL);
            if (strstr(response, "\"transcript_preview\":\"hello\"") == NULL) {
                fprintf(stderr,
                        "check failed: uncleared %s turn missing from voice "
                        "history: %s\n", cases[m].name, response);
                result = 1;
                goto cleanup;
            }
        }

        kill(agentd_child, SIGTERM);
        CHECK(waitpid(agentd_child, NULL, 0) == agentd_child);
        agentd_child = -1;
        kill(mock_child, SIGTERM);
        waitpid(mock_child, NULL, 0);
        mock_child = -1;
    }

    puts("agentd voice history clear race: cleared in-flight turn stayed "
         "clear while an uncleared turn was kept");
    return 0;

cleanup:
    if (agentd_child > 0)
        kill(agentd_child, SIGKILL);
    if (mock_child > 0)
        kill(mock_child, SIGKILL);
    unlink(wake_socket);
    unlink(stt_socket);
    unlink(socket_path);
    if (rmdir(directory) != 0)
        result = 1;
    return result;
}
