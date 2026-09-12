#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"
#include "adapter/voice_listening_led.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct le_adapter {
    int audio;
};

static struct le_adapter audio_adapter = {1};
static struct le_adapter led_adapter = {0};
static int audio_available = 1;
static int audio_calls;
static int led_calls;
static char led_args[2][256];

struct le_adapter *le_adapter_connect(const char *path, int timeout_ms)
{
    (void)timeout_ms;
    if (!strcmp(path, LE_ADAPTER_AUDIO_SOCK))
        return audio_available ? &audio_adapter : NULL;
    if (!strcmp(path, LE_ADAPTER_LED_SOCK))
        return &led_adapter;
    return NULL;
}

void le_adapter_close(struct le_adapter *adapter)
{
    (void)adapter;
}

int le_adapter_call(struct le_adapter *adapter, const char *command,
                    const char *args, char *output, size_t output_size)
{
    (void)output;
    (void)output_size;
    if (adapter->audio) {
        ++audio_calls;
        if (strcmp(command, "wake_chirp"))
            return -1;
        return 0;
    }
    if (led_calls < 2)
        snprintf(led_args[led_calls], sizeof(led_args[led_calls]), "%s", args);
    ++led_calls;
    return strcmp(command, "pattern") ? -1 : 0;
}

static void reset_calls(void)
{
    audio_calls = 0;
    led_calls = 0;
    memset(led_args, 0, sizeof(led_args));
}

int main(void)
{
    reset_calls();
    le_voice_listening_led_set(1);
    CHECK(audio_calls == 0);
    CHECK(led_calls == 1);
    CHECK(strstr(led_args[0], "\"name\":\"pulse\"") != NULL);

    reset_calls();
    le_voice_listening_feedback_set(1);
    CHECK(audio_calls == 1);
    CHECK(led_calls == 1);
    CHECK(strstr(led_args[0], "\"name\":\"pulse\"") != NULL);

    audio_available = 0;
    reset_calls();
    le_voice_listening_feedback_set(1);
    CHECK(audio_calls == 0);
    CHECK(led_calls == 1);
    CHECK(strstr(led_args[0], "\"name\":\"pulse\"") != NULL);
    audio_available = 1;

    reset_calls();
    le_voice_listening_feedback_set(0);
    CHECK(audio_calls == 0);
    CHECK(led_calls == 2);
    CHECK(strstr(led_args[0], "\"owner\":\"voice-listening\"") != NULL);
    CHECK(strstr(led_args[1], "\"owner\":\"wakeword\"") != NULL);

    CHECK(setenv("LE_VOICE_WAKE_CHIRP", "0", 1) == 0);
    reset_calls();
    le_voice_listening_feedback_set(1);
    CHECK(audio_calls == 0);
    CHECK(led_calls == 1);
    CHECK(unsetenv("LE_VOICE_WAKE_CHIRP") == 0);

    puts("voice listening feedback: ring-only and chirped paths: ok");
    return 0;
}
