#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"

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

static unsigned count_occurrences(const char *text, const char *needle)
{
    unsigned count = 0;
    size_t length = strlen(needle);

    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += length;
    }
    return count;
}

static int write_history_fixture(const char *path)
{
    FILE *history = fopen(path, "w");
    unsigned i;

    if (!history)
        return -1;
    if (fprintf(history,
                "{\"version\":1,\"history_generation\":1,"
                "\"turns\":[") < 0) {
        fclose(history);
        return -1;
    }
    for (i = 0; i < 12; ++i) {
        if (fprintf(history,
                    "%s{\"at_ms\":%u,\"stt_audio_ms\":%u,"
                    "\"stt_processing_ms\":%u,\"stt_total_ms\":%u,"
                    "\"first_text_ms\":%u,\"first_announce_ms\":%u,"
                    "\"first_pcm_ms\":%u,\"follow_up\":%s,"
                    "\"request_id\":\"fixture-%u\"}",
                    i ? "," : "", 1000U + i, 10U + i, 20U + i,
                    30U + i, 40U + i, 50U + i, 60U + i,
                    i & 1U ? "true" : "false", i) < 0) {
            fclose(history);
            return -1;
        }
    }
    if (fprintf(history, "]}\n") < 0) {
        fclose(history);
        return -1;
    }
    if (fclose(history) != 0 || chmod(path, 0600) != 0)
        return -1;
    return 0;
}

static int clear_history_after_barrier(const char *socket_path, int barrier_fd)
{
    char release;
    char response[LE_ADAPTER_MSG_MAX];

    if (read(barrier_fd, &release, 1) != 1)
        return 1;
    return call(socket_path, "history_clear", NULL,
                response, sizeof(response)) == 0 ? 0 : 1;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-agentd-test-XXXXXX";
    char socket_path[256];
    char config_path[256];
    char history_path[384];
    char history_backup_path[400];
    char credentials_path[256];
    char capture_path[256];
    char audio_socket[256];
    char audio_capture[256];
    char wake_socket[256];
    char stt_socket[256];
    char voice_trigger[256];
    char first_pcm_path[256];
    char persisted_history[LE_ADAPTER_MSG_MAX];
    char response[LE_ADAPTER_MSG_MAX];
    struct stat status;
    struct timespec delay = {0, 10000000L};
    pid_t child = -1;
    pid_t audio_child = -1;
    pid_t stt_child = -1;
    pid_t source_child = -1;
    pid_t clear_children[2] = {-1, -1};
    int clear_barrier[2] = {-1, -1};
    size_t i;
    int result = 0;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(socket_path, sizeof(socket_path), "%s/agent.sock", directory);
    snprintf(config_path, sizeof(config_path), "%s/agent.json", directory);
    snprintf(history_path, sizeof(history_path),
             "%s.history-generation", config_path);
    snprintf(history_backup_path, sizeof(history_backup_path),
             "%s.bak", history_path);
    snprintf(credentials_path, sizeof(credentials_path),
             "%s/oauth.json", directory);
    snprintf(capture_path, sizeof(capture_path), "%s/curl.conf", directory);
    snprintf(audio_socket, sizeof(audio_socket),
             "%s/audio.sock", directory);
    snprintf(audio_capture, sizeof(audio_capture),
             "%s/audio.txt", directory);
    snprintf(wake_socket, sizeof(wake_socket),
             "%s/wake.sock", directory);
    snprintf(stt_socket, sizeof(stt_socket),
             "%s/stt.sock", directory);
    snprintf(voice_trigger, sizeof(voice_trigger),
             "%s/voice.trigger", directory);
    snprintf(first_pcm_path, sizeof(first_pcm_path),
             "%s/first-pcm", directory);
    CHECK(setenv("LE_TEST_CURL_CAPTURE", capture_path, 1) == 0);
    CHECK(unsetenv("LE_TEST_CURL_MODE") == 0);
    CHECK(setenv("LE_AGENT_AUTH_POLL_MIN_SECONDS", "0", 1) == 0);
    CHECK(setenv("LE_TEST_TTS_MARKER", first_pcm_path, 1) == 0);
    /* Force the marker after response generation creates the history record. */
    CHECK(setenv("LE_TEST_TTS_MARKER_DELAY_MS", "200", 1) == 0);
    audio_child = fork();
    CHECK(audio_child >= 0);
    if (audio_child == 0) {
        execl("./build/mock-audio-adapter",
              "./build/mock-audio-adapter",
              audio_socket, audio_capture, (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(audio_socket, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(audio_socket, F_OK) == 0);
    stt_child = fork();
    CHECK(stt_child >= 0);
    if (stt_child == 0) {
        execl("./build/libreecho-sttd", "./build/libreecho-sttd",
              "--socket", stt_socket, "--model-dir", "mock",
              "--threads", "2", (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(stt_socket, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(stt_socket, F_OK) == 0);
    source_child = fork();
    CHECK(source_child >= 0);
    if (source_child == 0) {
        execl("./build/mock-voice-source",
              "./build/mock-voice-source",
              wake_socket, voice_trigger, (char *)NULL);
        _exit(127);
    }
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl("./build/libreecho-agentd", "./build/libreecho-agentd",
              "--socket", socket_path,
              "--config", config_path,
              "--credentials", credentials_path,
              "--curl", "./build/mock-llm-curl",
              "--audio-socket", audio_socket,
              "--tts-socket", audio_socket,
              "--tts-first-pcm-file", first_pcm_path,
              "--wake-socket", wake_socket,
              "--stt-socket", stt_socket,
              (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(socket_path, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(socket_path, F_OK) == 0);
    CHECK(call(socket_path, "status", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"authenticated\":false") != NULL);
    CHECK(strstr(response, "\"latency_target_ms\":3000") != NULL);
    CHECK(call(socket_path, "auth_start", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"user_code\":\"TEST-CODE\"") != NULL);
    CHECK(strstr(response,
                 "\"verification_url\":\"https://auth.openai.com/"
                 "codex/device\"") != NULL);
    CHECK(call(socket_path, "auth_poll", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"authenticated\":true") != NULL);
    CHECK(stat(credentials_path, &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(call(
              socket_path, "configure",
              "{\"enabled\":true,\"model\":\"gpt-5.4\","
              "\"prompt\":\"Answer briefly for spoken playback.\"}",
              response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"enabled\":true") != NULL);
    CHECK(strstr(response,
                 "\"prompt\":\"Answer briefly for spoken playback.\"") != NULL);
    CHECK(call(socket_path, "respond",
               "{\"text\":\"Say hello.\"}",
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"text\":\"Hello\"") != NULL);
    for (i = 0; i < 300 && access(audio_capture, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(audio_capture, F_OK) == 0);
    {
        FILE *capture = fopen(audio_capture, "r");
        char spoken[512];

        CHECK(capture != NULL);
        CHECK(fgets(spoken, sizeof(spoken), capture) != NULL);
        fclose(capture);
        CHECK(strstr(spoken, "\"text\":\"Hello\"") != NULL);
    }
    {
        FILE *trigger = fopen(voice_trigger, "w");

        CHECK(trigger != NULL);
        CHECK(fputs("go\n", trigger) >= 0);
        CHECK(fclose(trigger) == 0);
    }
    for (i = 0; i < 500; ++i) {
        FILE *capture = fopen(audio_capture, "r");
        char spoken[512];
        unsigned int lines = 0;

        if (capture) {
            while (fgets(spoken, sizeof(spoken), capture))
                ++lines;
            fclose(capture);
        }
        if (lines >= 3)
            break;
        nanosleep(&delay, NULL);
    }
    CHECK(i < 500);
    CHECK(call(socket_path, "status", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"voice_pipeline\":true") != NULL);
    CHECK(strstr(response, "\"wake_events\":1") != NULL);
    CHECK(strstr(response, "\"follow_up_listens\":1") != NULL);
    CHECK(strstr(response, "\"completed_transcripts\":2") != NULL);
    CHECK(strstr(response,
                 "\"last_speech_end_to_first_pcm_ms\":") != NULL);
    CHECK(strstr(response, "\"last_stt_total_ms\":") != NULL);
    CHECK(strstr(response, "\"latency_target_met\":") != NULL);
    CHECK(strstr(response, "\"latency_violations\":") != NULL);
    CHECK(call(socket_path, "history", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"turns\":[]") == NULL);
    CHECK(strstr(response, "\"stt_total_ms\":") != NULL);
    CHECK(stat(history_path, &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    for (i = 0; i < 500; ++i) {
        FILE *history = fopen(history_path, "r");
        size_t length = 0;

        if (history) {
            int read_error;

            length = fread(persisted_history, 1,
                           sizeof(persisted_history) - 1, history);
            read_error = ferror(history);
            if (fclose(history) == 0 && !read_error &&
                length > 0 && length < sizeof(persisted_history) - 1) {
                persisted_history[length] = '\0';
                if (strstr(persisted_history, "\"request_id\":\"") != NULL &&
                    strstr(persisted_history, "\"first_pcm_ms\":0") == NULL)
                    break;
            }
        }
        nanosleep(&delay, NULL);
    }
    CHECK(i < 500);
    CHECK(count_occurrences(response, "\"at_ms\":") == 2);
    CHECK(count_occurrences(response, "\"stt_audio_ms\":") == 2);
    CHECK(strstr(response, "\"follow_up\":true") != NULL);
    CHECK(strstr(response, "\"follow_up\":false") != NULL);
    kill(child, SIGTERM);
    CHECK(waitpid(child, NULL, 0) == child);
    child = -1;
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl("./build/libreecho-agentd", "./build/libreecho-agentd",
              "--socket", socket_path,
              "--config", config_path,
              "--credentials", credentials_path,
              "--curl", "./build/mock-llm-curl",
              "--audio-socket", audio_socket,
              "--tts-socket", audio_socket,
              "--tts-first-pcm-file", first_pcm_path,
              "--wake-socket", wake_socket,
              "--stt-socket", stt_socket,
              (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(socket_path, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(socket_path, F_OK) == 0);
    CHECK(call(socket_path, "history", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"turns\":[]") == NULL);
    CHECK(strstr(response, "\"stt_total_ms\":") != NULL);
    CHECK(strstr(response, "\"first_pcm_ms\":0") == NULL);
    CHECK(count_occurrences(response, "\"at_ms\":") == 2);
    CHECK(count_occurrences(response, "\"stt_audio_ms\":") == 2);
    CHECK(strstr(response, "\"follow_up\":true") != NULL);
    CHECK(strstr(response, "\"follow_up\":false") != NULL);
    CHECK(write_history_fixture(history_path) == 0);
    CHECK(rename(history_path, history_backup_path) == 0);
    {
        FILE *history = fopen(history_path, "w");
        CHECK(history != NULL);
        CHECK(fputs("{corrupt", history) >= 0);
        CHECK(fclose(history) == 0);
    }
    kill(child, SIGTERM);
    CHECK(waitpid(child, NULL, 0) == child);
    child = -1;
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl("./build/libreecho-agentd", "./build/libreecho-agentd",
              "--socket", socket_path,
              "--config", config_path,
              "--credentials", credentials_path,
              "--curl", "./build/mock-llm-curl",
              "--audio-socket", audio_socket,
              "--tts-socket", audio_socket,
              "--tts-first-pcm-file", first_pcm_path,
              "--wake-socket", wake_socket,
              "--stt-socket", stt_socket,
              (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(socket_path, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(socket_path, F_OK) == 0);
    CHECK(call(socket_path, "history", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"history_generation\":1") != NULL);
    CHECK(count_occurrences(response, "\"at_ms\":") == 12);
    CHECK(count_occurrences(response, "\"stt_audio_ms\":") == 12);
    CHECK(strstr(response, "\"at_ms\":1011") != NULL);
    CHECK(strstr(response, "\"at_ms\":1000") != NULL);
    CHECK(strstr(response, "\"follow_up\":true") != NULL);
    CHECK(strstr(response, "\"follow_up\":false") != NULL);
    puts("agentd: bounded turn history survives restart: ok");
    CHECK(call(
              socket_path, "configure",
              "{\"provider\":\"openai-compatible\",\"enabled\":true,"
              "\"base_url\":\"http://198.51.100.20:8001/v1\","
              "\"model\":\"Gemma-4-12B\"}",
              response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"provider\":\"openai-compatible\"") != NULL);
    CHECK(strstr(response,
                 "\"base_url\":\"http://198.51.100.20:8001/v1\"") != NULL);
    CHECK(call(socket_path, "respond",
               "{\"text\":\"Check the local provider.\"}",
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"text\":\"Local ready\"") != NULL);
    CHECK(call(socket_path, "logout", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"authenticated\":false") != NULL);
    CHECK(access(credentials_path, F_OK) != 0);
    CHECK(pipe(clear_barrier) == 0);
    for (i = 0; i < 2; ++i) {
        clear_children[i] = fork();
        CHECK(clear_children[i] >= 0);
        if (clear_children[i] == 0) {
            close(clear_barrier[1]);
            _exit(clear_history_after_barrier(socket_path, clear_barrier[0]));
        }
    }
    close(clear_barrier[0]);
    clear_barrier[0] = -1;
    CHECK(write(clear_barrier[1], "xx", 2) == 2);
    close(clear_barrier[1]);
    clear_barrier[1] = -1;
    for (i = 0; i < 2; ++i) {
        int clear_status;
        CHECK(waitpid(clear_children[i], &clear_status, 0) ==
              clear_children[i]);
        clear_children[i] = -1;
        CHECK(WIFEXITED(clear_status) && WEXITSTATUS(clear_status) == 0);
    }
    CHECK(call(socket_path, "history", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"history_generation\":3") != NULL);
    CHECK(strstr(response, "\"turns\":[]") != NULL);
    puts("agentd: concurrent history clears serialize generation and ring reset: ok");
    puts("agentd: device auth, private token store and configuration: ok");

cleanup:
    if (clear_barrier[0] >= 0)
        close(clear_barrier[0]);
    if (clear_barrier[1] >= 0)
        close(clear_barrier[1]);
    for (i = 0; i < 2; ++i) {
        if (clear_children[i] > 0) {
            kill(clear_children[i], SIGTERM);
            waitpid(clear_children[i], NULL, 0);
        }
    }
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    if (audio_child > 0) {
        kill(audio_child, SIGTERM);
        waitpid(audio_child, NULL, 0);
    }
    if (source_child > 0) {
        kill(source_child, SIGTERM);
        waitpid(source_child, NULL, 0);
    }
    if (stt_child > 0) {
        kill(stt_child, SIGTERM);
        waitpid(stt_child, NULL, 0);
    }
    unlink(socket_path);
    unlink(config_path);
    unlink(history_path);
    unlink(history_backup_path);
    unlink(credentials_path);
    unlink(capture_path);
    unlink(audio_socket);
    unlink(audio_capture);
    unlink(wake_socket);
    unlink(stt_socket);
    unlink(voice_trigger);
    unlink(first_pcm_path);
    rmdir(directory);
    return result;
}
