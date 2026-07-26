#ifndef LE_WAKE_LED_H
#define LE_WAKE_LED_H

#include <stdint.h>

#define LE_WAKE_LED_DURATION_NS 5000000000ULL

struct le_wake_led {
    const char *socket_path;
    uint64_t stop_ns;
};

void le_wake_led_init(struct le_wake_led *led, const char *socket_path);
int le_wake_led_trigger(struct le_wake_led *led, uint64_t now_ns);
int le_wake_led_expire(struct le_wake_led *led, uint64_t now_ns);
void le_wake_led_shutdown(struct le_wake_led *led);

#endif
