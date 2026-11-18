#include "wake_led.h"

#include "adapter.h"
#include "listening_ring.h"

#include <stddef.h>

#define WAKE_LED_CONNECT_TIMEOUT_MS 100
#define WAKE_LED_OWNER "wakeword"

static int send_pattern(const struct le_wake_led *led,
                        const char *arguments)
{
    struct le_adapter *adapter;
    int result;

    adapter = le_adapter_connect(
        led->socket_path, WAKE_LED_CONNECT_TIMEOUT_MS);
    if (!adapter)
        return LE_ADAPTER_ERR_CONNECT;
    result = le_adapter_call(
        adapter, "pattern", arguments, NULL, 0);
    le_adapter_close(adapter);
    return result;
}

static int stop_pattern(const struct le_wake_led *led)
{
    return send_pattern(
        led, "{\"name\":\"stop\",\"owner\":\"" WAKE_LED_OWNER "\"}");
}

void le_wake_led_init(struct le_wake_led *led, const char *socket_path)
{
    if (!led)
        return;
    led->socket_path = socket_path;
    led->stop_ns = 0;
}

int le_wake_led_trigger(struct le_wake_led *led, uint64_t now_ns)
{
    int result;

    if (!led || !led->socket_path)
        return LE_ADAPTER_ERR_PROTO;

    /*
     * Remove an earlier wake override before restarting it. This preserves
     * the LED daemon's single previous-owner slot when wakes arrive less
     * than five seconds apart.
     */
    (void)stop_pattern(led);
    /*
     * Use the same Alexa-style listening ring as the recognition window:
     * the Listening theme colour, with a brighter segment pointing at the
     * selected talker beam.  If micd has no confident direction the helper
     * leaves the ring solid; if micd is unavailable it still shows the
     * solid listening colour rather than the old recording-red pulse.
     */
    result = le_listening_ring_show(led->socket_path, WAKE_LED_OWNER);
    if (result != LE_ADAPTER_OK) {
        led->stop_ns = 0;
        return result;
    }
    led->stop_ns = now_ns + LE_WAKE_LED_DURATION_NS;
    return LE_ADAPTER_OK;
}

int le_wake_led_expire(struct le_wake_led *led, uint64_t now_ns)
{
    int result;

    if (!led || !led->stop_ns || now_ns < led->stop_ns)
        return LE_ADAPTER_OK;
    led->stop_ns = 0;
    result = stop_pattern(led);
    return result;
}

void le_wake_led_shutdown(struct le_wake_led *led)
{
    if (!led || !led->stop_ns)
        return;
    led->stop_ns = 0;
    (void)stop_pattern(led);
}
