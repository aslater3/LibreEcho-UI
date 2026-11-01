/*
 * Host test for the btd passive advertisement ring (bt_adv_proxy.c).
 *
 * Covers the lease window, BR/EDR filtering, oversized refusal, address byte
 * order (MGMT payload is least significant byte first), the exact hex record
 * layout that esphomed decodes, batch limits, gap reporting and a cursor that
 * is ahead of the ring after a restart.
 */
#include "adapter/bt_adv_proxy.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void query(const struct bt_adv_ring *ring, uint32_t after, char *out,
                  size_t size)
{
    int n = bt_adv_query(ring, after, out, size);

    check(n >= 0, "query fits the buffer");
    check(n >= 0 && (size_t)n == strlen(out), "query returns its length");
}

int main(void)
{
    struct bt_adv_ring ring;
    /* MGMT payload order: least significant address byte first. */
    const uint8_t addr_public[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    const uint8_t addr_random[6] = { 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa };
    const uint8_t eir_a[3] = { 0x02, 0x01, 0x06 };
    uint8_t eir_long[32];
    char out[2048];
    unsigned i;

    memset(eir_long, 0x5a, sizeof(eir_long));

    /* Lease window: live until the deadline, clamped, ended by ttl 0. */
    bt_adv_ring_init(&ring);
    check(!bt_adv_leased(&ring, 100), "no lease before one is granted");
    bt_adv_lease(&ring, 100, 1000);
    check(bt_adv_leased(&ring, 1099), "lease live inside its window");
    check(!bt_adv_leased(&ring, 1100), "lease lapses at its deadline");
    bt_adv_lease(&ring, 2000, 0);
    check(!bt_adv_leased(&ring, 2000), "ttl 0 ends the lease at once");
    bt_adv_lease(&ring, 0, 999999);
    check(bt_adv_leased(&ring, 29999), "lease clamped to the maximum");
    check(!bt_adv_leased(&ring, 30000), "clamped lease still expires");

    /* BR/EDR results never enter the relay. */
    bt_adv_ring_init(&ring);
    check(bt_adv_push(&ring, addr_public, 0, -40, eir_a, sizeof(eir_a)) == 0,
          "BR/EDR result ignored");
    check(ring.last_seq == 0, "BR/EDR does not advance the sequence");

    /* Public LE advertisement: address reversed to display order, type 0. */
    check(bt_adv_push(&ring, addr_public, 1, -60, eir_a, sizeof(eir_a)) == 1,
          "LE public stored");
    check(ring.records[0].seq == 1, "first record has seq 1");
    check(memcmp(ring.records[0].address,
                 "\x11\x22\x33\x44\x55\x66", 6) == 0,
          "address stored most significant byte first");
    check(ring.records[0].addr_type == 0, "LE public maps to ESPHome type 0");

    /* Random LE advertisement maps to type 1. */
    check(bt_adv_push(&ring, addr_random, 2, -100, NULL, 0) == 1,
          "LE random stored");
    check(ring.records[1].addr_type == 1, "LE random maps to ESPHome type 1");

    /* Oversized legacy payload is refused and counted, never truncated. */
    check(bt_adv_push(&ring, addr_public, 1, -50, eir_long,
                      sizeof(eir_long)) == -1, "oversized advertisement refused");
    check(ring.dropped == 1 && ring.last_seq == 2,
          "refused advertisement is counted, not stored");

    /* Exact wire text: addr, type, rssi (two's complement), len, data. */
    query(&ring, 0, out, sizeof(out));
    check(strcmp(out,
                 "{\"last\":2,\"gap\":0,\"dropped\":1,\"records\":\""
                 "112233445566" "00" "c4" "03" "020106"
                 "aabbccddeeff" "01" "9c" "00" "\"}") == 0,
          "batch text matches the record layout");

    /* Cursor at the head: nothing new, cursor stays put. */
    query(&ring, 2, out, sizeof(out));
    check(strcmp(out,
                 "{\"last\":2,\"gap\":0,\"dropped\":1,\"records\":\"\"}") == 0,
          "caught-up cursor returns an empty batch");

    /* Wrap the ring: 40 stored records, 32 retained, 16 per batch. */
    bt_adv_ring_init(&ring);
    for (i = 0; i < 40; i++)
        check(bt_adv_push(&ring, addr_public, 1, -70, eir_a, sizeof(eir_a)) == 1,
              "record stored while filling");
    query(&ring, 0, out, sizeof(out));
    check(strstr(out, "{\"last\":24,\"gap\":1,") == out,
          "overwritten records reported as a gap; batch capped at 16");
    check(strstr(out, "\"records\":\"") != NULL, "batch carries records");
    query(&ring, 30, out, sizeof(out));
    check(strstr(out, "{\"last\":40,\"gap\":0,") == out,
          "cursor inside the ring replays without a gap");

    /* A cursor from a previous daemon life is ahead of this ring. */
    query(&ring, 1000, out, sizeof(out));
    check(strstr(out, "{\"last\":40,\"gap\":1,") == out,
          "future cursor reports a gap and resyncs");

    /* Small output buffer fails closed instead of truncating JSON. */
    check(bt_adv_query(&ring, 30, out, 40) == -1,
          "undersized output buffer rejected");

    if (failures) {
        fprintf(stderr, "bt adv proxy: %d failure(s)\n", failures);
        return 1;
    }
    printf("bt adv proxy: ok\n");
    return 0;
}
