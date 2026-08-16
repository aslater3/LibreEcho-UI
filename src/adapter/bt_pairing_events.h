#ifndef LIBREECHO_BT_PAIRING_EVENTS_H
#define LIBREECHO_BT_PAIRING_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#define LE_BT_MGMT_EV_USER_CONFIRM_REQUEST 0x000f

int le_bt_pairing_event_value(uint16_t event, const uint8_t *payload,
                              size_t size, uint32_t *value);

#endif
