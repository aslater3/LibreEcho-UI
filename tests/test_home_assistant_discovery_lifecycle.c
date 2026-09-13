/* Behavioral regression for the Home Assistant discovery lifecycle.
 *
 * Two review findings shaped this test:
 *   1. the AirPlay controller restart sat inside the `||` pipeline chain, so
 *      an install without the optional AirPlay squashfs payload left
 *      sttd/ttsd/agentd stopped; and
 *   2. discovery is owned by the shared libreecho-mdnsd supervisor, so the
 *      refresh must call its init/status path instead of restarting AirPlay.
 *
 * This runs the real apply_home_assistant_mode() with argument-stubbed init
 * scripts and asserts that the requested pipeline state is restored even when
 * the discovery refresh fails, that the refresh never restarts the AirPlay
 * controller, and that a failed refresh never fails the transition.
 */
#define _POSIX_C_SOURCE 200809L
#define LE_INIT_AGENTD    "/tmp/libreecho-ha-agentd.init"
#define LE_INIT_STTD      "/tmp/libreecho-ha-sttd.init"
#define LE_INIT_TTSD      "/tmp/libreecho-ha-ttsd.init"
#define LE_INIT_WYOMINGD  "/tmp/libreecho-ha-wyomingd.init"
#define LE_INIT_AIRPLAYD  "/tmp/libreecho-ha-airplayd.init"
#define LE_INIT_MDNSD     "/tmp/libreecho-ha-mdnsd.init"
#define static
#include "../src/api.c"
#undef static

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH "/tmp/libreecho-ha-lifecycle.log"

static const char *const all_paths[] = {
    LE_INIT_AGENTD, LE_INIT_STTD, LE_INIT_TTSD, LE_INIT_WYOMINGD,
    LE_INIT_AIRPLAYD, LE_INIT_MDNSD
};

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

/* Each stub records "<script> <action>" and exits with the given status. The
 * discovery probe distinguishes `status` from the operations that follow it,
 * so a degraded supervisor can be modelled without failing anything else. */
static void write_script(const char *path, int status, int other)
{
    FILE *file = fopen(path, "w");

    if (!file) {
        fprintf(stderr, "cannot create %s\n", path);
        exit(1);
    }
    fprintf(file, "#!/bin/sh\n");
    fprintf(file, "printf '%%s %%s\\n' '%s' \"$1\" >>\"$LE_HA_LOG\"\n",
            basename_of(path));
    fprintf(file, "case \"$1\" in\nstatus) exit %d ;;\n*) exit %d ;;\nesac\n",
            status, other);
    fclose(file);
    if (chmod(path, 0700) != 0) {
        fprintf(stderr, "cannot chmod %s\n", path);
        exit(1);
    }
}

/* status_rc / other_rc apply to the mDNS init script only. */
static void rewrite_all(int airplay_rc, int mdns_status_rc, int mdns_other_rc)
{
    size_t i;

    for (i = 0; i < sizeof(all_paths) / sizeof(all_paths[0]); ++i) {
        if (!strcmp(all_paths[i], LE_INIT_AIRPLAYD))
            write_script(all_paths[i], airplay_rc, airplay_rc);
        else if (!strcmp(all_paths[i], LE_INIT_MDNSD))
            write_script(all_paths[i], mdns_status_rc, mdns_other_rc);
        else
            write_script(all_paths[i], 0, 0);
    }
}

static void reset_log(void)
{
    if (unlink(LOG_PATH) == 0)
        return;
    /* A missing log is the expected first-run state. */
}

static int log_count(void)
{
    FILE *file = fopen(LOG_PATH, "r");
    char line[256];
    int count = 0;

    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file))
        ++count;
    fclose(file);
    return count;
}

static int log_position(const char *entry)
{
    FILE *file = fopen(LOG_PATH, "r");
    char line[256];
    int index = 0;

    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strcmp(line, entry)) {
            fclose(file);
            return index;
        }
        ++index;
    }
    fclose(file);
    return -1;
}

static void expect_invoked(const char *script, const char *action)
{
    char entry[128];

    snprintf(entry, sizeof(entry), "%s %s", script, action);
    if (log_position(entry) < 0) {
        fprintf(stderr, "expected invocation missing: %s\n", entry);
        exit(1);
    }
}

static void expect_not_invoked(const char *script)
{
    FILE *file = fopen(LOG_PATH, "r");
    char line[256];
    size_t length = strlen(script);

    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, script, length)) {
            fprintf(stderr, "the AirPlay controller must not drive discovery: %s",
                    line);
            fclose(file);
            exit(1);
        }
    }
    fclose(file);
}

int main(void)
{
    int rc;

    reset_log();
    if (setenv("LE_HA_LOG", LOG_PATH, 1) != 0)
        return 1;

    /* Disable while the shared supervisor is unavailable: the refresh fails,
     * but every local daemon must still be restored and AirPlay untouched. */
    rewrite_all(1, 1, 1);
    reset_log();
    rc = apply_home_assistant_mode(0);
    if (rc != LE_OK) {
        fprintf(stderr, "disable returned %d, expected LE_OK\n", rc);
        return 1;
    }
    expect_invoked("libreecho-ha-wyomingd.init", "stop");
    expect_invoked("libreecho-ha-sttd.init", "start");
    expect_invoked("libreecho-ha-ttsd.init", "start");
    expect_invoked("libreecho-ha-agentd.init", "start");
    expect_invoked("libreecho-ha-mdnsd.init", "status");
    expect_invoked("libreecho-ha-mdnsd.init", "start");
    expect_not_invoked("libreecho-ha-airplayd.init");
    if (log_position("libreecho-ha-sttd.init start") >
        log_position("libreecho-ha-mdnsd.init status")) {
        fprintf(stderr, "pipeline restore must not wait on the discovery refresh\n");
        return 1;
    }
    if (log_count() != 6) {
        fprintf(stderr, "disable ran %d commands, expected 6\n", log_count());
        return 1;
    }
    if (home_assistant_discovery_unavailable) {
        fprintf(stderr, "disabling must not report an unavailable discovery\n");
        return 1;
    }

    /* Enable while the discovery refresh fails: the local pipeline is stopped,
     * Wyoming is started, and the transition still succeeds. The failed
     * refresh must be reported, not silently discarded. */
    reset_log();
    rc = apply_home_assistant_mode(1);
    if (rc != LE_OK) {
        fprintf(stderr, "enable returned %d, expected LE_OK\n", rc);
        return 1;
    }
    expect_invoked("libreecho-ha-agentd.init", "stop");
    expect_invoked("libreecho-ha-sttd.init", "stop");
    expect_invoked("libreecho-ha-ttsd.init", "stop");
    expect_invoked("libreecho-ha-wyomingd.init", "start");
    expect_invoked("libreecho-ha-mdnsd.init", "status");
    expect_invoked("libreecho-ha-mdnsd.init", "start");
    expect_not_invoked("libreecho-ha-airplayd.init");
    if (log_count() != 6) {
        fprintf(stderr, "enable ran %d commands, expected 6\n", log_count());
        return 1;
    }
    if (!home_assistant_discovery_unavailable) {
        fprintf(stderr, "a failed discovery refresh must be reported as unavailable\n");
        return 1;
    }

    /* Enable with a healthy supervisor: the probe succeeds and no restart is
     * needed, so nothing is reported as unavailable. */
    rewrite_all(0, 0, 0);
    reset_log();
    rc = apply_home_assistant_mode(1);
    if (rc != LE_OK || home_assistant_discovery_unavailable) {
        fprintf(stderr, "enable with a healthy supervisor must report discovery available\n");
        return 1;
    }
    expect_invoked("libreecho-ha-mdnsd.init", "status");
    expect_not_invoked("libreecho-ha-airplayd.init");
    if (log_count() != 5) {
        fprintf(stderr, "healthy probe ran %d commands, expected 5\n", log_count());
        return 1;
    }

    /* When the supervisor is not installed the refresh is skipped entirely and
     * the pipeline transition is still success. Disabling needs no
     * advertisement, so it is not reported; enabling cannot advertise. */
    unlink(LE_INIT_MDNSD);
    reset_log();
    rc = apply_home_assistant_mode(0);
    if (rc != LE_OK) {
        fprintf(stderr, "disable without supervisor returned %d, expected LE_OK\n", rc);
        return 1;
    }
    expect_invoked("libreecho-ha-agentd.init", "start");
    expect_invoked("libreecho-ha-sttd.init", "start");
    expect_invoked("libreecho-ha-ttsd.init", "start");
    if (log_count() != 4) {
        fprintf(stderr, "skipped refresh ran %d commands, expected 4\n", log_count());
        return 1;
    }
    if (home_assistant_discovery_unavailable) {
        fprintf(stderr, "disabling without a supervisor must not report unavailable discovery\n");
        return 1;
    }
    if (apply_home_assistant_mode(1) != LE_OK || !home_assistant_discovery_unavailable) {
        fprintf(stderr, "enabling without a supervisor must report unavailable discovery\n");
        return 1;
    }

    /* A genuine pipeline failure must still be reported. */
    rewrite_all(1, 1, 1);
    write_script(LE_INIT_STTD, 1, 1);
    reset_log();
    if (apply_home_assistant_mode(0) != LE_IO) {
        fprintf(stderr, "a failing pipeline command must report LE_IO\n");
        return 1;
    }

    /* The documented voice-pipeline route must synchronize discovery through
     * the shared supervisor, not through the AirPlay controller. */
    rewrite_all(1, 0, 0);
    reset_log();
    if (voice_pipeline_restart("home-assistant") != LE_OK) {
        fprintf(stderr, "home-assistant voice pipeline restart must succeed\n");
        return 1;
    }
    expect_invoked("libreecho-ha-wyomingd.init", "start");
    expect_invoked("libreecho-ha-mdnsd.init", "status");
    expect_not_invoked("libreecho-ha-airplayd.init");

    rewrite_all(1, 0, 0);
    reset_log();
    if (voice_pipeline_restart("local") != LE_OK) {
        fprintf(stderr, "local voice pipeline restart must succeed\n");
        return 1;
    }
    expect_invoked("libreecho-ha-mdnsd.init", "status");
    expect_not_invoked("libreecho-ha-airplayd.init");

    /* The pipeline mode and the Home Assistant integration bit are two signals
     * for the same Wyoming daemon: this route starts or stops it, and the mDNS
     * supervisor advertises the mDNS service from the bit. Keep them
     * synchronized, or a switch that stops the daemon would leave a stale
     * Wyoming advertisement pointing at a closed port. */
    {
        struct api_context context;

        memset(&context, 0, sizeof(context));
        if (voice_pipeline_update(&context, "{\"mode\":\"home-assistant\"}") != LE_OK ||
            (context.integrations & 1u) == 0) {
            fprintf(stderr, "home-assistant mode must set the HA integration bit\n");
            return 1;
        }
        if (voice_pipeline_update(&context, "{\"mode\":\"local\"}") != LE_OK ||
            (context.integrations & 1u) != 0) {
            fprintf(stderr, "a local pipeline must clear the HA integration bit\n");
            return 1;
        }
        context.integrations |= 1u;
        if (voice_pipeline_update(&context,
                "{\"mode\":\"custom\",\"stt_wyoming_uri\":\"tcp://127.0.0.1:10300\","
                "\"stt_model\":\"whisper-small\","
                "\"tts_wyoming_uri\":\"tcp://127.0.0.1:10200\","
                "\"tts_voice\":\"en_GB-alan-medium\"}") != LE_OK ||
            (context.integrations & 1u) != 0) {
            fprintf(stderr, "a custom pipeline must clear the HA integration bit\n");
            return 1;
        }
    }

    reset_log();
    for (size_t i = 0; i < sizeof(all_paths) / sizeof(all_paths[0]); ++i)
        unlink(all_paths[i]);
    unlink(LOG_PATH);
    puts("home assistant discovery lifecycle: independent and recoverable PASS");
    return 0;
}
