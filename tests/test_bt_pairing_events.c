#include "adapter/bt_pairing_events.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int test_confirmation_value(void)
{
    unsigned char payload[12] = {0};
    uint32_t value = 0;

    payload[7] = 1;
    payload[8] = 0xf1;
    payload[9] = 0xfb;
    payload[10] = 0x09;
    payload[11] = 0x00;
    CHECK(le_bt_pairing_event_value(LE_BT_MGMT_EV_USER_CONFIRM_REQUEST,
                                    payload, sizeof(payload), &value) == 0);
    CHECK(value == 654321U);
    CHECK(le_bt_pairing_event_value(LE_BT_MGMT_EV_USER_CONFIRM_REQUEST,
                                    payload, 11, &value) == -1);
    return 0;
}

static int test_passkey_notification_value(void)
{
    unsigned char payload[11] = {0};
    uint32_t value = 0;

    payload[7] = 0x40;
    payload[8] = 0xe2;
    payload[9] = 0x01;
    payload[10] = 0x00;
    CHECK(le_bt_pairing_event_value(0x0017, payload, sizeof(payload), &value) == 0);
    CHECK(value == 123456U);
    CHECK(le_bt_pairing_event_value(0x0017, payload, 10, &value) == -1);
    CHECK(le_bt_pairing_event_value(0x0017, NULL, sizeof(payload), &value) == -1);
    return 0;
}

int main(void)
{
    if (test_confirmation_value() || test_passkey_notification_value())
        return 1;
    puts("bt pairing-event decoder contract: ok");
    return 0;
}
