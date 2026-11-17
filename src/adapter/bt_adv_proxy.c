#include "bt_adv_proxy.h"

#include <stdio.h>
#include <string.h>

void bt_adv_ring_init(struct bt_adv_ring *ring)
{
    memset(ring, 0, sizeof(*ring));
}

void bt_adv_lease(struct bt_adv_ring *ring, uint64_t now_ms, unsigned ttl_ms)
{
    if (ttl_ms == 0) {
        ring->lease_until_ms = 0;
        return;
    }
    if (ttl_ms > BT_ADV_LEASE_MAX_MS)
        ttl_ms = BT_ADV_LEASE_MAX_MS;
    ring->lease_until_ms = now_ms + ttl_ms;
}

int bt_adv_leased(const struct bt_adv_ring *ring, uint64_t now_ms)
{
    return ring->lease_until_ms != 0 && now_ms < ring->lease_until_ms;
}

int bt_adv_push(struct bt_adv_ring *ring, const uint8_t *address,
                uint8_t mgmt_addr_type, int rssi, const uint8_t *eir,
                size_t eir_length)
{
    struct bt_adv_record *record;
    size_t i;

    if (mgmt_addr_type != 1 && mgmt_addr_type != 2)
        return 0;
    if (eir_length > BT_ADV_DATA_MAX) {
        ring->dropped++;
        return -1;
    }
    record = &ring->records[ring->last_seq % BT_ADV_RING];
    ring->last_seq++;
    record->seq = ring->last_seq;
    for (i = 0; i < 6; i++)
        record->address[i] = address[5 - i];
    record->addr_type = mgmt_addr_type == 2 ? 1 : 0;
    if (rssi < -127)
        rssi = -127;
    if (rssi > 127)
        rssi = 127;
    record->rssi = (int8_t)rssi;
    record->length = (uint8_t)eir_length;
    if (eir_length)
        memcpy(record->data, eir, eir_length);
    return 1;
}

/* Append-only writer over a caller buffer; fails closed on overflow. */
struct out_cursor {
    char *data;
    size_t size;
    size_t used;
    int failed;
};

static void put(struct out_cursor *c, const char *text)
{
    size_t n = strlen(text);

    if (c->failed)
        return;
    if (n >= c->size - c->used) {
        c->failed = 1;
        return;
    }
    memcpy(c->data + c->used, text, n + 1);
    c->used += n;
}

static void put_hex(struct out_cursor *c, const uint8_t *bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    char pair[3];
    size_t i;

    for (i = 0; i < count; i++) {
        pair[0] = digits[bytes[i] >> 4];
        pair[1] = digits[bytes[i] & 15];
        pair[2] = '\0';
        put(c, pair);
    }
}

int bt_adv_query(const struct bt_adv_ring *ring, uint32_t after,
                 char *out, size_t out_size)
{
    struct out_cursor c = { out, out_size, 0, 0 };
    uint32_t oldest, start, count, last, seq;
    int gap = 0;
    char head[96];
    int head_len;

    if (!out || out_size < 2)
        return -1;
    out[0] = '\0';
    if (after > ring->last_seq) {
        /* The daemon restarted, or the cursor is from a sequence this
         * ring never produced: older records cannot be replayed. */
        gap = 1;
        after = ring->last_seq;
    }
    oldest = ring->last_seq > BT_ADV_RING ?
             ring->last_seq - BT_ADV_RING + 1 : 1;
    start = after + 1;
    if (start < oldest) {
        gap = 1;
        start = oldest;
    }
    count = 0;
    if (start <= ring->last_seq)
        count = ring->last_seq - start + 1;
    if (count > BT_ADV_BATCH)
        count = BT_ADV_BATCH;
    /* The cursor advances only past records actually returned. */
    last = count ? start + count - 1 : ring->last_seq;

    head_len = snprintf(head, sizeof(head),
                        "{\"last\":%lu,\"gap\":%d,\"dropped\":%lu,\"records\":\"",
                        (unsigned long)last, gap,
                        (unsigned long)ring->dropped);
    if (head_len < 0 || (size_t)head_len >= sizeof(head))
        return -1;
    put(&c, head);
    for (seq = start; seq < start + count; seq++) {
        const struct bt_adv_record *r = &ring->records[(seq - 1) % BT_ADV_RING];
        uint8_t addr_type = r->addr_type;
        uint8_t rssi = (uint8_t)r->rssi;
        uint8_t length = r->length;

        put_hex(&c, r->address, 6);
        put_hex(&c, &addr_type, 1);
        put_hex(&c, &rssi, 1);
        put_hex(&c, &length, 1);
        put_hex(&c, r->data, r->length);
    }
    put(&c, "\"}");
    if (c.failed)
        return -1;
    return (int)c.used;
}
