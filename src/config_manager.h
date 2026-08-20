#ifndef LE_CONFIG_MANAGER_H
#define LE_CONFIG_MANAGER_H

/*
 * LibreEcho central configuration manager.
 *
 * All services read from /etc/libreecho/config.json. Each service owns
 * a named section. The web daemon is the single writer; companion daemons
 * are read-only consumers that reload on SIGHUP.
 *
 * Config history: on every write, the previous version is saved to
 * /etc/libreecho/history/config-<timestamp>.json (last 10 kept).
 *
 * Backup/restore: the web API exposes export/import. The backup tool
 * (tools/libreecho-backup.sh) creates a tar.gz with manifest.
 */

#include <stddef.h>

#define LE_CONFIG_PATH      "/etc/libreecho/config.json"
#define LE_CONFIG_HISTORY   "/etc/libreecho/history"
#define LE_CONFIG_HISTORY_MAX 10
#define LE_CONFIG_BUF_MAX   16384

/*
 * Initialize the config manager. Reads the central config file.
 * Returns 0 on success, -1 if the file is missing or unparseable
 * (in which case defaults should be used).
 */
int le_config_init(const char *path);

/*
 * Reload configuration from disk (called on SIGHUP).
 * Returns 0 on success, -1 on parse failure (old config retained).
 */
int le_config_reload(void);

/*
 * Get the raw JSON for a named section.
 *   section: top-level key name (e.g. "audio", "led", "network", "system")
 *   out: buffer to receive the section's JSON object
 *   out_size: size of out
 * Returns 0 on success, -1 if section not found.
 */
int le_config_section(const char *section, char *out, size_t out_size);

/*
 * Typed getters that look up "section.key" in the config.
 * Return 0 on success, -1 if not found.
 */
int le_config_get_int(const char *section, const char *key, int *value);
int le_config_get_bool(const char *section, const char *key, int *value);
int le_config_get_string(const char *section, const char *key, char *out, size_t out_size);

/*
 * Write the full config (web daemon only). Saves history before writing.
 *   json: complete config JSON
 *   len: length of json
 * Returns 0 on success, -1 on failure.
 */
int le_config_write(const char *json, size_t len);

/*
 * Get the path to the config file in use.
 */
const char *le_config_path(void);

#endif /* LE_CONFIG_MANAGER_H */
