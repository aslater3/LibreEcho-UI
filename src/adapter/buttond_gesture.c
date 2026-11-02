#include "buttond_gesture.h"

#include <stddef.h>
#include <string.h>

static const char *const button_names[BUTTON_GESTURE_COUNT] = {
    "volume_up", "volume_down", "microphone_mute", "action"
};

static const char *const kind_names[BUTTON_GESTURE_KIND_COUNT] = {
    "press", "long_press", "double_press"
};

const char *button_gesture_button_name(int button)
{
    if (button < 0 || button >= BUTTON_GESTURE_COUNT)
        return NULL;
    return button_names[button];
}

const char *button_gesture_kind_name(int kind)
{
    if (kind < 0 || kind >= BUTTON_GESTURE_KIND_COUNT)
        return NULL;
    return kind_names[kind];
}

void button_gestures_init(struct button_gestures *g)
{
    memset(g, 0, sizeof(*g));
}

static int report(struct button_gesture_event *event, int button, int kind)
{
    event->button = button;
    event->kind = kind;
    return 1;
}

int button_gesture_key(struct button_gestures *g, int button, int value,
                       long long now_ms, struct button_gesture_event *event)
{
    struct button_gesture_state *s;

    if (button < 0 || button >= BUTTON_GESTURE_COUNT)
        return 0;
    /* Kernel autorepeat is a hold continuing, not a new press. */
    if (value != 0 && value != 1)
        return 0;
    s = &g->button[button];

    if (value == 1) {
        if (s->down)
            return 0;
        s->down = 1;
        s->long_sent = 0;
        s->down_ms = now_ms;
        /* A short press whose window already closed is a plain press. */
        if (s->pending && now_ms - s->pending_ms > BUTTON_GESTURE_DOUBLE_MS) {
            s->pending = 0;
            return report(event, button, BUTTON_GESTURE_PRESS);
        }
        return 0;
    }

    if (!s->down)
        return 0;
    s->down = 0;
    if (s->long_sent)
        return 0;
    /* Held past the threshold but the poll had not reported it yet. */
    if (now_ms - s->down_ms >= BUTTON_GESTURE_LONG_MS) {
        s->long_sent = 1;
        s->pending = 0;
        return report(event, button, BUTTON_GESTURE_LONG_PRESS);
    }
    if (s->pending && s->down_ms - s->pending_ms <= BUTTON_GESTURE_DOUBLE_MS) {
        s->pending = 0;
        return report(event, button, BUTTON_GESTURE_DOUBLE_PRESS);
    }
    /* Hold the short press back until the window shows whether a second follows. */
    s->pending = 1;
    s->pending_ms = now_ms;
    return 0;
}

int button_gesture_expire(struct button_gestures *g, long long now_ms,
                          struct button_gesture_event *event)
{
    int button;

    for (button = 0; button < BUTTON_GESTURE_COUNT; button++) {
        struct button_gesture_state *s = &g->button[button];

        if (s->down && !s->long_sent &&
            now_ms - s->down_ms >= BUTTON_GESTURE_LONG_MS) {
            /* A long press drops any short press still waiting for a double. */
            s->long_sent = 1;
            s->pending = 0;
            return report(event, button, BUTTON_GESTURE_LONG_PRESS);
        }
        if (!s->down && s->pending &&
            now_ms - s->pending_ms > BUTTON_GESTURE_DOUBLE_MS) {
            s->pending = 0;
            return report(event, button, BUTTON_GESTURE_PRESS);
        }
    }
    return 0;
}
