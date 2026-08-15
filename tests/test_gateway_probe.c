#include "adapter/gateway_probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void finish_ipv4_header(unsigned char *packet, size_t length)
{
    write_u16(packet + 2, (uint16_t)length);
    write_u16(packet + 10, 0);
    write_u16(packet + 10, le_gateway_probe_checksum(packet, 20));
}

static int make_reply(unsigned char packet[28], struct in_addr *gateway)
{
    struct in_addr local;
    uint16_t checksum;

    memset(packet, 0, 28);
    packet[0] = 0x45;
    packet[8] = 64;
    packet[9] = IPPROTO_ICMP;
    CHECK(inet_pton(AF_INET, "192.0.2.1", gateway) == 1);
    CHECK(inet_pton(AF_INET, "192.0.2.2", &local) == 1);
    memcpy(packet + 12, &gateway->s_addr, sizeof(gateway->s_addr));
    memcpy(packet + 16, &local.s_addr, sizeof(local.s_addr));
    packet[20] = 0;
    packet[21] = 0;
    write_u16(packet + 24, 0x1234);
    write_u16(packet + 26, 7);
    checksum = le_gateway_probe_checksum(packet + 20, 8);
    write_u16(packet + 22, checksum);
    finish_ipv4_header(packet, 28);
    return 0;
}

static int test_reply_matching_is_strict(void)
{
    unsigned char packet[28];
    struct in_addr gateway;

    CHECK(make_reply(packet, &gateway) == 0);

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

static int test_ipv4_envelope_is_validated(void)
{
    unsigned char packet[28];
    struct in_addr gateway;

    CHECK(make_reply(packet, &gateway) == 0);
    packet[9] = IPPROTO_UDP;
    finish_ipv4_header(packet, sizeof(packet));
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);

    CHECK(make_reply(packet, &gateway) == 0);
    finish_ipv4_header(packet, sizeof(packet) + 1);
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);

    CHECK(make_reply(packet, &gateway) == 0);
    packet[6] = 0x20;
    finish_ipv4_header(packet, sizeof(packet));
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);

    CHECK(make_reply(packet, &gateway) == 0);
    packet[10] ^= 1;
    CHECK(le_gateway_probe_reply_matches(packet, sizeof(packet), gateway.s_addr,
                                          0x1234, 7) == 0);
    return 0;
}

static int test_receive_work_is_bounded(void)
{
    struct le_gateway_probe probe;
    unsigned char invalid = 0;
    int pair[2];
    int flags;
    int packet;

    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    flags = fcntl(pair[0], F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK(fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) == 0);
    le_gateway_probe_init(&probe);
    probe.fd = pair[0];
    probe.active = 1;
    for (packet = 0; packet < 64; ++packet)
        CHECK(send(pair[1], &invalid, 1, 0) == 1);
    CHECK(le_gateway_probe_receive(&probe) == 0);
    CHECK(recv(pair[0], &invalid, 1, MSG_DONTWAIT) == 1);
    le_gateway_probe_close(&probe);
    CHECK(close(pair[1]) == 0);
    return 0;
}

int main(void)
{
    if (test_checksum_round_trip() || test_reply_matching_is_strict() ||
        test_ipv4_envelope_is_validated() || test_receive_work_is_bounded())
        return 1;
    puts("gateway probe packet contract: ok");
    return 0;
}
