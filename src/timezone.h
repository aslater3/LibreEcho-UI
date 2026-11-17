#ifndef LE_TIMEZONE_H
#define LE_TIMEZONE_H
/*
 * The device time zone (#371).
 *
 * The image has no zoneinfo database, so the zone is a POSIX TZ string such
 * as "CST6CDT,M3.2.0,M11.1.0" stored as "timezone" in web-config.json. The
 * initramfs exports it as TZ before the service graph starts, which is how
 * every daemon learns it at boot. Its filter only lets through letters,
 * digits and "+-:,./", so angle-bracket names ("<+04>-4", which tzdata and
 * Home Assistant use for zones without a letter abbreviation) are rewritten
 * to plain letters here before they are stored; the offsets and rules are
 * unchanged, only the abbreviation strftime's %Z prints differs.
 *
 * le_timezone_follow() lets a long-running daemon pick up a change without a
 * reboot. It owns one fixed buffer that it hands to putenv() on the first
 * call and afterwards only rewrites in place, always NUL-terminated within
 * its bounds. environ is never reallocated and no string is ever freed, so a
 * concurrent getenv("TZ") inside localtime_r() can at worst see a zone mid
 * update for one call, never freed memory. Call it first from the thread
 * that runs before any other (main), then from the place that formats local
 * time; one translation unit per process.
 *
 * Header-only so the daemons that use it need no new link inputs.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* POSIX (XSI) putenv; declared here because -D_POSIX_C_SOURCE alone hides it. */
int putenv(char *);

#define LE_TIMEZONE_MAX 63
#define LE_TIMEZONE_DEFAULT_CONFIG "/data/libreecho/config/web-config.json"

static inline int le_timezone_valid(const char *tz)
{
    size_t i;

    if (!tz || !tz[0] || strlen(tz) > LE_TIMEZONE_MAX)
        return 0;
    for (i = 0; tz[i]; ++i) {
        unsigned char ch = (unsigned char)tz[i];

        if (!isalnum(ch) && ch != '+' && ch != '-' && ch != ':' &&
            ch != ',' && ch != '.' && ch != '/')
            return 0;
    }
    return 1;
}

/*
 * Rewrite <...> names to letters: the first (standard time) becomes "LST",
 * the second (daylight time) "LDT". Returns 0 and a valid zone in out, or -1
 * when the input is not something le_timezone_valid() accepts afterwards.
 */
static inline int le_timezone_normalize(const char *in, char *out, size_t size)
{
    size_t used = 0;
    int names = 0;

    if (!in || !out || size < 2)
        return -1;
    out[0] = 0;
    while (*in) {
        const char *text = NULL;
        size_t n;

        if (*in == '<') {
            const char *end = strchr(in, '>');

            if (!end || end == in + 1 || names >= 2)
                return -1;
            for (const char *p = in + 1; p < end; ++p)
                if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-')
                    return -1;
            text = names++ ? "LDT" : "LST";
            in = end + 1;
            n = 3;
        } else {
            text = in++;
            n = 1;
        }
        if (used + n >= size)
            return -1;
        memcpy(out + used, text, n);
        used += n;
        out[used] = 0;
    }
    return le_timezone_valid(out) ? 0 : -1;
}

/*
 * The value of the top-level "timezone" string in a web-config.json body,
 * or -1. The stored value is always a validated zone, so it carries no
 * quotes or escapes; anything else is refused rather than parsed.
 */
static inline int le_timezone_from_config(const char *json, char *out, size_t size)
{
    const char *p = json;
    int depth = 0;

    if (!json || !out || size < 2)
        return -1;
    for (; *p; ++p) {
        if (*p == '{' || *p == '[') {
            depth++;
            continue;
        }
        if (*p == '}' || *p == ']') {
            depth--;
            continue;
        }
        if (*p != '"')
            continue;
        {
            const char *start = p + 1, *end = start;
            size_t n;

            while (*end && *end != '"')
                end += *end == '\\' && end[1] ? 2 : 1;
            if (!*end)
                return -1;
            p = end;
            if (depth != 1 || (size_t)(end - start) != 8 ||
                memcmp(start, "timezone", 8))
                continue;
            ++end;
            while (isspace((unsigned char)*end))
                ++end;
            if (*end != ':')
                continue;
            ++end;
            while (isspace((unsigned char)*end))
                ++end;
            if (*end != '"')
                return -1;
            start = ++end;
            while (*end && *end != '"' && *end != '\\')
                ++end;
            if (*end != '"')
                return -1;
            n = (size_t)(end - start);
            if (n >= size)
                return -1;
            memcpy(out, start, n);
            out[n] = 0;
            return le_timezone_valid(out) ? 0 : -1;
        }
    }
    return -1;
}

/* The configuration this process follows: LE_TIMEZONE_CONFIG, else the device default. */
static inline const char *le_timezone_config_path(void)
{
    const char *path = getenv("LE_TIMEZONE_CONFIG");

    return path && path[0] ? path : LE_TIMEZONE_DEFAULT_CONFIG;
}

/*
 * Re-read the zone when the configuration changes and update TZ in place.
 * config_path NULL means le_timezone_config_path().
 * Checks at most once every LE_TIMEZONE_RECHECK_SECONDS; an unreadable file
 * or an invalid zone leaves the current zone alone. Returns 1 when TZ
 * changed, else 0.
 */
#ifndef LE_TIMEZONE_RECHECK_SECONDS
#define LE_TIMEZONE_RECHECK_SECONDS 10
#endif
static inline int le_timezone_follow(const char *config_path)
{
    static char env[3 + LE_TIMEZONE_MAX + 1];
    static int installed;
    static time_t checked;
    static struct timespec seen_mtime;
    static off_t seen_size = -1;
    struct timespec now_ts;
    struct stat st;
    char json[16384], zone[LE_TIMEZONE_MAX + 1];
    const char *path;
    size_t n;
    FILE *file;

    /* Take over an inherited zone straight away, so on the device (where the
     * initramfs always sets TZ) the one putenv() happens on the first call,
     * before any other thread exists. Without an inherited zone the libc
     * default stays in force until the configuration names one. */
    if (!installed) {
        const char *current = getenv("TZ");

        if (le_timezone_valid(current)) {
            snprintf(env, sizeof(env), "TZ=%s", current);
            if (putenv(env) != 0)
                return 0;
            installed = 1;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now_ts) != 0)
        return 0;
    if (checked && now_ts.tv_sec - checked < LE_TIMEZONE_RECHECK_SECONDS)
        return 0;
    checked = now_ts.tv_sec ? now_ts.tv_sec : 1;
    path = config_path && config_path[0] ? config_path : le_timezone_config_path();
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (st.st_size == seen_size && st.st_mtim.tv_sec == seen_mtime.tv_sec &&
        st.st_mtim.tv_nsec == seen_mtime.tv_nsec)
        return 0;
    file = fopen(path, "r");
    if (!file)
        return 0;
    n = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[n] = 0;
    seen_size = st.st_size;
    seen_mtime = st.st_mtim;
    if (le_timezone_from_config(json, zone, sizeof(zone)) != 0 ||
        (installed && !strcmp(env + 3, zone)))
        return 0;
    if (!installed) {
        snprintf(env, sizeof(env), "TZ=%s", zone);
        if (putenv(env) != 0)
            return 0;
        installed = 1;
        tzset();
        return 1;
    }
    /* Clear the tail first so the string stays terminated throughout. */
    {
        size_t len = strlen(zone), old = strlen(env + 3);

        if (old > len)
            memset(env + 3 + len, 0, old - len);
        env[3 + len] = 0;
        memcpy(env + 3, zone, len);
    }
    tzset();
    return 1;
}

#endif
