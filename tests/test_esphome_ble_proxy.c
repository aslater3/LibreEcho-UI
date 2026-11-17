/*
 * Host test for the ESPHome passive raw-advertisement proxy (esphome_ble_proxy.c).
 *
 * Checks subscription ownership, the btd lease and poll schedule, cursor
 * handling, and that the encoded BluetoothLERawAdvertisementsResponse decodes
 * to the same address, RSSI (sint32 zigzag), address type and payload.
 */
#include "adapter/esphome_ble_proxy.h"
#include "adapter/esphome_proto.h"

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

/* Subscribe request body: field 1 uint32 flags. */
static size_t subscribe_body(unsigned flags, unsigned char *out)
{
    struct ep_writer w = { out, 8, 0, 0 };

    ep_uint(&w, 1, flags);
    return w.error ? 0 : w.length;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int main(void)
{
    struct le_ble_proxy proxy;
    unsigned char body[8], out[2048];
    char args[160];
    size_t n;
    int kind;

    check(LE_BLE_PROXY_FEATURES == 33U, "feature flags are PASSIVE_SCAN | RAW_ADVERTISEMENTS");

    le_ble_proxy_init(&proxy);
    check(le_ble_proxy_due(&proxy, 0, args, sizeof(args)) == 0,
          "nothing is due without a subscriber");

    /* Subscription ownership: one raw subscriber, unknown bits refused. */
    n = subscribe_body(LE_BLE_PROXY_SUB_RAW, body);
    check(le_ble_proxy_subscribe(&proxy, 0, body, n) == 0, "raw subscribe accepted");
    check(le_ble_proxy_subscribe(&proxy, 1, body, n) == -1,
          "second client refused while subscribed");
    n = subscribe_body(2, body);
    check(le_ble_proxy_subscribe(&proxy, 0, body, n) == -1,
          "unknown subscription bit refused");

    /* First lease first, then the cursor-init poll with no "after". */
    kind = le_ble_proxy_due(&proxy, 0, args, sizeof(args));
    check(kind == LE_BLE_JOB_LEASE, "lease due immediately on subscribe");
    check(strcmp(args, "{\"enabled\":true,\"ttl_ms\":5000}") == 0,
          "lease asks for a 5 s window");
    kind = le_ble_proxy_due(&proxy, 0, args, sizeof(args));
    check(kind == LE_BLE_JOB_POLL, "poll due on subscribe");
    check(strcmp(args, "{}") == 0, "first poll only sets the cursor");
    check(le_ble_proxy_due(&proxy, 100, args, sizeof(args)) == 0,
          "nothing due again inside the poll interval");
    kind = le_ble_proxy_due(&proxy, 250, args, sizeof(args));
    check(kind == LE_BLE_JOB_POLL, "poll due every 250 ms");
    check(proxy.poll_due_ms == 500, "next poll is scheduled");

    /* A batch with two records: a random address with no payload, and a
     * public address with a three-byte payload and RSSI -60. Hex layout:
     * addr12 type2 rssi2 len2 data. */
    {
        static const char line[] =
            "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"last\":2,\"gap\":0,"
            "\"dropped\":0,\"records\":\""
            "112233445566" "00" "c4" "03" "020106"
            "aabbccddeeff" "01" "9c" "00" "\"}}";
        size_t written = le_ble_proxy_poll_result(&proxy, line, strlen(line),
                                                  out, sizeof(out));
        struct ep_reader r;
        struct ep_field f;
        unsigned count = 0;

        check(written > 0, "batch encodes to a message");
        ep_reader_init(&r, out, written);
        while (ep_next(&r, &f) > 0) {
            uint64_t address, rssi_zigzag, address_type;
            uint64_t expect_address[2] = { 0x112233445566ULL, 0xaabbccddeeffULL };
            uint64_t expect_zigzag[2] = { 119U, 199U };   /* sint32 zigzag of -60, -100 */
            uint64_t expect_type[2] = { 0, 1 };
            size_t expect_data[2] = { 3, 0 };

            check(f.number == 1 && f.wire == 2, "each advertisement is repeated field 1");
            if (count < 2) {
                address = ep_get_uint(f.data, f.length, 1, 0);
                rssi_zigzag = ep_get_uint(f.data, f.length, 2, 0);
                address_type = ep_get_uint(f.data, f.length, 3, 0);
                check(address == expect_address[count], "address decodes to display value");
                check(rssi_zigzag == expect_zigzag[count], "rssi encodes as sint32 zigzag");
                check(address_type == expect_type[count], "address type maps to ESPHome value");
                {
                    struct ep_reader inner;
                    struct ep_field g;
                    size_t data_length = 0;

                    ep_reader_init(&inner, f.data, f.length);
                    while (ep_next(&inner, &g) > 0)
                        if (g.number == 4)
                            data_length = g.length;
                    check(data_length == expect_data[count], "payload length preserved");
                }
            }
            count++;
        }
        check(!r.error, "encoded message is well-formed protobuf");
        check(count == 2, "both records encoded");
        check(proxy.cursor == 2 && proxy.cursor_valid, "cursor advanced to last");
    }

    /* Next poll uses the cursor. */
    kind = le_ble_proxy_due(&proxy, 750, args, sizeof(args));
    check(kind == LE_BLE_JOB_POLL && strcmp(args, "{\"after\":2}") == 0,
          "poll resumes after the cursor");

    /* Malformed hex stops at the bad record and encodes nothing from it. */
    {
        static const char bad[] =
            "{\"ok\":true,\"data\":{\"last\":3,\"gap\":0,\"dropped\":0,"
            "\"records\":\"11zz33445566" "00" "c4" "00" "\"}}";
        size_t written = le_ble_proxy_poll_result(&proxy, bad, strlen(bad),
                                                  out, sizeof(out));
        check(written == 0, "malformed record is not encoded");
        check(proxy.cursor == 3, "cursor still advances past a dropped batch");
    }

    /* Oversized or truncated payload length is refused. */
    {
        static const char trunc[] =
            "{\"ok\":true,\"data\":{\"last\":4,\"gap\":0,\"dropped\":0,"
            "\"records\":\"112233445566" "00" "c4" "05" "0201\"}}";
        check(le_ble_proxy_poll_result(&proxy, trunc, strlen(trunc),
                                       out, sizeof(out)) == 0,
              "record shorter than its declared length refused");
    }

    /* Withdrawal: flags 0 from the owner ends the stream with one stop lease. */
    n = subscribe_body(0, body);
    check(le_ble_proxy_subscribe(&proxy, 1, body, n) == 0,
          "flags 0 from a non-owner is a no-op");
    check(proxy.owner == 0, "non-owner withdrawal leaves owner unchanged");
    check(le_ble_proxy_subscribe(&proxy, 0, body, n) == 0, "owner withdraws");
    kind = le_ble_proxy_due(&proxy, 800, args, sizeof(args));
    check(kind == LE_BLE_JOB_LEASE && strcmp(args, "{\"enabled\":false}") == 0,
          "withdrawal stops the btd lease at once");
    check(le_ble_proxy_due(&proxy, 800, args, sizeof(args)) == 0,
          "nothing else due after withdrawal");

    /* Client close drops the subscription and schedules the stop too. */
    n = subscribe_body(LE_BLE_PROXY_SUB_RAW, body);
    check(le_ble_proxy_subscribe(&proxy, 1, body, n) == 0, "new subscriber accepted");
    le_ble_proxy_close(&proxy, 0);
    check(proxy.owner == 1, "closing a non-owner keeps the subscription");
    le_ble_proxy_close(&proxy, 1);
    kind = le_ble_proxy_due(&proxy, 900, args, sizeof(args));
    check(kind == LE_BLE_JOB_LEASE && starts_with(args, "{\"enabled\":false"),
          "closing the owner stops the btd lease");

    /* Undersized args buffer fails closed. */
    n = subscribe_body(LE_BLE_PROXY_SUB_RAW, body);
    check(le_ble_proxy_subscribe(&proxy, 2, body, n) == 0, "subscribe again");
    check(le_ble_proxy_due(&proxy, 1000, args, 4) == 0,
          "undersized args buffer does not schedule a request");

    if (failures) {
        fprintf(stderr, "esphome ble proxy: %d failure(s)\n", failures);
        return 1;
    }
    printf("esphome ble proxy: ok\n");
    return 0;
}
