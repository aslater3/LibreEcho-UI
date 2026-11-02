/* Time zone helper (#371): validation, <...> rewriting, config extraction and live follow. */
#define LE_TIMEZONE_RECHECK_SECONDS 0
#include "timezone.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");

    assert(f);
    assert(fputs(body, f) >= 0);
    assert(fclose(f) == 0);
}

static int local_hour(time_t when)
{
    struct tm tm;

    assert(localtime_r(&when, &tm));
    return tm.tm_hour;
}

int main(void)
{
    char out[64], path[] = "/tmp/le-timezone-XXXXXX";
    const time_t january = 1767268800; /* 2026-01-01 12:00:00 UTC */
    const time_t july = 1782907200;    /* 2026-07-01 12:00:00 UTC */
    int fd;

    /* Validation keeps the initramfs character set. */
    assert(le_timezone_valid("UTC"));
    assert(le_timezone_valid("CST6CDT,M3.2.0,M11.1.0"));
    assert(le_timezone_valid("GMT0BST,M3.5.0/1,M10.5.0"));
    assert(!le_timezone_valid(""));
    assert(!le_timezone_valid(NULL));
    assert(!le_timezone_valid("UTC;reboot"));
    assert(!le_timezone_valid("<+04>-4"));
    assert(!le_timezone_valid("Europe/London\""));
    memset(out, 'A', 64);
    out[63] = 0;
    assert(le_timezone_valid(out));
    out[0] = 0;
    {
        char longer[66];

        memset(longer, 'A', 65);
        longer[65] = 0;
        assert(!le_timezone_valid(longer));
    }

    /* Angle-bracket names (what Home Assistant sends for e.g. Asia/Dubai) become letters. */
    assert(le_timezone_normalize("<+04>-4", out, sizeof(out)) == 0);
    assert(!strcmp(out, "LST-4"));
    assert(le_timezone_normalize("<-03>3<-02>,M3.2.0,M11.1.0", out, sizeof(out)) == 0);
    assert(!strcmp(out, "LST3LDT,M3.2.0,M11.1.0"));
    assert(le_timezone_normalize("<+0545>-5:45", out, sizeof(out)) == 0);
    assert(!strcmp(out, "LST-5:45"));
    assert(le_timezone_normalize("CET-1CEST,M3.5.0,M10.5.0/3", out, sizeof(out)) == 0);
    assert(!strcmp(out, "CET-1CEST,M3.5.0,M10.5.0/3"));
    assert(le_timezone_normalize("<+04", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("<>1", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("<a;b>1", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("<A>1<B>,M3.2.0,M11.1.0<C>", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("UTC 0", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("", out, sizeof(out)) == -1);
    assert(le_timezone_normalize("UTC0", out, 3) == -1);

    /* Only the top-level "timezone" string counts; similar keys and nesting do not. */
    assert(le_timezone_from_config("{\"timezone_name\":\"Europe/London\",\"timezone\": \"GMT0BST,M3.5.0/1,M10.5.0\"}", out, sizeof(out)) == 0);
    assert(!strcmp(out, "GMT0BST,M3.5.0/1,M10.5.0"));
    assert(le_timezone_from_config("{\"x\":{\"timezone\":\"EST5EDT\"}}", out, sizeof(out)) == -1);
    assert(le_timezone_from_config("{\"note\":\"\\\"timezone\\\"\",\"timezone\":\"EST5EDT\"}", out, sizeof(out)) == 0);
    assert(!strcmp(out, "EST5EDT"));
    assert(le_timezone_from_config("{\"timezone\":\"EST5\\\"EDT\"}", out, sizeof(out)) == -1);
    assert(le_timezone_from_config("{\"timezone\":\"UTC;x\"}", out, sizeof(out)) == -1);
    assert(le_timezone_from_config("{\"timezone\":42}", out, sizeof(out)) == -1);
    assert(le_timezone_from_config("{}", out, sizeof(out)) == -1);

    /* Live follow: TZ changes in place when the configuration does. */
    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    write_file(path, "{\n  \"timezone\": \"UTC\"\n}\n");
    assert(setenv("LE_TIMEZONE_CONFIG", path, 1) == 0);
    assert(setenv("TZ", "UTC", 1) == 0);
    tzset();
    (void)le_timezone_follow(NULL);
    assert(local_hour(january) == 12);
    {
        const char *before = getenv("TZ");

        write_file(path, "{\n  \"timezone\": \"CST6CDT,M3.2.0,M11.1.0\",\n  \"room\": \"x\"\n}\n");
        assert(le_timezone_follow(NULL) == 1);
        assert(!strcmp(getenv("TZ"), "CST6CDT,M3.2.0,M11.1.0"));
        /* The same buffer is rewritten, never a fresh allocation. */
        assert(getenv("TZ") == before);
        assert(local_hour(january) == 6);
        assert(local_hour(july) == 7);
        /* Unchanged file: nothing to do. */
        assert(le_timezone_follow(NULL) == 0);
        /* A broken or hostile file leaves the zone alone. */
        write_file(path, "{\n  \"timezone\": \"UTC;rm\"\n}\n");
        assert(le_timezone_follow(NULL) == 0);
        assert(!strcmp(getenv("TZ"), "CST6CDT,M3.2.0,M11.1.0"));
        /* A shorter zone is written over a longer one cleanly. */
        write_file(path, "{\"timezone\":\"JST-9\"}");
        assert(le_timezone_follow(NULL) == 1);
        assert(!strcmp(getenv("TZ"), "JST-9"));
        assert(local_hour(january) == 21);
        assert(getenv("TZ") == before);
    }
    unlink(path);
    /* A missing file leaves the zone alone. */
    write_file(path, "{\"timezone\":\"UTC\"}");
    unlink(path);
    assert(le_timezone_follow(NULL) == 0);
    assert(!strcmp(getenv("TZ"), "JST-9"));
    puts("timezone: ok");
    return 0;
}
