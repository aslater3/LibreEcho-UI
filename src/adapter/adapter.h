#ifndef LE_ADAPTER_H
#define LE_ADAPTER_H

/*
 * LibreEcho companion-daemon protocol.
 *
 * Each hardware domain (audio, LED, network, wake-word) is owned by a small
 * companion daemon that exposes a versioned JSON protocol over a fixed
 * AF_UNIX socket.  The web daemon (backend_linux.c) is a client only; it
 * never touches hardware directly.
 *
 * Wire format: one JSON object per line, newline-terminated, UTF-8.
 *   Request:  {"v":1,"id":N,"cmd":"...","args":{...}}\n
 *   Response: {"v":1,"id":N,"ok":true,"data":{...}}\n
 *             {"v":1,"id":N,"ok":false,"error":"..."}\n
 *   Event:    {"v":1,"event":"...","data":{...}}\n   (server-pushed, id=0)
 *
 * Limits: 4 KiB max message, 8-byte-aligned, no embedded NULs.
 */

#include <stddef.h>
#include <stdint.h>

#define LE_ADAPTER_PROTO_VERSION 1
#define LE_ADAPTER_MSG_MAX       4096
#define LE_ADAPTER_PATH_MAX      128

/* Default socket paths (overridable via environment for testing). */
#define LE_ADAPTER_NETWORK_SOCK  "/run/libreecho/network.sock"
#define LE_ADAPTER_AUDIO_SOCK    "/run/libreecho/audio.sock"
#define LE_ADAPTER_LED_SOCK      "/run/libreecho/led.sock"
#define LE_ADAPTER_WAKEWORD_SOCK "/run/libreecho/wakeword.sock"
#define LE_ADAPTER_BLUETOOTH_SOCK "/run/libreecho/bluetooth.sock"

/* Result codes shared between client and daemon sides. */
enum le_adapter_result {
    LE_ADAPTER_OK = 0,
    LE_ADAPTER_ERR_CONNECT = -1,   /* socket missing or refused */
    LE_ADAPTER_ERR_IO = -2,        /* short read/write, broken pipe */
    LE_ADAPTER_ERR_TIMEOUT = -3,   /* no response within deadline */
    LE_ADAPTER_ERR_PROTO = -4,     /* malformed JSON, version mismatch */
    LE_ADAPTER_ERR_REJECTED = -5,  /* daemon returned ok:false */
    LE_ADAPTER_ERR_NOMEM = -6
};

/*
 * Client handle.  Opaque; created by le_adapter_connect(), freed by
 * le_adapter_close().  Not thread-safe; one handle per calling thread.
 */
struct le_adapter;

/* Connect to a companion daemon socket.  Returns NULL on failure. */
struct le_adapter *le_adapter_connect(const char *sock_path, int timeout_ms);
void le_adapter_close(struct le_adapter *a);

/*
 * Send a command and wait for the matching response.
 *   cmd       – command name (e.g. "scan", "connect", "status")
 *   args_json – JSON object string for "args", or NULL for {}
 *   out       – caller buffer for the "data" object (may be NULL)
 *   out_size  – size of out
 * Returns LE_ADAPTER_OK on success, negative error otherwise.
 * On LE_ADAPTER_ERR_REJECTED, out contains the error string if provided.
 */
int le_adapter_call(struct le_adapter *a, const char *cmd,
                    const char *args_json, char *out, size_t out_size);

/*
 * Server-side helpers (used by companion daemons).
 */

/* Create, bind, listen on a Unix socket.  Returns fd or -1. */
int le_adapter_listen(const char *sock_path);

/* Accept one client (blocking).  Returns fd or -1. */
int le_adapter_accept(int listen_fd);

/*
 * Parse a request line.  Fills cmd (up to cmd_size), args pointer (into
 * msg buffer, NUL-terminated in place), and request id.
 * Returns 0 on success, -1 on malformed input.
 */
int le_adapter_parse_request(char *msg, char *cmd, size_t cmd_size,
                             char **args, unsigned long *id);

/* Format a success response into buf.  Returns bytes written. */
int le_adapter_respond_ok(char *buf, size_t size, unsigned long id,
                          const char *data_json);

/* Format an error response into buf.  Returns bytes written. */
int le_adapter_respond_err(char *buf, size_t size, unsigned long id,
                           const char *error_msg);

/* Format a server-pushed event into buf.  Returns bytes written. */
int le_adapter_format_event(char *buf, size_t size, const char *event_type,
                            const char *data_json);

#endif /* LE_ADAPTER_H */
