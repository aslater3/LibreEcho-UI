/*
 * Wall-clock state for Alexa Gadgets.  See bt_gadget.h.
 */
#include "adapter/bt_gadget.h"
#include "adapter/bt_agt.h"

#include <string.h>

void le_gadget_init(struct le_gadget *gadget)
{
    if (gadget)
        memset(gadget, 0, sizeof(*gadget));
}

int le_gadget_set_timeinfo(struct le_gadget *gadget, const char *iso_local)
{
    size_t length;

    if (!gadget || !iso_local)
        return -1;
    length = strlen(iso_local);
    if (length == 0 || length >= sizeof(gadget->timeinfo))
        return -1;
    memcpy(gadget->timeinfo, iso_local, length + 1);
    gadget->timeinfo_valid = 1;
    return 0;
}

static struct le_gadget_alert *find_alert(struct le_gadget *gadget,
                                          const char *token)
{
    size_t i;

    for (i = 0; i < LE_GADGET_MAX_ALERTS; ++i)
        if (gadget->alerts[i].in_use &&
            strcmp(gadget->alerts[i].token, token) == 0)
            return &gadget->alerts[i];
    return NULL;
}

/* Copy `value` into a fixed buffer, rejecting anything that cannot fit. */
static int copy_field(char *out, size_t out_size, const char *value)
{
    size_t length;

    if (!value)
        return -1;
    length = strlen(value);
    if (length == 0 || length >= out_size)
        return -1;
    memcpy(out, value, length + 1);
    return 0;
}

int le_gadget_set_alert(struct le_gadget *gadget, const char *token,
                        const char *type, const char *scheduled_time)
{
    struct le_gadget_alert *alert;
    size_t i;

    if (!gadget || !token || !type || !scheduled_time)
        return -1;
    if (strlen(token) >= LE_GADGET_TOKEN_MAX)
        return -1;
    alert = find_alert(gadget, token);
    if (!alert) {
        for (i = 0; i < LE_GADGET_MAX_ALERTS; ++i)
            if (!gadget->alerts[i].in_use) {
                alert = &gadget->alerts[i];
                break;
            }
        if (!alert)
            return -1;
        memset(alert, 0, sizeof(*alert));
        alert->in_use = 1;
        if (copy_field(alert->token, sizeof(alert->token), token) < 0)
            return -1;
    }
    if (copy_field(alert->type, sizeof(alert->type), type) < 0 ||
        copy_field(alert->scheduled_time, sizeof(alert->scheduled_time),
                   scheduled_time) < 0)
        return -1;
    return 0;
}

int le_gadget_delete_alert(struct le_gadget *gadget, const char *token)
{
    struct le_gadget_alert *alert;

    if (!gadget || !token)
        return -1;
    alert = find_alert(gadget, token);
    if (!alert)
        return -1;
    memset(alert, 0, sizeof(*alert));
    return 0;
}

int le_gadget_set_timers_active(struct le_gadget *gadget, int active)
{
    if (!gadget)
        return 0;
    /* A real Echo sends `active` once per ring and `cleared` afterwards. */
    if (active) {
        if (gadget->timers_announced)
            return 0;
        gadget->timers_announced = 1;
        return 1;
    }
    if (!gadget->timers_announced)
        return 0;
    gadget->timers_announced = 0;
    return 1;
}

/* Append one already-built message as a frame; returns the new length. */
static size_t append_frame(struct le_gadget *gadget, uint8_t *frames,
                           size_t frames_size, size_t used,
                           const uint8_t *message, size_t message_length)
{
    size_t frame_length;

    if (!message_length)
        return used;
    if (++gadget->seq > 0xEF)
        gadget->seq = 1;
    frame_length = le_agt_encode(frames + used, frames_size - used, gadget->seq,
                                message, message_length);
    return frame_length ? used + frame_length : used;
}

size_t le_gadget_build_connect_frames(struct le_gadget *gadget, uint8_t *frames,
                                      size_t frames_size)
{
    uint8_t message[LE_AGT_FRAME_MAX];
    size_t used = 0;

    if (!gadget || !frames || !frames_size)
        return 0;
    used = append_frame(gadget, frames, frames_size, used, message,
                        le_agt_build_discover(message, sizeof(message)));
    if (gadget->timeinfo_valid)
        used = append_frame(gadget, frames, frames_size, used, message,
                            le_agt_build_state_update(message, sizeof(message),
                                                      "timeinfo",
                                                      gadget->timeinfo));
    used = append_frame(gadget, frames, frames_size, used, message,
                        le_agt_build_clear_indicator(message, sizeof(message)));
    return used;
}

size_t le_gadget_build_alert_frames(const struct le_gadget *gadget,
                                    uint8_t *frames, size_t frames_size)
{
    uint8_t message[LE_AGT_FRAME_MAX];
    struct le_gadget *mutable_gadget = (struct le_gadget *)gadget;
    size_t used = 0;
    size_t i;

    if (!gadget || !frames || !frames_size)
        return 0;
    for (i = 0; i < LE_GADGET_MAX_ALERTS; ++i) {
        const struct le_gadget_alert *alert = &gadget->alerts[i];
        size_t message_length;

        if (!alert->in_use)
            continue;
        message_length = le_agt_build_set_alert(
            message, sizeof(message), alert->token, alert->type,
            alert->scheduled_time);
        used = append_frame(mutable_gadget, frames, frames_size, used, message,
                            message_length);
    }
    return used;
}
