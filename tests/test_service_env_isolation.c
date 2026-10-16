/*
 * The service-control environment boundary: what a caller's own service
 * identity must not do.
 *
 * The Web daemon, the watchdog recovery path and the factory-reset path each
 * exec another service's init script while carrying their own generic
 * ARGS/DAEMON/PIDFILE/LOGFILE. Those scripts resolve their settings with
 * `VAR=${VAR:-default}`, so an inherited value wins and one daemon's argv is
 * handed to another service's binary -- libreecho-sttd, libreecho-ttsd and
 * libreecho-agentd printed usage and exited, /run/libreecho/agent.sock never
 * appeared, and the Web API answered 503 "Voice assistant service
 * unavailable" (issue #249).
 *
 * This drives the shipped boundary helper with that environment set and reads
 * what the child actually received. The assertions are on the environment and
 * argv of a real exec'd process, in all three shapes the callers use: script
 * plus action (API and factory reset), script plus action plus value (voice
 * switch) and /bin/sh plus script plus action (watchdog recovery).
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/service_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_ARGS "--backend linux --config /data/libreecho/config/web-config.json --web-root /usr/local/share/libreecho/web --listen 0.0.0.0:8080 --allow-insecure-lan"
#define WEB_DAEMON "/usr/local/sbin/libreecho-web"
#define WEB_PIDFILE "/var/run/libreecho-web.pid"
#define WEB_LOGFILE "/var/log/libreecho-web.log"
#define SCRIPT "/tmp/libreecho-249-boundary.sh"
#define RECORD "/tmp/libreecho-249-boundary.txt"
#define MISSING "/tmp/libreecho-249-does-not-exist"

static char record[8192];
static int failures;

static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

static void write_script(void)
{
    static const char body[] =
        "#!/bin/sh\n"
        "{\n"
        "    printf 'argc=%s\\n' \"$#\"\n"
        "    for arg in \"$@\"; do printf 'arg=%s\\n' \"$arg\"; done\n"
        "    printf 'ARGS=%s\\n' \"${ARGS-<unset>}\"\n"
        "    printf 'DAEMON=%s\\n' \"${DAEMON-<unset>}\"\n"
        "    printf 'PIDFILE=%s\\n' \"${PIDFILE-<unset>}\"\n"
        "    printf 'LOGFILE=%s\\n' \"${LOGFILE-<unset>}\"\n"
        "    printf 'SENTINEL=%s\\n' \"${LIBREECHO_TEST_SENTINEL-<unset>}\"\n"
        "    printf 'SERVICE=%s\\n' \"${LE_STT_WYOMING_URI-<unset>}\"\n"
        "    printf 'PATH=%s\\n' \"$([ -n \"$PATH\" ] && echo present || echo absent)\"\n"
        "} > \"${LIBREECHO_TEST_RECORD}\"\n"
        "exit \"${LIBREECHO_TEST_EXIT:-0}\"\n";
    FILE *file = fopen(SCRIPT, "w");

    if (!file || fputs(body, file) == EOF || fclose(file) != 0 ||
        chmod(SCRIPT, 0700) != 0) {
        printf("  FAIL  fixture script could not be written\n");
        exit(1);
    }
}

static void load_record(void)
{
    FILE *file = fopen(RECORD, "r");
    size_t count;

    record[0] = 0;
    if (!file) {
        check(0, "the child wrote its record");
        return;
    }
    count = fread(record, 1, sizeof(record) - 1, file);
    record[count] = 0;
    fclose(file);
}

static void expect_line(const char *line)
{
    char needle[512];
    int found;

    snprintf(needle, sizeof(needle), "%s\n", line);
    found = strstr(record, needle) != NULL;
    if (!found)
        printf("        missing: %s\n        record:\n%s", line, record);
    check(found, line);
}

/* Every shape must run the child with the caller's identity removed. */
static void expect_no_identity(void)
{
    expect_line("ARGS=<unset>");
    expect_line("DAEMON=<unset>");
    expect_line("PIDFILE=<unset>");
    expect_line("LOGFILE=<unset>");
    /* Everything else the caller passed is not the boundary's business. */
    expect_line("SENTINEL=kept");
    expect_line("SERVICE=kept");
    expect_line("PATH=present");
}

int main(void)
{
    const char *script_action[3] = {SCRIPT, "start", NULL};
    const char *switch_args[4] = {SCRIPT, "switch", "northern-male", NULL};
    const char *shell_action[4] = {"sh", SCRIPT, "recover", NULL};

    write_script();
    setenv("LIBREECHO_TEST_RECORD", RECORD, 1);
    /* Exactly what the Web daemon carries while it controls other services. */
    setenv("ARGS", WEB_ARGS, 1);
    setenv("DAEMON", WEB_DAEMON, 1);
    setenv("PIDFILE", WEB_PIDFILE, 1);
    setenv("LOGFILE", WEB_LOGFILE, 1);
    setenv("LIBREECHO_TEST_SENTINEL", "kept", 1);
    setenv("LE_STT_WYOMING_URI", "kept", 1);
    unsetenv("LIBREECHO_TEST_EXIT");

    printf("boundary: script plus action (API and factory reset)\n");
    unlink(RECORD);
    check(le_service_command(SCRIPT, script_action) == 0, "the script ran");
    load_record();
    expect_line("argc=1");
    expect_line("arg=start");
    expect_no_identity();

    printf("boundary: script plus action plus value (voice switch)\n");
    unlink(RECORD);
    check(le_service_command(SCRIPT, switch_args) == 0, "the script ran");
    load_record();
    expect_line("argc=2");
    expect_line("arg=switch");
    expect_line("arg=northern-male");
    expect_no_identity();

    printf("boundary: /bin/sh plus script plus action (watchdog recovery)\n");
    unlink(RECORD);
    check(le_service_command("/bin/sh", shell_action) == 0, "the script ran");
    load_record();
    expect_line("argc=1");
    expect_line("arg=recover");
    expect_no_identity();

    printf("boundary: outcome and caller state\n");
    check(strcmp(getenv("ARGS"), WEB_ARGS) == 0,
          "the caller keeps its own environment");
    check(strcmp(getenv("DAEMON"), WEB_DAEMON) == 0,
          "the caller keeps its own DAEMON");
    setenv("LIBREECHO_TEST_EXIT", "7", 1);
    check(le_service_command(SCRIPT, script_action) != 0,
          "a failing script is reported as a failure");
    unsetenv("LIBREECHO_TEST_EXIT");
    check(le_service_command(MISSING, script_action) != 0,
          "a script that cannot be executed is reported as a failure");

    if (failures) {
        printf("service environment boundary: %d failure(s)\n", failures);
        return 1;
    }
    printf("service environment boundary: ok\n");
    return 0;
}
