#include "esphome_ble_proxy.h"

#include "esphome_proto.h"
#include "../json.h"

#include <stdio.h>
#include <string.h>

/* Bounded output sink for protobuf encoding. */
struct sink {
    unsigned char *data;
    size_t capacity;
    size_t length;
    int error;
};

static void put_byte(struct sink *s, unsigned char b)
{
    if (s->error)
        return;
    if (s->length >= s->capacity) {
        s->error = 1;
        return;
    }
    s->data[s->length++] = b;
}

static void put_varint(struct sink *s, uint64_t v)
{
    do {
        unsigned char b = (unsigned char)(v & 0x7f);

        v >>= 7;
        if (v)
            b |= 0x80;
        put_byte(s, b);
    } while (v && !s->error);
}

static void put_tag(struct sink *s, unsigned field, unsigned wire)
{
    put_varint(s, ((uint64_t)field << 3) | wire);
}

static void put_bytes(struct sink *s, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        put_byte(s, p[i]);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int hex_byte(const char *text, unsigned char *out)
{
    int hi = hex_value(text[0]), lo = hex_value(text[1]);

    if (hi < 0 || lo < 0)
        return -1;
    *out = (unsigned char)(hi * 16 + lo);
    return 0;
}

void le_ble_proxy_init(struct le_ble_proxy *proxy)
{
    memset(proxy, 0, sizeof(*proxy));
    proxy->owner = -1;
}

int le_ble_proxy_subscribe(struct le_ble_proxy *proxy, int client,
                           const void *msg, size_t length)
{
    uint64_t flags = ep_get_uint(msg, length, 1, 0);

    if (flags & ~(uint64_t)LE_BLE_PROXY_SUB_RAW)
        return -1;
    if (!(flags & LE_BLE_PROXY_SUB_RAW)) {
        /* Flags 0 is how a client withdraws its subscription. */
        if (proxy->owner == client) {
            proxy->owner = -1;
            proxy->stop_pending = 1;
        }
        return 0;
    }
    if (proxy->owner >= 0 && proxy->owner != client)
        return -1;
    if (proxy->owner != client) {
        proxy->owner = client;
        proxy->cursor_valid = 0;
        proxy->lease_due_ms = 0;
        proxy->poll_due_ms = 0;
    }
    return 0;
}

void le_ble_proxy_close(struct le_ble_proxy *proxy, int client)
{
    if (proxy->owner == client) {
        proxy->owner = -1;
        proxy->stop_pending = 1;
    }
}

int le_ble_proxy_due(struct le_ble_proxy *proxy, uint64_t now_ms,
                     char *args, size_t args_size)
{
    int written;

    if (!args || args_size < 2)
        return 0;
    if (proxy->stop_pending) {
        proxy->stop_pending = 0;
        snprintf(args, args_size, "{\"enabled\":false}");
        return LE_BLE_JOB_LEASE;
    }
    if (proxy->owner < 0)
        return 0;
    if (now_ms >= proxy->lease_due_ms) {
        proxy->lease_due_ms = now_ms + LE_BLE_LEASE_EVERY_MS;
        written = snprintf(args, args_size,
                           "{\"enabled\":true,\"ttl_ms\":%u}",
                           (unsigned)LE_BLE_LEASE_TTL_MS);
        if (written < 0 || (size_t)written >= args_size)
            return 0;
        return LE_BLE_JOB_LEASE;
    }
    if (now_ms >= proxy->poll_due_ms) {
        proxy->poll_due_ms = now_ms + LE_BLE_POLL_EVERY_MS;
        if (proxy->cursor_valid)
            written = snprintf(args, args_size, "{\"after\":%lu}",
                               (unsigned long)proxy->cursor);
        else
            written = snprintf(args, args_size, "{}");
        if (written < 0 || (size_t)written >= args_size)
            return 0;
        return LE_BLE_JOB_POLL;
    }
    return 0;
}

/* One fixed-width record: 12 address, 2 type, 2 rssi, 2 length, 2*length data. */
static size_t encode_record(const char *hex, size_t hex_length,
                            unsigned char *out, size_t out_size,
                            size_t *consumed)
{
    unsigned char address[6], type, rssi, length, data[31], nested[64];
    struct sink body = { nested, sizeof(nested), 0, 0 };
    struct sink frame = { out, out_size, 0, 0 };
    uint32_t zigzag;
    int32_t signed_rssi;
    uint64_t address_value = 0;
    size_t i;

    if (hex_length < 18)
        return 0;
    for (i = 0; i < 6; i++) {
        if (hex_byte(hex + 2 * i, &address[i]) != 0)
            return 0;
        address_value = (address_value << 8) | address[i];
    }
    if (hex_byte(hex + 12, &type) != 0 || hex_byte(hex + 14, &rssi) != 0 ||
        hex_byte(hex + 16, &length) != 0 || length > sizeof(data))
        return 0;
    if (hex_length < 18 + 2 * (size_t)length)
        return 0;
    for (i = 0; i < length; i++) {
        if (hex_byte(hex + 18 + 2 * i, &data[i]) != 0)
            return 0;
    }
    if (type > 1)
        return 0;

    /* BluetoothLERawAdvertisement: 1 address uint64, 2 rssi sint32,
     * 3 address_type uint32, 4 data bytes. */
    put_tag(&body, 1, 0);
    put_varint(&body, address_value);
    signed_rssi = (int8_t)rssi;
    zigzag = ((uint32_t)signed_rssi << 1) ^ (uint32_t)(signed_rssi >> 31);
    put_tag(&body, 2, 0);
    put_varint(&body, zigzag);
    put_tag(&body, 3, 0);
    put_varint(&body, type);
    put_tag(&body, 4, 2);
    put_varint(&body, length);
    put_bytes(&body, data, length);
    if (body.error)
        return 0;

    /* BluetoothLERawAdvertisementsResponse.advertisements, repeated field 1. */
    put_tag(&frame, 1, 2);
    put_varint(&frame, body.length);
    put_bytes(&frame, nested, body.length);
    if (frame.error)
        return 0;
    *consumed = 18 + 2 * (size_t)length;
    return frame.length;
}

size_t le_ble_proxy_poll_result(struct le_ble_proxy *proxy,
                                const char *line, size_t line_length,
                                unsigned char *out, size_t out_size)
{
    static char records[LE_BLE_RECORDS_MAX];
    unsigned int last = 0;
    size_t used = 0, consumed = 0, total = 0;
    int got;

    (void)line_length;
    if (proxy->owner < 0 || !out)
        return 0;
    if (json_get_uint(line, "last", &last) != 1)
        return 0;
    got = json_get_string(line, "records", records, sizeof(records));
    if (got == 1) {
        size_t length = strlen(records);

        /* A short or odd hex string is a daemon bug: stop at the first
         * record that does not parse, keep what was encoded before it. */
        while (used < length) {
            size_t written = encode_record(records + used, length - used,
                                           out + total, out_size - total,
                                           &consumed);
            if (written == 0 || consumed == 0)
                break;
            total += written;
            used += consumed;
        }
    }
    proxy->cursor = (uint32_t)last;
    proxy->cursor_valid = 1;
    return total;
}
