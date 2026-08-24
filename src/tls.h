/*
 * TLS for the parts of LibreEcho that reach the network.
 *
 * Two consumers with different trust needs, so they are separate calls rather
 * than one "make it secure" entry point:
 *
 *   - radiod fetching an https:// stream. There is no CA bundle on this image
 *     and no way to keep one current, so certificates cannot be verified
 *     against a trust store. The connection is encrypted but the peer is not
 *     authenticated, and le_tls_client_verified() says so rather than letting
 *     a caller assume otherwise.
 *
 *   - the web server presenting a self-signed certificate generated on the
 *     device. Self-signed means browsers will warn on first visit; the point
 *     is to stop the password and session token crossing the LAN in clear,
 *     not to prove identity to a third party.
 */
#ifndef LIBREECHO_TLS_H
#define LIBREECHO_TLS_H

#include <stddef.h>

struct le_tls;

/* Wrap an already-connected socket as a TLS client. hostname is used for SNI.
   Returns NULL on failure; the caller still owns fd and must close it. */
struct le_tls *le_tls_client_open(int fd, const char *hostname);

/*
 * Accept a TLS connection on an already-accepted socket, presenting the
 * certificate and key at the given paths. Returns NULL if the handshake
 * fails or the files cannot be loaded. The caller still owns fd.
 */
struct le_tls *le_tls_server_open(int fd, const char *cert_path,
                                  const char *key_path);

/* Whether the peer certificate chained to a trusted root. Always 0 on this
   image -- see the note above. Exposed so callers can report honestly. */
int le_tls_client_verified(const struct le_tls *tls);

/* Same shape as read(2)/write(2): bytes moved, 0 on clean close, -1 on error. */
long le_tls_read(struct le_tls *tls, void *buf, size_t len);

/*
 * Bytes already decrypted and buffered inside the TLS layer. poll() only sees
 * the socket, so a relay that ignores this can stall with a full request
 * sitting in the buffer.
 */
int le_tls_pending(struct le_tls *tls);
long le_tls_write(struct le_tls *tls, const void *buf, size_t len);
long le_tls_write_deadline(struct le_tls *tls, const void *buf, size_t len,
                           int timeout_ms);

void le_tls_close(struct le_tls *tls);

/*
 * Ensure a self-signed certificate and key exist at the given paths,
 * generating them if absent. Returns LE_OK on success. The key is written
 * 0600; both live beside the rest of the device configuration.
 */
/*
 * Read back facts about a generated certificate for display. The SHA-256
 * fingerprint is the only way a user can confirm the certificate their browser
 * warns about is the one this device made, so it is worth surfacing.
 * Returns LE_OK on success. Any out pointer may be NULL.
 */
int le_tls_cert_info(const char *cert_path, char *not_after, size_t na_size,
                     char *fingerprint, size_t fp_size);

int le_tls_ensure_self_signed(const char *cert_path, const char *key_path,
                              const char *common_name);

#endif
