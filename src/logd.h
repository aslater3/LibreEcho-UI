#ifndef LE_LOGD_H
#define LE_LOGD_H

/*
 * LibreEcho central log daemon protocol.
 *
 * All services send log entries to logd via a SOCK_DGRAM Unix socket.
 * Fire-and-forget: if the socket is unavailable, the entry is dropped
 * (or falls back to stderr depending on le_log configuration).
 *
 * Wire format: single JSON line, newline-terminated, max 1024 bytes.
 *   {"ts":1721476800,"level":"info","service":"networkd","msg":"scan requested"}
 *
 * logd writes to /var/log/libreecho/system.log in the same JSON-lines format.
 * Rotation: when the file exceeds LOGD_MAX_SIZE, rename to .1, .2, etc.
 *
 * The web daemon reads the log file directly for the /api/v1/logs endpoint.
 */

#define LE_LOGD_SOCK       "/run/libreecho/log.sock"
#define LE_LOGD_DIR        "/var/log/libreecho"
#define LE_LOGD_FILE       "/var/log/libreecho/system.log"
#define LE_LOGD_MAX_SIZE   (512 * 1024)   /* 512 KiB per file */
#define LE_LOGD_MAX_FILES  3              /* system.log, .1, .2 */
#define LE_LOGD_MSG_MAX    1024

#endif /* LE_LOGD_H */
