#ifndef LE_BUTTOND_GESTURE_H
#define LE_BUTTOND_GESTURE_H

/*
 * Press, double-press and long-press detection for the hardware buttons (#394).
 * Pure logic: the caller supplies monotonic milliseconds and key values, and
 * gets back at most one event per call. Local handling in buttond.c never
 * waits on this; it only reports what the buttons did.
 */

enum button_gesture_button {
    BUTTON_GESTURE_VOLUME_UP = 0,
    BUTTON_GESTURE_VOLUME_DOWN,
    BUTTON_GESTURE_MICROPHONE_MUTE,
    BUTTON_GESTURE_ACTION,
    BUTTON_GESTURE_COUNT
};

enum button_gesture_kind {
    BUTTON_GESTURE_PRESS = 0,
    BUTTON_GESTURE_LONG_PRESS,
    BUTTON_GESTURE_DOUBLE_PRESS,
    BUTTON_GESTURE_KIND_COUNT
};

/* A press held at least this long is a long press; reported while held. */
#define BUTTON_GESTURE_LONG_MS 800LL
/* Two presses starting within this gap of each other are a double press. */
#define BUTTON_GESTURE_DOUBLE_MS 350LL

struct button_gesture_state {
    int down;             /* the button is held now */
    int long_sent;        /* the long press for this hold already went out */
    int pending;          /* a short press may still become a double press */
    long long down_ms;    /* when the current hold started */
    long long pending_ms; /* when the pending short press was released */
};

struct button_gestures {
    struct button_gesture_state button[BUTTON_GESTURE_COUNT];
};

struct button_gesture_event {
    int button;           /* enum button_gesture_button */
    int kind;             /* enum button_gesture_kind */
};

/* Names used on the wire and as Home Assistant event types. */
const char *button_gesture_button_name(int button);
const char *button_gesture_kind_name(int kind);

void button_gestures_init(struct button_gestures *g);

/*
 * Feed one EV_KEY value for a button: 1 press, 0 release. Autorepeat (2) and
 * unknown buttons are ignored. Returns 1 and fills *event when an event is due
 * now, 0 otherwise.
 */
int button_gesture_key(struct button_gestures *g, int button, int value,
                       long long now_ms, struct button_gesture_event *event);

/*
 * Call periodically while buttons may be held or a short press is pending.
 * Returns 1 and fills *event for one due event (a long press reached while
 * still held, or a short press whose double-press window has passed); call
 * again until it returns 0.
 */
int button_gesture_expire(struct button_gestures *g, long long now_ms,
                          struct button_gesture_event *event);

#endif
