#ifndef LIBREECHO_LIVE_HA_H
#define LIBREECHO_LIVE_HA_H

/*
 * Bounded Home Assistant REST client for GPT-Live delegation.
 *
 * lived talks to Home Assistant only through the two fixed calls the
 * allow-listed ha.* tools need: read one entity state and call one service on
 * one entity.  The client is deliberately small: plain http:// only (the
 * long-lived token is never sent over an unencrypted hop to a non-loopback
 * peer unless the owner set that URL), one request per connection, a fixed
 * response buffer, and a hard timeout on every socket operation.
 *
 * The token is read from a file the owner provisions (mode 0600, under
 * /data/libreecho/secrets/).  It is never logged, never placed in a tool
 * result, and never written anywhere else by this module.
 */

#include <stddef.h>

#define LE_LIVE_HA_TOKEN_MAX 256
#define LE_LIVE_HA_BODY_MAX 2048
#define LE_LIVE_HA_URL_MAX 160

struct le_live_ha_response {
    int status;
    size_t length;
    char body[LE_LIVE_HA_BODY_MAX];
};

/*
 * Read the long-lived token from `path`.  Returns 0 when the file holds a
 * token of plausible shape (letters, digits, '.', '_', '-'; 16 to 255 bytes,
 * surrounding whitespace removed), -1 otherwise.  The caller's buffer is
 * cleared on failure.
 */
int le_live_ha_load_token(const char *path, char *token, size_t size);

/*
 * Perform one HTTP/1.1 request against `base_url` (http://host[:port], origin
 * only).  Returns 0 when a complete response with a status line was received
 * (inspect response->status), -1 on any transport failure: bad URL, DNS or
 * connect failure, timeout, oversized or malformed response.  `body` may be
 * NULL.  The token, when non-empty, is sent as a bearer credential.
 */
int le_live_ha_request(const char *base_url, const char *token,
                       const char *method, const char *path, const char *body,
                       int timeout_ms, struct le_live_ha_response *response);

#endif
