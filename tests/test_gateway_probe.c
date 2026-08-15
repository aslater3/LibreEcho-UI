#include "adapter/gateway_probe.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void write_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)(value & 0xff);
}

static int test_checksum_round_trip(void)
{
    unsigned char echo[8] = {8, 0, 0, 0, 0x12, 0x34, 0x00, 0x07};
    uint16_t checksum = le_gateway_probe_checksum(echo, sizeof(echo));

    CHECK(checksum != 0);
    write_u16(echo + 2, checksum);
    CHECK(le_gateway_probe_checksum(echo, sizeof(echo)) == 0);
    return 0;
}

static int test_reply_matching_is_strict(void)
{
    unsigned char packet[28];
    struct in_addr gateway;
    uint16_t checksum;

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x45;
    CHECK(inet_pton(AF_INET, "192.0.2.1", &gateway) == 1);
    memcpy(packet + 12, &gateway.s_addr, sizeof(gateway.s_addr));
    packet[20] = 0;
    packet[21] = 0;
    write_u16(packet + 24, 0x1234);
    write_u16(packet + 26, 7);
    checksum = le_gateway_probe_checksum(packet + 20, 8);
    write_u16(packet + 22, checksum);

    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 1);
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1235, 7) == 0);
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 8) == 0);
    packet[20] = 8;
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);
    packet[20] = 0;
    packet[12] ^= 1;
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);
    CHECK(le_gateway_probe_reply_matches(packet, 27, gateway.s_addr,
                                          0x1234, 7) == 0);
    return 0;
}

int main(void)
{
    if (test_checksum_round_trip() || test_reply_matching_is_strict())
        return 1;
    puts("gateway probe packet contract: ok");
    return 0;
}
