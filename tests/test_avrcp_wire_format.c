/*
 * Host-side AVRCP regression tests for ordinary AVCTP control traffic.
 *
 * The profile layer is exercised through its local socket-pair seam. These
 * fixtures are standard SINGLE COMMAND frames; they do not test malformed
 * input, controller transport, pairing, or service interaction.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adapter/bt_profile.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void check_avctp_response_header(const uint8_t *packet,
                                        uint8_t transaction,
                                        const char *label)
{
    char what[128];

    snprintf(what, sizeof(what), "%s preserves transaction", label);
    check((packet[0] >> 4) == transaction, what);
    snprintf(what, sizeof(what), "%s is a SINGLE packet", label);
    check(((packet[0] >> 2) & 0x03) == 0, what);
    snprintf(what, sizeof(what), "%s sets response C/R", label);
    check(((packet[0] >> 1) & 0x01) == 1, what);
}

int main(void)
{
    struct le_profiles profiles;
    uint8_t response[64];
    ssize_t length;

    /* AVCTP transaction 0, SINGLE COMMAND, AV/C STATUS, vendor-dependent
     * GET_CAPABILITIES for the Bluetooth SIG company capability. */
    static const uint8_t get_capabilities[] = {
        0x00, 0x11, 0x0e, 0x01, 0x48, 0x00,
        0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x01, 0x02
    };
    /* Expected fields follow BlueZ avrcp_handle_get_capabilities and
     * avctp_passthrough_press/release at upstream commit
     * 8750129efca858ff7e88e0d3166851fdd4590d8e, not the local encoder. */
    static const uint8_t get_capabilities_payload[] = {
        0x02, 0x01, 0x00, 0x19, 0x58
    };
    static const uint8_t get_events[] = {
        0x90, 0x11, 0x0e, 0x01, 0x48, 0x00,
        0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x01, 0x03
    };
    static const uint8_t get_events_response[] = {
        0x92, 0x11, 0x0e, 0x0c, 0x48, 0x00,
        0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x03,
        0x03, 0x01, 0x01
    };
    /* Standard PLAY press and release include the operation-data-length
     * operand, which is zero for these ordinary commands. */
    static const uint8_t passthrough[] = {
        0x20, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x44, 0x00
    };
    static const uint8_t release[] = {
        0xf0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0xc4, 0x00
    };
    static const uint8_t release_response[] = {
        0xf2, 0x11, 0x0e, 0x09, 0x48, 0x7c, 0xc4, 0x00
    };

    check(le_profile_test_init(&profiles, "LibreEcho") == 0,
          "profile test state initializes");

    length = le_profile_test_avrcp_exchange(&profiles, get_capabilities,
                                            sizeof(get_capabilities), response,
                                            sizeof(response));
    check(length == 13 + (ssize_t)sizeof(get_capabilities_payload),
          "GET_CAPABILITIES returns a complete vendor-dependent response");
    if (length == 13 + (ssize_t)sizeof(get_capabilities_payload)) {
        check_avctp_response_header(response, 0, "GET_CAPABILITIES response");
        check(response[1] == 0x11 && response[2] == 0x0e,
              "GET_CAPABILITIES response preserves AVRCP PID");
        check(response[3] == 0x0c && response[4] == 0x48 && response[5] == 0x00,
              "GET_CAPABILITIES response uses stable AV/C ctype and vendor opcode");
        check(response[6] == 0x00 && response[7] == 0x19 && response[8] == 0x58,
              "GET_CAPABILITIES response carries Bluetooth company operand");
        check(response[9] == 0x10 && response[10] == 0x00 &&
                  be16(response + 11) == sizeof(get_capabilities_payload),
              "GET_CAPABILITIES response has valid PDU and operand length");
        check(memcmp(response + 13, get_capabilities_payload,
                     sizeof(get_capabilities_payload)) == 0,
              "GET_CAPABILITIES response operands are correctly laid out");
    }

    length = le_profile_test_avrcp_exchange(&profiles, get_events,
                                            sizeof(get_events), response,
                                            sizeof(response));
    check(length == (ssize_t)sizeof(get_events_response),
          "GET_CAPABILITIES events returns only the requested capability");
    if (length == (ssize_t)sizeof(get_events_response)) {
        check_avctp_response_header(response, 9, "events response");
        check(memcmp(response, get_events_response, sizeof(get_events_response)) == 0,
              "events response has exact capability, count, event and PDU length");
    }

    length = le_profile_test_avrcp_exchange(&profiles, passthrough,
                                            sizeof(passthrough), response,
                                            sizeof(response));
    check(length == (ssize_t)sizeof(passthrough),
          "passthrough returns complete state and operation-data-length operands");
    if (length == (ssize_t)sizeof(passthrough)) {
        check_avctp_response_header(response, 2, "passthrough response");
        check(response[1] == 0x11 && response[2] == 0x0e && response[3] == 0x09,
              "passthrough response uses accepted AV/C ctype");
        check(response[4] == 0x48 && response[5] == 0x7c,
              "passthrough response preserves panel subunit and opcode");
        check(response[6] == 0x44 && response[7] == 0x00,
              "passthrough response preserves state, operation and data length");
    }

    length = le_profile_test_avrcp_exchange(&profiles, release,
                                            sizeof(release), response,
                                            sizeof(response));
    check(length == (ssize_t)sizeof(release_response),
          "PLAY release returns both required operands");
    if (length == (ssize_t)sizeof(release_response)) {
        check_avctp_response_header(response, 15, "PLAY release response");
        check(memcmp(response, release_response, sizeof(release_response)) == 0,
              "PLAY release preserves the release flag and zero data length");
    }

    le_profile_test_cleanup(&profiles);
    if (failures) {
        printf("avrcp wire format: %d FAILURE(s)\n", failures);
        return 1;
    }
    puts("avrcp wire format: all checks passed");
    return 0;
}
