#include "voice_listening_led.h"

#include "adapter.h"

void le_voice_listening_led_set(int active)
{
    struct le_adapter *adapter;
    const char *args;

    adapter = le_adapter_connect(LE_ADAPTER_LED_SOCK, 100);
    if (!adapter)
        return;
    args = active
        ? "{\"name\":\"pulse\",\"r\":255,\"g\":0,\"b\":0,\"brightness\":70,\"repeats\":0,\"owner\":\"voice-listening\"}"
        : "{\"name\":\"stop\",\"owner\":\"voice-listening\"}";
    (void)le_adapter_call(adapter, "pattern", args, NULL, 0);
    /*
     * waked lights its own red pulse on the wake word and clears it on a
     * fixed five-second timer, independently of how long the turn actually
     * runs.  Recognition normally ends well before that, so dropping only
     * the listening pattern left the ring red for the remainder of the wake
     * window -- the device looked like it was still recording seconds after
     * it had stopped, which is exactly the wrong thing for a microphone
     * indicator to do.  Clear the wake owner too once recognition ends;
     * waked's own expiry is idempotent and simply finds it already stopped.
     */
    if (!active)
        (void)le_adapter_call(
            adapter, "pattern",
            "{\"name\":\"stop\",\"owner\":\"wakeword\"}", NULL, 0);
    le_adapter_close(adapter);
}
