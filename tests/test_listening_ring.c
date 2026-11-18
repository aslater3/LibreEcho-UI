#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"
#include "adapter/listening_ring.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct le_adapter {
    int kind;
};

static struct le_adapter mic_adapter = {1};
static struct le_adapter led_adapter = {2};

static const char *beams_json;
static int mic_available = 1;
static int mic_calls;
static int led_calls;
static char led_args[512];

struct le_adapter *le_adapter_connect(const char *path, int timeout_ms)
{
    (void)timeout_ms;
    if (!strcmp(path, LE_ADAPTER_MIC_SOCK))
        return mic_available ? &mic_adapter : NULL;
    if (!strcmp(path, LE_ADAPTER_LED_SOCK))
        return &led_adapter;
    return NULL;
}

void le_adapter_set_io_timeout(struct le_adapter *adapter, int timeout_ms)
{
    (void)adapter;
    (void)timeout_ms;
}

void le_adapter_close(struct le_adapter *adapter)
{
    (void)adapter;
}

int le_adapter_call(struct le_adapter *adapter, const char *command,
                    const char *args, char *output, size_t output_size)
{
    (void)args;
    if (adapter == &mic_adapter) {
        ++mic_calls;
        if (strcmp(command, "beams") != 0 || !output || output_size == 0)
            return -1;
        snprintf(output, output_size, "%s", beams_json ? beams_json : "");
        return LE_ADAPTER_OK;
    }
    if (adapter == &led_adapter) {
        ++led_calls;
        if (strcmp(command, "pattern") != 0)
            return -1;
        snprintf(led_args, sizeof(led_args), "%s", args ? args : "");
        return LE_ADAPTER_OK;
    }
    return -1;
}

static void reset_calls(void)
{
    mic_calls = 0;
    led_calls = 0;
    beams_json = NULL;
    mic_available = 1;
    led_args[0] = '\0';
}

static int expect_direction(const char *json, int azimuth)
{
    char needle[64];

    reset_calls();
    beams_json = json;
    CHECK(le_listening_ring_show("/run/libreecho/led.sock", "voice-test") ==
          LE_ADAPTER_OK);
    CHECK(mic_calls == 1);
    CHECK(led_calls == 1);
    CHECK(strstr(led_args, "\"name\":\"direction\"") != NULL);
    CHECK(strstr(led_args, "\"profile\":\"listening\"") != NULL);
    CHECK(strstr(led_args, "\"owner\":\"voice-test\"") != NULL);
    CHECK(strstr(led_args, "\"repeats\":0") != NULL);
    if (azimuth >= 0) {
        snprintf(needle, sizeof(needle), "\"azimuth\":%d", azimuth);
        CHECK(strstr(led_args, needle) != NULL);
    } else {
        CHECK(strstr(led_args, "\"azimuth\"") == NULL);
    }
    return 0;
}

int main(void)
{
    CHECK(expect_direction(
              "{\"running\":true,\"contrast_db\":7.5,\"selected_deg\":270}",
              270) == 0);
    CHECK(expect_direction(
              "{\"running\":true,\"contrast_db\":2.0,\"selected_deg\":0}",
              0) == 0);
    CHECK(expect_direction(
              "{\"running\":true,\"contrast_db\":6.0,\"selected_deg\":359}",
              359) == 0);
    CHECK(expect_direction(
              "{\"running\":true,\"contrast_db\":1.9,\"selected_deg\":90}",
              -1) == 0);
    CHECK(expect_direction(
              "{\"running\":false,\"contrast_db\":9.0,\"selected_deg\":90}",
              -1) == 0);
    CHECK(expect_direction(
              "{\"running\":true,\"selected_deg\":90}", -1) == 0);
    CHECK(expect_direction(
              "{\"running\":true,\"contrast_db\":7.0,\"selected_deg\":360}",
              -1) == 0);

    reset_calls();
    mic_available = 0;
    beams_json =
        "{\"running\":true,\"contrast_db\":9.0,\"selected_deg\":90}";
    CHECK(le_listening_ring_show("/run/libreecho/led.sock", "voice-test") ==
          LE_ADAPTER_OK);
    CHECK(mic_calls == 0);
    CHECK(led_calls == 1);
    CHECK(strstr(led_args, "\"azimuth\"") == NULL);

    puts("listening ring: direction, fallback and owner: ok");
    return 0;
}
