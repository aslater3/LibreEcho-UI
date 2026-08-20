#ifndef LIBREECHO_BT_MGMT_EVENTS_H
#define LIBREECHO_BT_MGMT_EVENTS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure parsers for Linux Bluetooth management (MGMT) connection events.
 *
 * Wire layout shared by MGMT_EV_DEVICE_CONNECTED (0x000b),
 * MGMT_EV_DEVICE_DISCONNECTED (0x000c) and MGMT_EV_CONNECT_FAILED (0x000d):
 *
 *   payload[0..5]  remote Bluetooth address
 *   payload[6]     address type
 *   payload[7]     disconnect reason / connect-failed status (events 0x000c
 *                  and 0x000d only)
 *
 * The parsers are bounded, allocation-free, and reject undersized payloads.
 */

#define LE_BT_MGMT_ADDR_INFO_SIZE 7U

/* Human-readable names for MGMT_DEV_DISCONN_* reason codes. */
const char *le_bt_mgmt_disconnect_reason_name(unsigned int reason);

/*
 * Render a MGMT_EV_DEVICE_DISCONNECTED payload as
 * "reason-name (0xNN)".  Returns 0 on success, -1 when the payload is
 * missing, undersized for a reason byte, or out is too small.
 */
int le_bt_mgmt_disconnect_reason_text(const uint8_t *payload, size_t size,
                                      char *out, size_t out_size);

/*
 * Render a MGMT_EV_CONNECT_FAILED payload as "status-name (0xNN)" using the
 * MGMT command status code names.  Same error contract as the disconnect
 * renderer.
 */
int le_bt_mgmt_connect_failed_text(const uint8_t *payload, size_t size,
                                   char *out, size_t out_size);

#endif
