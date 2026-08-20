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
    le_adapter_close(adapter);
}
