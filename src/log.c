#include "log.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef LE_LOG_SYSLOG
#include <syslog.h>
#endif

static enum le_log_level threshold = LE_LOG_INFO;
static int show_source = 0;
static int use_syslog = 0;
static char ident_buf[32] = "libreecho";

void le_log_init(const char *ident, int argc, char **argv)
{
    int i;
    if (ident) {
        size_t n = strlen(ident);
        if (n >= sizeof(ident_buf)) n = sizeof(ident_buf) - 1;
        memcpy(ident_buf, ident, n);
        ident_buf[n] = '\0';
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose"))
            threshold = LE_LOG_DEBUG;
        else if (!strcmp(argv[i], "--debug")) {
            threshold = LE_LOG_DEBUG;
            show_source = 1;
        } else if (!strcmp(argv[i], "--quiet"))
            threshold = LE_LOG_WARNING;
        else if (!strcmp(argv[i], "--syslog"))
            use_syslog = 1;
    }
#ifdef LE_LOG_SYSLOG
    if (use_syslog)
        openlog(ident_buf, LOG_PID | LOG_NDELAY, LOG_DAEMON);
#else
    (void)use_syslog;
#endif
}

void le_log_set_level(enum le_log_level level) { threshold = level; }
void le_log_set_source_info(int enabled) { show_source = enabled; }

int le_log_would_emit(enum le_log_level level) { return level >= threshold; }

static const char *level_str(enum le_log_level level)
{
    switch (level) {
    case LE_LOG_DEBUG:   return "DEBUG";
    case LE_LOG_INFO:    return "INFO";
    case LE_LOG_WARNING: return "WARN";
    case LE_LOG_ERROR:   return "ERROR";
    }
    return "?";
}

#ifdef LE_LOG_SYSLOG
static int level_to_syslog(enum le_log_level level)
{
    switch (level) {
    case LE_LOG_DEBUG:   return LOG_DEBUG;
    case LE_LOG_INFO:    return LOG_INFO;
    case LE_LOG_WARNING: return LOG_WARNING;
    case LE_LOG_ERROR:   return LOG_ERR;
    }
    return LOG_INFO;
}
#endif

void le_log(enum le_log_level level, const char *fmt, ...)
{
    va_list ap;
    char msg[1024];

    if (level < threshold)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

#ifdef LE_LOG_SYSLOG
    if (use_syslog) {
        syslog(level_to_syslog(level), "%s", msg);
        if (level >= LE_LOG_ERROR) {
            /* Also mirror errors to stderr for visibility */
            fprintf(stderr, "%s [%s] %s\n", ident_buf, level_str(level), msg);
        }
        return;
    }
#endif

    if (show_source && level == LE_LOG_DEBUG) {
        fprintf(stderr, "%s [%s] %s\n", ident_buf, level_str(level), msg);
    } else {
        struct timespec ts;
        struct tm tm;
        char stamp[32];
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        fprintf(stderr, "%s.%03d %s [%s] %s\n",
                stamp, (int)(ts.tv_nsec / 1000000),
                ident_buf, level_str(level), msg);
    }
    fflush(stderr);
}

void le_log_perror(enum le_log_level level, const char *fmt, ...)
{
    va_list ap;
    char msg[1024];
    int saved_errno = errno;

    if (level < threshold)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    le_log(level, "%s: %s", msg, strerror(saved_errno));
}
