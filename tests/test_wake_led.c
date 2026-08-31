#include "adapter/adapter.h"
#include "adapter/wake_led.h"

#include <stdio.h>
#include <string.h>

static int status_contains(const char *socket_path, const char *needle)
{
    struct le_adapter *adapter =
        le_adapter_connect(socket_path, 500);
    char status[LE_ADAPTER_MSG_MAX];
    int result;

    if (!adapter)
        return 0;
    result = le_adapter_call(
        adapter, "status", "{}", status, sizeof(status));
    le_adapter_close(adapter);
    return result == LE_ADAPTER_OK && strstr(status, needle) != NULL;
}

int main(int argc, char **argv)
{
    struct le_wake_led led;
    const uint64_t started = 1000000000ULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <led-socket>\n", argv[0]);
        return 2;
    }
    le_wake_led_init(&led, argv[1]);
    if (le_wake_led_trigger(&led, started) != LE_ADAPTER_OK ||
        !status_contains(argv[1], "\"pattern\":\"pulse\"") ||
        !status_contains(argv[1], "\"pattern_owner\":\"wakeword\"")) {
        fprintf(stderr, "wake LED did not start\n");
        return 1;
    }
    if (le_wake_led_expire(
            &led, started + LE_WAKE_LED_DURATION_NS - 1) != LE_ADAPTER_OK ||
        !status_contains(argv[1], "\"pattern_owner\":\"wakeword\"")) {
        fprintf(stderr, "wake LED expired before five seconds\n");
        return 1;
    }
    if (le_wake_led_expire(
            &led, started + LE_WAKE_LED_DURATION_NS) != LE_ADAPTER_OK ||
        status_contains(argv[1], "\"pattern_owner\":\"wakeword\"")) {
        fprintf(stderr, "wake LED did not expire at five seconds\n");
        return 1;
    }
    puts("wake LED five-second pulse: ok");
    return 0;
}
