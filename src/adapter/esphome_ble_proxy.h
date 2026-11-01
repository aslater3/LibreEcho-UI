#ifndef LIBREECHO_ESPHOME_BLE_PROXY_H
#define LIBREECHO_ESPHOME_BLE_PROXY_H

#include <stddef.h>
#include <stdint.h>

/*
 * ESPHome Bluetooth proxy, passive raw-advertisement slice.
 *
 * One Home Assistant client may subscribe with SubscribeBluetoothLEAdvertisements
 * (message 66, flags bit 0 = raw advertisements).  While subscribed, esphomed
 * keeps a short "proxy_scan" lease on btd and drains "adverts" batches, which
 * it re-encodes as BluetoothLERawAdvertisementsResponse (message 93).
 *
 * Pure logic only: no sockets, no clocks, no client table.  esphomed.c owns
 * the transport and passes the monotonic time in.  Field numbers and flag
 * values follow ESPHome api.proto (aioesphomeapi 46.2.0).
 */

#define LE_BLE_PROXY_FEATURES     33U   /* PASSIVE_SCAN | RAW_ADVERTISEMENTS */
#define LE_BLE_PROXY_SUB_RAW      1U    /* SubscriptionFlag RAW_ADVERTISEMENTS */
#define LE_BLE_PROXY_MSG_SUBSCRIBE 66
#define LE_BLE_PROXY_MSG_RAW_ADVS  93

/* Job kinds that esphomed.c uses for btd requests (distinct from its own). */
#define LE_BLE_JOB_LEASE 9
#define LE_BLE_JOB_POLL  10

#define LE_BLE_LEASE_TTL_MS 5000U
#define LE_BLE_LEASE_EVERY_MS 1000U
#define LE_BLE_POLL_EVERY_MS 250U
#define LE_BLE_RECORDS_MAX  2048U  /* hex string budget for one batch */

struct le_ble_proxy {
    int owner;               /* subscribed client index, -1 when none */
    int cursor_valid;
    int stop_pending;
    uint32_t cursor;         /* btd "last" seq to pass as after= */
    uint64_t lease_due_ms;
    uint64_t poll_due_ms;
};

void le_ble_proxy_init(struct le_ble_proxy *proxy);

/* Handle a SubscribeBluetoothLEAdvertisementsRequest from client.  Returns 0,
 * or -1 when the request is malformed or another client already owns the
 * subscription (the caller then closes that client). */
int le_ble_proxy_subscribe(struct le_ble_proxy *proxy, int client,
                           const void *msg, size_t length);
void le_ble_proxy_close(struct le_ble_proxy *proxy, int client);

/* Next btd request that is due at now_ms: LE_BLE_JOB_LEASE with args for
 * "proxy_scan", LE_BLE_JOB_POLL with args for "adverts", or 0 for none.
 * args receives the JSON object text (NUL-terminated). Call again until 0. */
int le_ble_proxy_due(struct le_ble_proxy *proxy, uint64_t now_ms,
                     char *args, size_t args_size);

/* Consume a successful "adverts" response line and encode the records it
 * carries as a BluetoothLERawAdvertisementsResponse body in out.  Returns the
 * body length, or 0 when there is nothing to send. Advances the cursor. */
size_t le_ble_proxy_poll_result(struct le_ble_proxy *proxy,
                                const char *line, size_t line_length,
                                unsigned char *out, size_t out_size);

#endif
