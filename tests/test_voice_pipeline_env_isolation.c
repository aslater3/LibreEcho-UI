/*
 * Voice-pipeline restart under a conflicting caller environment.
 *
 * The Web backend is a process carrying its own generic
 * ARGS/DAEMON/PIDFILE/LOGFILE. Changing the voice pipeline from the Web UI
 * makes it stop and start the other services through their init scripts, and
 * those scripts resolve their settings with `VAR=${VAR:-default}`. Before the
 * environment boundary existed the inherited values won: libreecho-sttd,
 * libreecho-ttsd and libreecho-agentd were launched with the Web daemon's
 * argv (usage on stderr, exit, no /run/libreecho/agent.sock, so the API
 * answered 503 "Voice assistant service is unavailable"), and
 * libreecho-wyomingd failed the same way in Home Assistant mode (issue #249).
 *
 * This runs the real api.c transition for both modes against fixture init
 * scripts with the Web daemon's environment set, and asserts every service
 * resolves its own identity and argv. The fixtures mirror the shipped
 * scripts' resolution rule; test_service_env_isolation_contract.sh pins the
 * defaults used here to the shipped scripts so the fixture cannot drift into
 * a test of itself.
 */
#define _POSIX_C_SOURCE 200809L
#define LE_INIT_AGENTD   "/tmp/libreecho-249-agentd.init"
#define LE_INIT_STTD     "/tmp/libreecho-249-sttd.init"
#define LE_INIT_TTSD     "/tmp/libreecho-249-ttsd.init"
#define LE_INIT_WYOMINGD "/tmp/libreecho-249-wyomingd.init"
#define static
#include "../src/api.c"
#undef static

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RECORD "/tmp/libreecho-249-pipeline.txt"
#define WEB_ARGS "--backend linux --config /data/libreecho/config/web-config.json --web-root /usr/local/share/libreecho/web --listen 0.0.0.0:8080 --allow-insecure-lan"

/* The shipped defaults each fixture resolves, fully expanded. */
#define AGENTD_DAEMON "/run/libreecho/features/assistant/root/usr/local/sbin/libreecho-agentd"
#define AGENTD_ARGS "--socket /run/libreecho/agent.sock --curl /run/libreecho/features/assistant/root/usr/local/libexec/libreecho-curl"
#define STTD_DAEMON "/run/libreecho/features/stt/root/usr/local/sbin/libreecho-sttd"
#define STTD_ARGS "--socket /run/libreecho/stt.sock --model-dir /run/libreecho/features/stt/root/usr/local/share/libreecho/stt --threads 2"
#define TTSD_DAEMON "/run/libreecho/features/tts/root/usr/local/sbin/libreecho-ttsd"
#define TTSD_ARGS "--foreground --socket /run/libreecho/tts.sock --model-dir /run/libreecho/features/tts/root/usr/local/share/libreecho/tts/models --voice northern-male"
#define WYOMINGD_DAEMON "/usr/local/sbin/libreecho-wyomingd"
#define WYOMINGD_ARGS "--foreground --port 10700 --wake-socket /run/libreecho/wakeword.sock --audio-bus /run/libreecho-audio/system.pcm --mdns-socket /run/libreecho/mdns.sock"

static const char *const fixture_paths[] = {
    LE_INIT_AGENTD, LE_INIT_STTD, LE_INIT_TTSD, LE_INIT_WYOMINGD
};
static char record[8192];
static int failures;

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

/*
 * The fixture resolves exactly what the shipped init script resolves: its own
 * defaults, with an inherited override winning -- which is the rule that made
 * a caller's argv reach another service.
 */
static void write_fixture(const char *path, const char *service,
                          const char *daemon, const char *pidfile,
                          const char *logfile, const char *args)
{
    FILE *file = fopen(path, "w");
    int written;

    if (!file) {
        printf("  FAIL  fixture %s could not be written\n", path);
        exit(1);
    }
    written = fprintf(file,
        "#!/bin/sh\n"
        "DAEMON=${DAEMON:-%s}\n"
        "PIDFILE=${PIDFILE:-%s}\n"
        "LOGFILE=${LOGFILE:-%s}\n"
        "ARGS=${ARGS:-%s}\n"
        "printf '%s daemon=%%s pidfile=%%s logfile=%%s args=%%s\\n'"
        " \"$DAEMON\" \"$PIDFILE\" \"$LOGFILE\" \"$ARGS\""
        " >> \"${LIBREECHO_249_RECORD:-/dev/null}\"\n"
        "exit 0\n", daemon, pidfile, logfile, args, service);
    if (written < 0 || fclose(file) != 0 || chmod(path, 0700) != 0) {
        printf("  FAIL  fixture %s could not be written\n", path);
        exit(1);
    }
}

static void write_fixtures(void)
{
    size_t i;

    for (i = 0; i < sizeof(fixture_paths) / sizeof(fixture_paths[0]); ++i)
        unlink(fixture_paths[i]);
    write_fixture(LE_INIT_AGENTD, "agentd", AGENTD_DAEMON,
                  "/var/run/libreecho-agentd.pid", "/var/log/libreecho-agentd.log",
                  AGENTD_ARGS);
    write_fixture(LE_INIT_STTD, "sttd", STTD_DAEMON,
                  "/var/run/libreecho-sttd.pid", "/var/log/libreecho-sttd.log",
                  STTD_ARGS);
    write_fixture(LE_INIT_TTSD, "ttsd", TTSD_DAEMON,
                  "/var/run/libreecho-ttsd.pid", "/var/log/libreecho-ttsd.log",
                  TTSD_ARGS);
    write_fixture(LE_INIT_WYOMINGD, "wyomingd", WYOMINGD_DAEMON,
                  "/var/run/libreecho-wyomingd.pid", "/var/log/libreecho-wyomingd.log",
                  WYOMINGD_ARGS);
}

static void load_record(void)
{
    FILE *file = fopen(RECORD, "r");
    size_t count;

    record[0] = 0;
    if (!file)
        return;
    count = fread(record, 1, sizeof(record) - 1, file);
    record[count] = 0;
    fclose(file);
}

static void expect_line(const char *line)
{
    char needle[1024];
    int found;

    snprintf(needle, sizeof(needle), "%s\n", line);
    found = strstr(record, needle) != NULL;
    if (!found)
        printf("        missing: %s\n        record:\n%s", line, record);
    check(found, line);
}

static void expect_service(const char *service, const char *daemon,
                           const char *pidfile, const char *logfile,
                           const char *args)
{
    char line[1024];

    snprintf(line, sizeof(line), "%s daemon=%s pidfile=%s logfile=%s args=%s",
             service, daemon, pidfile, logfile, args);
    expect_line(line);
}

/* No service may be handed any part of the Web daemon's identity. */
static void expect_no_caller_leak(void)
{
    check(strstr(record, "libreecho-web") == NULL,
          "no service resolved a libreecho-web path");
    check(strstr(record, "--backend linux") == NULL,
          "no service resolved the Web daemon argv");
}

static void wait_for_restart(void)
{
    struct timespec delay = {0, 50000000L};
    unsigned int attempts;

    for (attempts = 0; attempts < 240; ++attempts) {
        if (!voice_pipeline_restart_pending())
            return;
        nanosleep(&delay, NULL);
    }
    check(0, "voice pipeline restart finished");
}

int main(void)
{
    write_fixtures();
    unlink(RECORD);
    setenv("LIBREECHO_249_RECORD", RECORD, 1);
    /* Exactly what the Web daemon carries while it controls other services. */
    setenv("ARGS", WEB_ARGS, 1);
    setenv("DAEMON", "/usr/local/sbin/libreecho-web", 1);
    setenv("PIDFILE", "/var/run/libreecho-web.pid", 1);
    setenv("LOGFILE", "/var/log/libreecho-web.log", 1);

    printf("local mode: every service keeps its own argv\n");
    check(apply_voice_pipeline_mode("local") == LE_BUSY, "local restart accepted");
    wait_for_restart();
    check(!strcmp(voice_pipeline_restart_state(), "ready"), "local restart completed");
    load_record();
    expect_service("sttd", STTD_DAEMON, "/var/run/libreecho-sttd.pid",
                   "/var/log/libreecho-sttd.log", STTD_ARGS);
    expect_service("ttsd", TTSD_DAEMON, "/var/run/libreecho-ttsd.pid",
                   "/var/log/libreecho-ttsd.log", TTSD_ARGS);
    expect_service("agentd", AGENTD_DAEMON, "/var/run/libreecho-agentd.pid",
                   "/var/log/libreecho-agentd.log", AGENTD_ARGS);
    expect_no_caller_leak();

    printf("home-assistant mode: wyomingd keeps its own argv\n");
    unlink(RECORD);
    check(apply_voice_pipeline_mode("home-assistant") == LE_BUSY,
          "home-assistant restart accepted");
    wait_for_restart();
    check(!strcmp(voice_pipeline_restart_state(), "ready"),
          "home-assistant restart completed");
    load_record();
    expect_service("wyomingd", WYOMINGD_DAEMON, "/var/run/libreecho-wyomingd.pid",
                   "/var/log/libreecho-wyomingd.log", WYOMINGD_ARGS);
    expect_no_caller_leak();

    if (failures) {
        printf("voice pipeline environment isolation: %d failure(s)\n", failures);
        return 1;
    }
    printf("voice pipeline environment isolation: ok\n");
    return 0;
}
