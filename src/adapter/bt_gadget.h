/*
 * Wall-clock state that btd pushes to a connected Alexa Gadget.
 *
 * The clock is a dumb display: the partner owns the truth and mirrors it.
 * This module keeps that state and renders the directives a real Echo sends
 * (see bt_agt.h), so btd only has to call it when something changes and
 * write the resulting frames.
 *
 * Wire behaviour reproduced from a real Echo (PROTOCOL.md section 5b):
 *   connect           Discover -> (Discover.Response) -> timeinfo -> ClearIndicator
 *   timer set/resumed SetAlert(token, TIMER, end-time)
 *   timer cancelled   DeleteAlert(token)
 *   timer ringing     StateUpdate timers=active, once
 *   ringing stopped   StateUpdate timers=cleared + DeleteAlert(token)
 *   half hour         timeinfo
 */
#ifndef LIBREECHO_BT_GADGET_H
#define LIBREECHO_BT_GADGET_H

#include <stddef.h>
#include <stdint.h>

#define LE_GADGET_MAX_ALERTS 8
#define LE_GADGET_TOKEN_MAX 16
#define LE_GADGET_TIME_MAX 40

struct le_gadget_alert {
    int in_use;
    char token[LE_GADGET_TOKEN_MAX];
    char type[12];                        /* TIMER / ALARM / REMINDER */
    char scheduled_time[LE_GADGET_TIME_MAX]; /* local ISO-8601 with offset */
};

struct le_gadget {
    struct le_gadget_alert alerts[LE_GADGET_MAX_ALERTS];
    uint8_t seq;            /* next AGT frame sequence number */
    char timeinfo[LE_GADGET_TIME_MAX];
    int timeinfo_valid;
    int timers_announced;   /* StateUpdate timers=active already sent */
    /* Local copy of the clock zone, e.g. "Europe/Zurich"; UI/CLI only. */
    char zone[64];
};

void le_gadget_init(struct le_gadget *gadget);

/* Set the time to report.  `iso_local` is local ISO-8601 with offset. */
int le_gadget_set_timeinfo(struct le_gadget *gadget, const char *iso_local);

/* Add or replace an alert by token.  Returns 0 on success. */
int le_gadget_set_alert(struct le_gadget *gadget, const char *token,
                        const char *type, const char *scheduled_time);

/* Remove an alert.  Returns 0 when it existed. */
int le_gadget_delete_alert(struct le_gadget *gadget, const char *token);

/* Clear every alert, e.g. before replacing the set from timerd. */
void le_gadget_clear_alerts(struct le_gadget *gadget);

/* Mark the ringing state; returns 1 when a StateUpdate is due. */
int le_gadget_set_timers_active(struct le_gadget *gadget, int active);

/*
 * Render the connect-time directives (timeinfo + ClearIndicator) into
 * `frames`, a sequence of complete AGT frames.  Returns the byte length, or
 * 0 when no time is known yet.  Each frame is written back to back; the
 * caller writes the whole buffer.
 */
size_t le_gadget_build_connect_frames(struct le_gadget *gadget, uint8_t *frames,
                                      size_t frames_size);

/*
 * Render the directives for every active alert (used after connect so the
 * clock shows timers it missed).  Returns the byte length.
 */
size_t le_gadget_build_alert_frames(const struct le_gadget *gadget,
                                    uint8_t *frames, size_t frames_size);

#endif /* LIBREECHO_BT_GADGET_H */
