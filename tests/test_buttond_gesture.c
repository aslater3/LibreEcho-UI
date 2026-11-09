#define _POSIX_C_SOURCE 200809L

/*
 * Host regression for the hardware button gesture model (#394). Times are
 * fed explicitly, so the window and hold boundaries are exact.
 */
#include "buttond_gesture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct button_gestures g;

/* Collects every event due at `now`, the way buttond's loop drains them. */
static int drain(long long now, int *button, int *kind, int cap)
{
    struct button_gesture_event ev;
    int n = 0;

    while (button_gesture_expire(&g, now, &ev)) {
        assert(n < cap);
        button[n] = ev.button;
        kind[n] = ev.kind;
        n++;
    }
    return n;
}

static int key(int button, int value, long long now, int *out_kind)
{
    struct button_gesture_event ev;

    if (!button_gesture_key(&g, button, value, now, &ev))
        return 0;
    assert(ev.button == button);
    *out_kind = ev.kind;
    return 1;
}

#define BTN BUTTON_GESTURE_VOLUME_UP
#define OTHER BUTTON_GESTURE_ACTION

static void test_names(void)
{
    assert(!strcmp(button_gesture_button_name(BUTTON_GESTURE_VOLUME_UP), "volume_up"));
    assert(!strcmp(button_gesture_button_name(BUTTON_GESTURE_VOLUME_DOWN), "volume_down"));
    assert(!strcmp(button_gesture_button_name(BUTTON_GESTURE_MICROPHONE_MUTE), "microphone_mute"));
    assert(!strcmp(button_gesture_button_name(BUTTON_GESTURE_ACTION), "action"));
    assert(button_gesture_button_name(BUTTON_GESTURE_COUNT) == NULL);
    assert(button_gesture_button_name(-1) == NULL);
    assert(!strcmp(button_gesture_kind_name(BUTTON_GESTURE_PRESS), "press"));
    assert(!strcmp(button_gesture_kind_name(BUTTON_GESTURE_LONG_PRESS), "long_press"));
    assert(!strcmp(button_gesture_kind_name(BUTTON_GESTURE_DOUBLE_PRESS), "double_press"));
    assert(button_gesture_kind_name(BUTTON_GESTURE_KIND_COUNT) == NULL);
    puts("button names and kinds: OK");
}

static void test_single_press_after_window(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    /* Press and release: nothing yet, a second press may follow. */
    assert(!key(BTN, 1, 1000, &kind));
    assert(!key(BTN, 0, 1100, &kind));
    assert(drain(1100 + BUTTON_GESTURE_DOUBLE_MS, button, kinds, 4) == 0);
    assert(drain(1100 + BUTTON_GESTURE_DOUBLE_MS + 1, button, kinds, 4) == 1);
    assert(button[0] == BTN && kinds[0] == BUTTON_GESTURE_PRESS);
    /* Reported once, not again on later polls. */
    assert(drain(5000, button, kinds, 4) == 0);
    puts("single press reported once after the double window: OK");
}

static void test_double_press(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    assert(!key(BTN, 0, 1080, &kind));
    /* Second press starts inside the window: a double, decided on release. */
    assert(!key(BTN, 1, 1200, &kind));
    assert(key(BTN, 0, 1280, &kind));
    assert(kind == BUTTON_GESTURE_DOUBLE_PRESS);
    /* The double consumes the pending press: no late single press. */
    assert(drain(5000, button, kinds, 4) == 0);
    puts("double press inside the window, no trailing single press: OK");
}

static void test_double_press_window_edge(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    assert(!key(BTN, 0, 1100, &kind));
    /* Second press starts exactly at the window edge: still a double. */
    assert(!key(BTN, 1, 1100 + BUTTON_GESTURE_DOUBLE_MS, &kind));
    assert(key(BTN, 0, 1100 + BUTTON_GESTURE_DOUBLE_MS + 50, &kind));
    assert(kind == BUTTON_GESTURE_DOUBLE_PRESS);

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    assert(!key(BTN, 0, 1100, &kind));
    /* One millisecond past the window: the first press is reported now, on the new press. */
    assert(key(BTN, 1, 1100 + BUTTON_GESTURE_DOUBLE_MS + 1, &kind));
    assert(kind == BUTTON_GESTURE_PRESS);
    assert(drain(1100 + BUTTON_GESTURE_DOUBLE_MS + 1, button, kinds, 4) == 0);
    /* That press is its own short press, and it gets its own window. */
    assert(!key(BTN, 0, 1100 + BUTTON_GESTURE_DOUBLE_MS + 60, &kind));
    assert(drain(9000, button, kinds, 4) == 1);
    assert(kinds[0] == BUTTON_GESTURE_PRESS);
    puts("double-press window is inclusive at 350 ms and exclusive past it: OK");
}

static void test_long_press_while_held(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    assert(drain(1000 + BUTTON_GESTURE_LONG_MS - 1, button, kinds, 4) == 0);
    assert(drain(1000 + BUTTON_GESTURE_LONG_MS, button, kinds, 4) == 1);
    assert(button[0] == BTN && kinds[0] == BUTTON_GESTURE_LONG_PRESS);
    /* Still held: no second long press, and the release is silent. */
    assert(drain(4000, button, kinds, 4) == 0);
    assert(!key(BTN, 0, 4000, &kind));
    assert(drain(9000, button, kinds, 4) == 0);
    puts("long press reported while held, once, release silent: OK");
}

static void test_long_press_releases_before_expire(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    /* Released after the threshold but before the poll noticed it. */
    assert(key(BTN, 0, 1000 + BUTTON_GESTURE_LONG_MS + 20, &kind));
    assert(kind == BUTTON_GESTURE_LONG_PRESS);
    assert(drain(9000, button, kinds, 4) == 0);
    puts("long press reported on release when the poll missed it: OK");
}

static void test_autorepeat_and_unknown_are_ignored(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    /* Kernel autorepeat during a hold does not restart or end the hold. */
    assert(!key(BTN, 2, 1400, &kind));
    assert(!key(BTN, 2, 1500, &kind));
    assert(drain(1000 + BUTTON_GESTURE_LONG_MS, button, kinds, 4) == 1);
    assert(kinds[0] == BUTTON_GESTURE_LONG_PRESS);
    assert(!key(BTN, 2, 1900, &kind));
    assert(!key(-1, 1, 1900, &kind));
    assert(!key(BUTTON_GESTURE_COUNT, 0, 1900, &kind));
    puts("autorepeat and unknown buttons do not produce events: OK");
}

static void test_release_without_press_and_duplicate_press(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 0, 1000, &kind));
    assert(drain(5000, button, kinds, 4) == 0);
    assert(!key(BTN, 1, 2000, &kind));
    /* A repeated press value while already down does not restart the hold. */
    assert(!key(BTN, 1, 2000 + BUTTON_GESTURE_LONG_MS - 100, &kind));
    assert(drain(2000 + BUTTON_GESTURE_LONG_MS, button, kinds, 4) == 1);
    assert(kinds[0] == BUTTON_GESTURE_LONG_PRESS);
    puts("release without press and duplicate press are ignored: OK");
}

static void test_buttons_are_independent(void)
{
    int kind = -1, button[4], kinds[4];

    button_gestures_init(&g);
    assert(!key(BTN, 1, 1000, &kind));
    assert(!key(BTN, 0, 1050, &kind));
    /* A press on another button does not pair with the volume-up press. */
    assert(!key(OTHER, 1, 1100, &kind));
    assert(!key(OTHER, 0, 1150, &kind));
    assert(drain(1050 + BUTTON_GESTURE_DOUBLE_MS + 1, button, kinds, 4) == 1);
    assert(button[0] == BTN && kinds[0] == BUTTON_GESTURE_PRESS);
    assert(drain(1150 + BUTTON_GESTURE_DOUBLE_MS + 1, button, kinds, 4) == 1);
    assert(button[0] == OTHER && kinds[0] == BUTTON_GESTURE_PRESS);
    puts("gestures on one button do not pair with another: OK");
}

int main(void)
{
    test_names();
    test_single_press_after_window();
    test_double_press();
    test_double_press_window_edge();
    test_long_press_while_held();
    test_long_press_releases_before_expire();
    test_autorepeat_and_unknown_are_ignored();
    test_release_without_press_and_duplicate_press();
    test_buttons_are_independent();
    puts("buttond gesture model: all tests ok");
    return 0;
}
