#ifndef LE_LOG_H
#define LE_LOG_H

/*
 * LibreEcho shared logging.
 *
 * Minimal, dependency-free, async-signal-safe logging for all daemons.
 * Output goes to stderr by default; optionally to syslog when daemonized.
 *
 * Levels: DEBUG < INFO < WARNING < ERROR
 * Default threshold: INFO (or WARNING if --quiet).
 * --verbose sets DEBUG, --debug sets DEBUG + includes source location.
 *
 * Usage:
 *   le_log_init("networkd", argc, argv);  // parses --verbose/--debug/--quiet
 *   le_log(LOG_INFO, "listening on %s", path);
 *   le_log_debug("wpa_ctrl: sent %s", cmd);
 */

enum le_log_level {
    LE_LOG_DEBUG = 0,
    LE_LOG_INFO = 1,
    LE_LOG_WARNING = 2,
    LE_LOG_ERROR = 3
};

/*
 * Initialize logging.  Call once at startup.
 *   ident   – daemon name (used as prefix / syslog ident)
 *   argc/argv – scanned for --verbose, --debug, --quiet, --syslog
 *               (unrecognized args are left for the caller's own parser)
 * Returns the index of the first non-logging argument (for re-parsing),
 * or just call before your own arg loop and ignore the return.
 */
void le_log_init(const char *ident, int argc, char **argv);

/* Set threshold directly (if not using le_log_init arg parsing). */
void le_log_set_level(enum le_log_level level);

/* Enable source file:line in debug output. */
void le_log_set_source_info(int enabled);

/* Core log function. */
void le_log(enum le_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros with automatic source location in debug mode. */
#define le_log_debug(...)   le_log(LE_LOG_DEBUG, __VA_ARGS__)
#define le_log_info(...)    le_log(LE_LOG_INFO, __VA_ARGS__)
#define le_log_warn(...)    le_log(LE_LOG_WARNING, __VA_ARGS__)
#define le_log_error(...)   le_log(LE_LOG_ERROR, __VA_ARGS__)

/* Log with errno appended (for failed syscalls). */
void le_log_perror(enum le_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define le_log_pdebug(...)  le_log_perror(LE_LOG_DEBUG, __VA_ARGS__)
#define le_log_pwarn(...)   le_log_perror(LE_LOG_WARNING, __VA_ARGS__)
#define le_log_perr(...)    le_log_perror(LE_LOG_ERROR, __VA_ARGS__)

/* Query current threshold (useful to skip expensive debug formatting). */
int le_log_would_emit(enum le_log_level level);

#endif /* LE_LOG_H */
