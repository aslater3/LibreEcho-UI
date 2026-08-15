/*
 * Bounded parsers for MGMT connection-event payloads.  See bt_mgmt_events.h
 * for the wire layout.  Issue #62: these make pairing/connection triage
 * possible from the daemon status instead of requiring an out-of-band raw
 * HCI monitor.
 */
#include "bt_mgmt_events.h"

#include <stdio.h>
#include <string.h>

const char *le_bt_mgmt_disconnect_reason_name(unsigned int reason)
{
    switch (reason) {
    case 0x00: return "unknown";
    case 0x01: return "timeout";
    case 0x02: return "local-host-terminated";
    case 0x03: return "remote-terminated";
    case 0x04: return "authentication-failed";
    case 0x05: return "authentication-rejected";
    case 0x06: return "local-unsupported-features";
    case 0x07: return "unsupported-remote-features";
    case 0x08: return "profile-unsupported";
    case 0x09: return "connection-parameters-rejected";
    case 0x0a: return "sync-connection-terminated";
    case 0x0b: return "connection-failed";
    default: return "unrecognized-reason";
    }
}

static const char *mgmt_status_text(unsigned int status)
{
    switch (status) {
    case 0x00: return "success";
    case 0x01: return "unknown-command";
    case 0x02: return "not-connected";
    case 0x03: return "failed";
    case 0x04: return "connect-failed";
    case 0x05: return "authentication-failed";
    case 0x06: return "not-paired";
    case 0x07: return "no-resources";
    case 0x08: return "timeout";
    case 0x09: return "already-connected";
    case 0x0a: return "busy";
    case 0x0b: return "rejected";
    case 0x0c: return "not-supported";
    case 0x0d: return "invalid-params";
    case 0x0e: return "disconnected";
    case 0x0f: return "not-powered";
    case 0x10: return "cancelled";
    case 0x11: return "invalid-index";
    case 0x12: return "rfkilled";
    case 0x13: return "already-paired";
    case 0x14: return "permission-denied";
    default: return "unrecognized-status";
    }
}

static int render_event_code_text(const char *name, unsigned int code,
                                  char *out, size_t out_size)
{
    int written;

    if (!name || !out || out_size == 0)
        return -1;
    written = snprintf(out, out_size, "%s (0x%02x)", name, code);
    if (written < 0 || (size_t)written >= out_size)
        return -1;
    return 0;
}

int le_bt_mgmt_disconnect_reason_text(const uint8_t *payload, size_t size,
                                      char *out, size_t out_size)
{
    if (!payload || size < LE_BT_MGMT_ADDR_INFO_SIZE + 1)
        return -1;
    return render_event_code_text(
        le_bt_mgmt_disconnect_reason_name(payload[LE_BT_MGMT_ADDR_INFO_SIZE]),
        payload[LE_BT_MGMT_ADDR_INFO_SIZE], out, out_size);
}

int le_bt_mgmt_connect_failed_text(const uint8_t *payload, size_t size,
                                   char *out, size_t out_size)
{
    if (!payload || size < LE_BT_MGMT_ADDR_INFO_SIZE + 1)
        return -1;
    return render_event_code_text(
        mgmt_status_text(payload[LE_BT_MGMT_ADDR_INFO_SIZE]),
        payload[LE_BT_MGMT_ADDR_INFO_SIZE], out, out_size);
}
