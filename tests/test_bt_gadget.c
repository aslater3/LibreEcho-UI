/*
 * Unit test for the wall-clock gadget state (bt_gadget.c).
 *
 * Pins the observable behaviour a real Echo has towards an Echo Wall Clock:
 * one `active` per ring, DeleteAlert on cancel, and the connect-time order
 * Discover -> timeinfo -> ClearIndicator.
 */
#include "adapter/bt_gadget.h"
#include "adapter/bt_agt.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

/* Binary-safe substring search: frames carry NUL bytes. */
static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t i;

    if (needle_len == 0 || haystack_len < needle_len)
        return NULL;
    for (i = 0; i + needle_len <= haystack_len; ++i)
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    return NULL;
}

static int count_frames(const uint8_t *frames, size_t length)
{
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    size_t offset = 0;
    int count = 0;

    while (offset < length) {
        size_t consumed = 0;
        if (le_agt_decode(frames + offset, length - offset, payload,
                          sizeof(payload), &consumed) < 0)
            return -1;
        offset += consumed;
        count++;
    }
    return count;
}

static int test_initially_empty(void)
{
    struct le_gadget gadget;
    uint8_t frames[LE_AGT_FRAME_MAX * 4];

    le_gadget_init(&gadget);
    CHECK(gadget.timeinfo_valid == 0);
    /* Discover + ClearIndicator; no timeinfo until one is known. */
    CHECK(count_frames(frames, le_gadget_build_connect_frames(
        &gadget, frames, sizeof(frames))) == 2);
    return 0;
}

static int test_connect_order(void)
{
    struct le_gadget gadget;
    uint8_t frames[LE_AGT_FRAME_MAX * 4];
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    size_t length;

    le_gadget_init(&gadget);
    CHECK(le_gadget_set_timeinfo(&gadget, "2026-10-14T10:38:52-04:00") == 0);
    length = le_gadget_build_connect_frames(&gadget, frames, sizeof(frames));
    CHECK(count_frames(frames, length) == 3);

    /* First frame is Discover. */
    {
        int decoded = le_agt_decode(frames, length, payload,
                                    sizeof(payload), NULL);
        CHECK(decoded > 0);
        CHECK(find_bytes(payload, (size_t)decoded, "Alexa.Discovery") != NULL);
    }
    /* Frames are binary (seq 0x00 / lengths), so search the whole range. */
    CHECK(find_bytes(frames, length, "timeinfo") != NULL);
    CHECK(find_bytes(frames, length, "ClearIndicator") != NULL);

    /* Out-of-order would break the clock: assert the byte order. */
    {
        const uint8_t *discover = find_bytes(frames, length, "Alexa.Discovery");
        const uint8_t *timeinfo = find_bytes(frames, length, "timeinfo");
        const uint8_t *clear = find_bytes(frames, length, "ClearIndicator");
        CHECK(discover && timeinfo && clear);
        CHECK(discover < timeinfo && timeinfo < clear);
    }
    return 0;
}

static int test_alert_lifecycle(void)
{
    struct le_gadget gadget;
    uint8_t frames[LE_AGT_FRAME_MAX * LE_GADGET_MAX_ALERTS];

    le_gadget_init(&gadget);
    CHECK(le_gadget_set_alert(&gadget, "1870862115", "TIMER",
                               "2026-10-14T10:44:19-04:00") == 0);
    CHECK(count_frames(frames, le_gadget_build_alert_frames(
        &gadget, frames, sizeof(frames))) == 1);

    /* Setting the same token again replaces rather than duplicates. */
    CHECK(le_gadget_set_alert(&gadget, "1870862115", "TIMER",
                               "2026-10-14T10:50:00-04:00") == 0);
    CHECK(count_frames(frames, le_gadget_build_alert_frames(
        &gadget, frames, sizeof(frames))) == 1);

    CHECK(le_gadget_delete_alert(&gadget, "1870862115") == 0);
    CHECK(le_gadget_build_alert_frames(&gadget, frames, sizeof(frames)) == 0);
    /* Deleting an unknown token reports failure. */
    CHECK(le_gadget_delete_alert(&gadget, "1870862115") < 0);
    return 0;
}

static int test_ringing_once(void)
{
    struct le_gadget gadget;

    le_gadget_init(&gadget);
    /* A real Echo sends `active` exactly once per ring. */
    CHECK(le_gadget_set_timers_active(&gadget, 1) == 1);
    CHECK(le_gadget_set_timers_active(&gadget, 1) == 0);
    CHECK(le_gadget_set_timers_active(&gadget, 1) == 0);
    CHECK(le_gadget_set_timers_active(&gadget, 0) == 1);
    CHECK(le_gadget_set_timers_active(&gadget, 0) == 0);
    /* A second ring announces again. */
    CHECK(le_gadget_set_timers_active(&gadget, 1) == 1);
    return 0;
}

static int test_alert_capacity(void)
{
    struct le_gadget gadget;
    char token[LE_GADGET_TOKEN_MAX];
    int i;

    le_gadget_init(&gadget);
    for (i = 0; i < LE_GADGET_MAX_ALERTS; ++i) {
        snprintf(token, sizeof(token), "%d", 1000 + i);
        CHECK(le_gadget_set_alert(&gadget, token, "TIMER",
                                  "2026-10-14T10:44:19-04:00") == 0);
    }
    /* A ninth distinct alert is refused rather than silently dropped. */
    CHECK(le_gadget_set_alert(&gadget, "9999", "TIMER",
                              "2026-10-14T10:44:19-04:00") < 0);
    /* Replacing an existing one still works at capacity. */
    CHECK(le_gadget_set_alert(&gadget, "1000", "ALARM",
                              "2026-10-14T11:00:00-04:00") == 0);
    return 0;
}

static int test_bounds(void)
{
    struct le_gadget gadget;
    uint8_t small[8];

    le_gadget_init(&gadget);
    CHECK(le_gadget_set_timeinfo(&gadget, NULL) < 0);
    CHECK(le_gadget_set_timeinfo(&gadget, "") < 0);
    CHECK(le_gadget_set_alert(&gadget, NULL, "TIMER", "x") < 0);
    CHECK(le_gadget_set_alert(&gadget, "1234567890123456", "TIMER", "x") < 0);
    /* Failed set_alert must not leak an in_use slot. */
    CHECK(le_gadget_set_alert(&gadget, "1", "", "2026-10-14T10:44:19-04:00") < 0);
    CHECK(le_gadget_build_alert_frames(&gadget, small, sizeof(small)) == 0);
    CHECK(gadget.alerts[0].in_use == 0);
    /* A tiny buffer yields no frames rather than an overrun. */
    CHECK(le_gadget_build_connect_frames(&gadget, small, sizeof(small)) == 0);
    CHECK(le_gadget_build_alert_frames(&gadget, small, sizeof(small)) == 0);
    return 0;
}

int main(void)
{
    if (test_initially_empty() || test_connect_order() ||
        test_alert_lifecycle() || test_ringing_once() ||
        test_alert_capacity() || test_bounds())
        return 1;
    printf("bt wall-clock state contract: ok\n");
    return 0;
}
