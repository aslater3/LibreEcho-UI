#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adapter/bt_profile.h"

static int failures;

static void check(int condition, const char *what)
{
    if (condition)
        printf("ok: %s\n", what);
    else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void)
{
    struct le_profiles profiles;
    uint8_t response[64];
    /* transaction=1, packet=SINGLE, message=COMMAND, signal=DISCOVER */
    const uint8_t discover[] = { 0x10, 0x01 };
    /* ACCEPT + DISCOVER, endpoint SEID 1, audio, TSEP=SINK. */
    const uint8_t expected[] = { 0x12, 0x01, 0x04, 0x08 };
    /* transaction=2, packet=SINGLE, message=COMMAND, signal=GET_CAPS,
     * ACP_SEID=1.  Media Codec has six value octets: media type, codec type,
     * four-byte SBC codec information element. */
    const uint8_t get_capabilities[] = { 0x20, 0x02, 0x04 };
    const uint8_t expected_capabilities[] = {
        0x22, 0x02,
        0x01, 0x00,
        0x07, 0x06, 0x00, 0x00, 0xff, 0xff, 0x02, 0x35,
    };
    ssize_t length;

    check(le_profile_test_init(&profiles, "LibreEcho") == 0,
          "profile test state initializes");
    length = le_profile_test_avdtp_exchange(&profiles, discover,
                                             sizeof(discover), response,
                                             sizeof(response));
    check(length == (ssize_t)sizeof(expected),
          "AVDTP DISCOVER response uses a two-byte SINGLE packet header");
    check(length == (ssize_t)sizeof(expected) &&
          memcmp(response, expected, sizeof(expected)) == 0,
          "AVDTP DISCOVER accept encodes a sink SEP and six-bit signal ID");

    length = le_profile_test_avdtp_exchange(&profiles, get_capabilities,
                                             sizeof(get_capabilities), response,
                                             sizeof(response));
    check(length == (ssize_t)sizeof(expected_capabilities),
          "AVDTP GET_CAPABILITIES returns a complete SBC capability element");
    check(length == (ssize_t)sizeof(expected_capabilities) &&
          memcmp(response, expected_capabilities,
                 sizeof(expected_capabilities)) == 0,
          "AVDTP SBC capability separates media type and codec type octets");
    le_profile_test_cleanup(&profiles);

    if (failures) {
        printf("avdtp wire format: %d FAILURE(s)\n", failures);
        return 1;
    }
    puts("avdtp wire format: all checks passed");
    return 0;
}