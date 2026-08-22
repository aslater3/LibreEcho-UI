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

static int write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    if (!file)
        return -1;
    if (fputs(text, file) < 0 || fclose(file) != 0)
        return -1;
    return 0;
}

static unsigned long read_count(const char *path)
{
    FILE *file = fopen(path, "r");
    unsigned long count = 0;

    if (!file)
        return 0;
    if (fscanf(file, "%lu", &count) != 1)
        count = 0;
    fclose(file);
    return count;
}

static unsigned int line_count(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[512];
    unsigned int count = 0;

    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file))
        ++count;
    fclose(file);
    return count;
}

static int wait_for_lines(const char *path, unsigned int target)
{
    struct timespec delay = {0, 10000000L};
    unsigned int i;

    for (i = 0; i < 300; ++i) {
        if (line_count(path) >= target) {
            nanosleep(&delay, NULL);
            return 0;
        }
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int respond_and_wait(const char *socket_path, const char *args,
                            char *response, size_t response_size,
                            const char *audio_path,
                            unsigned int *expected_audio_lines)
{
    if (call(socket_path, "respond", args, response, response_size) != 0)
        return -1;
    ++*expected_audio_lines;
    return wait_for_lines(audio_path, *expected_audio_lines);
}

int main(void)
{
    char directory[] = "/tmp/libreecho-agentd-test-XXXXXX";
    char socket_path[256];
    char config_path[256];
    char credentials_path[256];
    char capture_path[256];
    char audio_socket[256];
    char audio_capture[256];
    char wake_socket[256];
    char stt_socket[256];
    char voice_trigger[256];
    char first_pcm_path[256];
    char weather_mode_path[256];
    char weather_count_path[256];
    char body_capture_path[256];
    char response[LE_ADAPTER_MSG_MAX];
    struct stat status;
    struct timespec delay = {0, 10000000L};
    pid_t child = -1;
    pid_t audio_child = -1;
    pid_t stt_child = -1;
    pid_t source_child = -1;
    size_t i;
    unsigned int expected_audio_lines;
    int result = 0;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(socket_path, sizeof(socket_path), "%s/agent.sock", directory);
    snprintf(config_path, sizeof(config_path), "%s/agent.json", directory);
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
    snprintf(weather_mode_path, sizeof(weather_mode_path),
             "%s/weather-mode", directory);
    snprintf(weather_count_path, sizeof(weather_count_path),
             "%s/weather-count", directory);
    snprintf(body_capture_path, sizeof(body_capture_path),
             "%s/body.txt", directory);
    CHECK(write_text(weather_count_path, "0\n") == 0);
    CHECK(write_text(body_capture_path, "") == 0);
    CHECK(setenv("LE_TEST_CURL_CAPTURE", capture_path, 1) == 0);
    CHECK(setenv("LE_TEST_CURL_WEATHER_MODE_FILE", weather_mode_path, 1) == 0);
    CHECK(setenv("LE_TEST_CURL_WEATHER_COUNT", weather_count_path, 1) == 0);
    CHECK(setenv("LE_TEST_CURL_BODY_CAPTURE", body_capture_path, 1) == 0);
    CHECK(unsetenv("LE_TEST_CURL_MODE") == 0);
    CHECK(setenv("LE_AGENT_AUTH_POLL_MIN_SECONDS", "0", 1) == 0);
    CHECK(setenv("LE_TEST_TTS_MARKER", first_pcm_path, 1) == 0);
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
    CHECK(write_text(weather_mode_path, "sse\n") == 0);
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
    expected_audio_lines = line_count(audio_capture);
    CHECK(call(socket_path, "status", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"voice_pipeline\":true") != NULL);
    CHECK(strstr(response, "\"wake_events\":1") != NULL);
    CHECK(strstr(response, "\"follow_up_listens\":1") != NULL);
    CHECK(strstr(response, "\"completed_transcripts\":2") != NULL);
    CHECK(strstr(response,
                 "\"last_speech_end_to_first_pcm_ms\":") != NULL);
    CHECK(strstr(response, "\"last_stt_total_ms\":") != NULL);
    CHECK(strstr(response, "\"latency_target_met\":true") != NULL);
    CHECK(strstr(response, "\"latency_violations\":0") != NULL);

    CHECK(write_text(weather_mode_path, "weather-open-meteo\n") == 0);
    CHECK(call(
              socket_path, "configure",
              "{\"weather_provider\":\"open-meteo\","
              "\"home_location\":\"Austin, Texas\","
              "\"latitude\":\"30.2672\",\"longitude\":\"-97.7431\"}",
              response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"weather_provider\":\"open-meteo\"") != NULL);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"What is the weather?\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(strstr(response, "\"text\":\"Hello\"") != NULL);
    CHECK(read_count(weather_count_path) == 1);
    {
        FILE *body_file = fopen(body_capture_path, "r");
        char request_body[4096];
        size_t body_size;

        CHECK(body_file != NULL);
        body_size = fread(request_body, 1, sizeof(request_body) - 1, body_file);
        fclose(body_file);
        request_body[body_size] = '\0';
        CHECK(strstr(request_body, "current weather there is") != NULL);
    }
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"What is the weather again?\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(read_count(weather_count_path) == 1);
    CHECK(call(
              socket_path, "configure",
              "{\"latitude\":\"31.0000\"}",
              response, sizeof(response)) == 0);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"How is the weather there?\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(read_count(weather_count_path) == 2);
    CHECK(call(socket_path, "configure",
               "{\"latitude\":\"nan\"}",
               response, sizeof(response)) != 0);
    CHECK(call(socket_path, "configure",
               "{\"longitude\":\"0x1p2\"}",
               response, sizeof(response)) != 0);

    CHECK(write_text(weather_mode_path, "weather-met-no-large\n") == 0);
    CHECK(call(socket_path, "configure",
               "{\"weather_provider\":\"met-no\"}",
               response, sizeof(response)) == 0);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"What is the weather in MET format?\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(strstr(response, "\"text\":\"Hello\"") != NULL);
    CHECK(read_count(weather_count_path) == 3);

    CHECK(write_text(weather_mode_path, "weather-open-meteo\n") == 0);
    CHECK(call(socket_path, "configure",
               "{\"weather_provider\":\"off\",\"latitude\":\"\","
               "\"longitude\":\"\"}",
               response, sizeof(response)) == 0);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"Is weather disabled?\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(read_count(weather_count_path) == 3);
    {
        FILE *body_file = fopen(body_capture_path, "r");
        char request_body[4096];
        size_t body_size;

        CHECK(body_file != NULL);
        body_size = fread(request_body, 1, sizeof(request_body) - 1, body_file);
        fclose(body_file);
        request_body[body_size] = '\0';
        CHECK(strstr(request_body, "current weather there is") == NULL);
    }

    CHECK(write_text(weather_mode_path, "weather-fail\n") == 0);
    CHECK(call(socket_path, "configure",
               "{\"weather_provider\":\"open-meteo\","
               "\"latitude\":\"30.2672\",\"longitude\":\"-97.7431\"}",
               response, sizeof(response)) == 0);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"Weather provider is down.\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(read_count(weather_count_path) == 4);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"Weather provider is still down.\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(read_count(weather_count_path) == 4);

    CHECK(write_text(weather_mode_path, "buffered\n") == 0);
    CHECK(call(
              socket_path, "configure",
              "{\"provider\":\"openai-compatible\",\"enabled\":true,"
              "\"base_url\":\"http://198.51.100.20:8001/v1\","
              "\"model\":\"Gemma-4-12B\"}",
              response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"provider\":\"openai-compatible\"") != NULL);
    CHECK(strstr(response,
                 "\"base_url\":\"http://198.51.100.20:8001/v1\"") != NULL);
    CHECK(respond_and_wait(
              socket_path, "{\"text\":\"Check the local provider.\"}",
              response, sizeof(response), audio_capture,
              &expected_audio_lines) == 0);
    CHECK(strstr(response, "\"text\":\"Local ready\"") != NULL);
    CHECK(call(socket_path, "logout", NULL,
               response, sizeof(response)) == 0);
    CHECK(strstr(response, "\"authenticated\":false") != NULL);
    CHECK(access(credentials_path, F_OK) != 0);
    puts("agentd: device auth, private token store and configuration: ok");

cleanup:
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
    unlink(credentials_path);
    unlink(capture_path);
    unlink(audio_socket);
    unlink(audio_capture);
    unlink(wake_socket);
    unlink(stt_socket);
    unlink(voice_trigger);
    unlink(first_pcm_path);
    unlink(weather_mode_path);
    unlink(weather_count_path);
    unlink(body_capture_path);
    rmdir(directory);
    return result;
}
