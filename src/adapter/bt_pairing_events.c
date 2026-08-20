#include "bt_pairing_events.h"

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

int le_bt_pairing_event_value(uint16_t event, const uint8_t *payload,
                              size_t size, uint32_t *value)
{
    size_t offset = event == LE_BT_MGMT_EV_USER_CONFIRM_REQUEST ? 8U : 7U;
    size_t required = offset + sizeof(uint32_t);

    if (!payload || !value || size < required)
        return -1;
    *value = read_le32(payload + offset);
    return 0;
}
