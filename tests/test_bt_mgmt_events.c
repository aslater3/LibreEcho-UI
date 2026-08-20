/*
 * Unit test for the MGMT connection-event parsers (issue #62).
 *
 * Uses canned MGMT_EV_DEVICE_DISCONNECTED / MGMT_EV_CONNECT_FAILED payload
 * buffers to assert the reason/status byte is parsed, named, and rendered;
 * undersized payloads must be rejected instead of read past their end.
 */
#include "adapter/bt_mgmt_events.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void make_event_payload(unsigned char payload[8], unsigned char type,
                               unsigned char code)
{
    memset(payload, 0, 8);
    payload[0] = 0x00; payload[1] = 0x46; payload[2] = 0x81;
    payload[3] = 0x63; payload[4] = 0x01; payload[5] = 0x02;
    payload[6] = type;
    payload[7] = code;
}

static int test_disconnect_reason_names_known_codes(void)
{
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x00), "unknown"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x01), "timeout"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x02), "local-host-terminated"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x03), "remote-terminated"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x04), "authentication-failed"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x08), "profile-unsupported"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x0b), "connection-failed"));
    CHECK(!strcmp(le_bt_mgmt_disconnect_reason_name(0x7f), "unrecognized-reason"));
    return 0;
}

static int test_disconnect_reason_text_renders_byte_at_offset_7(void)
{
    unsigned char payload[8];
    char text[64];

    /* The 2026-08-14 audit event: reason 0x03 (remote terminated). */
    make_event_payload(payload, 0x00, 0x03);
    CHECK(le_bt_mgmt_disconnect_reason_text(payload, sizeof(payload),
                                            text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "remote-terminated (0x03)"));

    make_event_payload(payload, 0x01, 0x04);
    CHECK(le_bt_mgmt_disconnect_reason_text(payload, sizeof(payload),
                                            text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "authentication-failed (0x04)"));
    return 0;
}

static int test_connect_failed_text_uses_status_names(void)
{
    unsigned char payload[8];
    char text[64];

    make_event_payload(payload, 0x00, 0x04);
    CHECK(le_bt_mgmt_connect_failed_text(payload, sizeof(payload),
                                         text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "connect-failed (0x04)"));

    make_event_payload(payload, 0x00, 0x08);
    CHECK(le_bt_mgmt_connect_failed_text(payload, sizeof(payload),
                                         text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "timeout (0x08)"));

    make_event_payload(payload, 0x00, 0x99);
    CHECK(le_bt_mgmt_connect_failed_text(payload, sizeof(payload),
                                         text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "unrecognized-status (0x99)"));
    return 0;
}

static int test_undersized_payloads_are_rejected(void)
{
    unsigned char payload[8];
    char text[64];

    make_event_payload(payload, 0x00, 0x03);
    /* Address info only, no reason byte. */
    CHECK(le_bt_mgmt_disconnect_reason_text(payload, 7, text, sizeof(text)) == -1);
    CHECK(le_bt_mgmt_connect_failed_text(payload, 7, text, sizeof(text)) == -1);
    /* No payload at all. */
    CHECK(le_bt_mgmt_disconnect_reason_text(payload, 0, text, sizeof(text)) == -1);
    CHECK(le_bt_mgmt_connect_failed_text(NULL, sizeof(payload), text, sizeof(text)) == -1);
    /* Output buffer too small for the rendered text. */
    CHECK(le_bt_mgmt_disconnect_reason_text(payload, sizeof(payload), text, 4) == -1);
    return 0;
}

int main(void)
{
    if (test_disconnect_reason_names_known_codes() ||
        test_disconnect_reason_text_renders_byte_at_offset_7() ||
        test_connect_failed_text_uses_status_names() ||
        test_undersized_payloads_are_rejected())
        return 1;
    puts("bt mgmt connection-event contract: ok");
    return 0;
}
