#ifndef LIBREECHO_BT_ADV_PROXY_H
#define LIBREECHO_BT_ADV_PROXY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Passive BLE advertisement relay state for btd.
 *
 * esphomed holds a short lease ("proxy_scan") while a Home Assistant client
 * subscribes to raw advertisements, then drains a bounded ring ("adverts").
 * Records are low-energy only: BR/EDR inquiry results never enter the ring,
 * and advertisements longer than the legacy 31-byte payload are counted as
 * dropped instead of truncated.  Everything is fixed-size; no allocation.
 */

#define BT_ADV_RING        32U
#define BT_ADV_DATA_MAX    31U
#define BT_ADV_BATCH       16U
#define BT_ADV_LEASE_MAX_MS 30000U

struct bt_adv_record {
    uint32_t seq;
    uint8_t address[6];      /* display order, most significant byte first */
    uint8_t addr_type;       /* 0 public, 1 random (ESPHome numbering) */
    int8_t rssi;
    uint8_t length;
    uint8_t data[BT_ADV_DATA_MAX];
};

struct bt_adv_ring {
    struct bt_adv_record records[BT_ADV_RING];
    uint32_t last_seq;       /* seq of the newest record; 0 = none yet */
    uint32_t dropped;        /* oversized LE advertisements refused */
    uint64_t lease_until_ms; /* monotonic; 0 = no lease */
};

void bt_adv_ring_init(struct bt_adv_ring *ring);

/* Extend the lease to now + ttl_ms, clamped to BT_ADV_LEASE_MAX_MS.
 * ttl_ms == 0 ends it at once. */
void bt_adv_lease(struct bt_adv_ring *ring, uint64_t now_ms, unsigned ttl_ms);
int bt_adv_leased(const struct bt_adv_ring *ring, uint64_t now_ms);

/*
 * Record one DEVICE_FOUND advertisement.  mgmt_addr_type is the kernel MGMT
 * value (0 BR/EDR, 1 LE public, 2 LE random); address is in MGMT payload
 * order (least significant byte first).  Returns 1 when stored, 0 when
 * ignored (BR/EDR), -1 when refused as oversized.
 */
int bt_adv_push(struct bt_adv_ring *ring, const uint8_t *address,
                uint8_t mgmt_addr_type, int rssi, const uint8_t *eir,
                size_t eir_length);

/*
 * Render the body of an "adverts" reply: records with seq > after, oldest
 * first, at most BT_ADV_BATCH.  Output is a JSON object:
 *   {"last":N,"gap":0|1,"dropped":N,"records":"<hex>"}
 * where each record is 12 hex address digits, 2 hex addr type, 2 hex RSSI
 * (two's complement), 2 hex length, then 2*length hex data digits.
 * "last" is the seq to pass as the next "after".  gap is 1 when records
 * between after and the oldest retained record were overwritten.
 * Returns the number of bytes written, or -1 if out is too small.
 */
int bt_adv_query(const struct bt_adv_ring *ring, uint32_t after,
                 char *out, size_t out_size);

#endif
