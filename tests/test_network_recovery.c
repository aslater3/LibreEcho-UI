/*
 * Focused host test for the secure recovery core (issue #96).
 *
 * The production lifecycle and marker/secret helpers are exercised directly
 * with an isolated process/command oracle and temp files.  No real network,
 * radio, child daemon or root privilege is required.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "network_recovery.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

static int fs_is_tmpfs(const char *path)
{
    struct statfs status;
    if (statfs(path, &status) < 0)
        return 0;
    return (unsigned long)status.f_type == (unsigned long)TMPFS_MAGIC;
}

static int failures;
static int checks;

#define CHECK(cond) do { \
    ++checks; \
    if (!(cond)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_STR_EQ(a, b) do { \
    ++checks; \
    if (strcmp((a), (b)) != 0) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, (a), (b)); \
    } \
} while (0)

struct fake_backend {
    int spawn_count;
    pid_t next_pid;
    char spawned[8][PATH_MAX];
    int spawn_fail;
    int probe_result;
    int available_result;
    char missing_path[PATH_MAX + 16];
    int alive;                 /* 1 = wait reports alive, 0 = exited */
    int signal_count;
    pid_t signal_pids[32];
    int signal_signums[32];
    int wait_count;
    int probe_count;
    char probe_argv[24][256];
    char probe_fail_path[PATH_MAX];
};

static struct fake_backend fake;

static pid_t fake_spawn(void *ctx, char *const argv[], char *err, size_t err_size)
{
    struct fake_backend *f = ctx;
    (void)err;
    (void)err_size;
    if (f->spawn_fail)
        return -1;
    if (f->spawn_count >= 8)
        return -1;
    snprintf(f->spawned[f->spawn_count], sizeof(f->spawned[0]), "%s", argv[0]);
    ++f->spawn_count;
    return f->next_pid++;
}

static int fake_signal(void *ctx, pid_t pid, int sig)
{
    struct fake_backend *f = ctx;
    if (f->signal_count < 32) {
        f->signal_pids[f->signal_count] = pid;
        f->signal_signums[f->signal_count] = sig;
        ++f->signal_count;
    }
    return 0;
}

static int fake_wait(void *ctx, pid_t pid, int *status)
{
    struct fake_backend *f = ctx;
    (void)pid;
    if (status)
        *status = 0;
    ++f->wait_count;
    return f->alive ? 1 : 0;
}

static int fake_probe(void *ctx, char *const argv[])
{
    struct fake_backend *f = ctx;
    int fail;
    if (f->probe_count < 24) {
        char *slot = f->probe_argv[f->probe_count];
        size_t used = 0;
        int i;
        slot[0] = '\0';
        for (i = 0; argv[i] && used + 1 < sizeof(f->probe_argv[0]); ++i) {
            int n = snprintf(slot + used,
                             sizeof(f->probe_argv[0]) - used, "%s%s",
                             i ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(f->probe_argv[0]) - used)
                break;
            used += (size_t)n;
        }
        ++f->probe_count;
    }
    fail = f->probe_fail_path[0] && argv[0] &&
           !strcmp(argv[0], f->probe_fail_path);
    if (fail)
        return -1;
    return f->probe_result;
}

static int probe_recorded(const char *needle)
{
    int i;
    for (i = 0; i < fake.probe_count; ++i)
        if (strstr(fake.probe_argv[i], needle))
            return 1;
    return 0;
}

static int fake_available(void *ctx, const char *path)
{
    struct fake_backend *f = ctx;
    if (!path || !path[0])
        return 0;
    if (f->missing_path[0] && strcmp(path, f->missing_path) == 0)
        return 0;
    return f->available_result;
}

static unsigned long long fake_identity(void *ctx, pid_t pid)
{
    (void)ctx;
    return (unsigned long long)pid + 1000;
}

static struct le_recovery_backend make_backend(void)
{
    struct le_recovery_backend backend;
    memset(&backend, 0, sizeof(backend));
    backend.spawn = fake_spawn;
    backend.signal_child = fake_signal;
    backend.wait_child = fake_wait;
    backend.probe = fake_probe;
    backend.available = fake_available;
    backend.identity = fake_identity;
    backend.ctx = &fake;
    return backend;
}

static void init_fake(struct le_recovery *recovery,
                      const struct le_recovery_config *config, long long now)
{
    struct le_recovery_backend backend = make_backend();
    le_recovery_init(recovery, config, &backend, now);
}

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.next_pid = 40000;
    fake.probe_result = 0;
    fake.available_result = 1;
    fake.alive = 1;
}

static void write_file(const char *path, const char *content, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        exit(2);
    }
    if (write(fd, content, strlen(content)) < 0)
        exit(2);
    close(fd);
    (void)chmod(path, mode);
}

static void make_config(struct le_recovery_config *config, const char *dir)
{
    char path[512];
    le_recovery_config_default(config, "wlan0");
    snprintf(path, sizeof(path), "%s/recovery-mode", dir);
    snprintf(config->marker_path, sizeof(config->marker_path), "%s", path);
    snprintf(path, sizeof(path), "%s/recovery-psk", dir);
    snprintf(config->psk_path, sizeof(config->psk_path), "%s", path);
    snprintf(path, sizeof(path), "%s/recovery.json", dir);
    snprintf(config->config_path, sizeof(config->config_path), "%s", path);
    snprintf(config->run_dir, sizeof(config->run_dir), "%s", dir);
    snprintf(config->hostapd_bin, sizeof(config->hostapd_bin),
             "%s/hostapd", dir);
    snprintf(config->dhcp_bin, sizeof(config->dhcp_bin), "%s/dnsmasq", dir);
    snprintf(config->dns_bin, sizeof(config->dns_bin), "%s/dnsmasq", dir);
    snprintf(config->serial_path, sizeof(config->serial_path), "%s/serial",
             dir);
    snprintf(config->hostapd_conf, sizeof(config->hostapd_conf), "%s/h.conf",
             dir);
    snprintf(config->dhcp_conf, sizeof(config->dhcp_conf), "%s/d.conf", dir);
    /* Host tests cannot create root-owned tmpfs, so exercise the strict logic
     * by policy override on rejected cases below. */
    config->require_tmpfs = 0;
    config->require_root_owner = 0;
    config->start_timeout_ms = 100;
    config->stop_timeout_ms = 100;
    snprintf(config->ap_probe_cmd, sizeof(config->ap_probe_cmd), "%s/ap-probe",
             dir);
    snprintf(config->net_up_cmd, sizeof(config->net_up_cmd), "%s/net-up", dir);
    snprintf(config->net_down_cmd, sizeof(config->net_down_cmd),
             "%s/net-down", dir);
    snprintf(config->ap_address, sizeof(config->ap_address), "192.168.4.1");
    config->auto_timeout_ms = 120000;
}

static void test_config_defaults(void)
{
    struct le_recovery_config config;
    le_recovery_config_default(&config, "wlan1");
    CHECK(config.enabled == 1);
    CHECK(config.auto_enabled == 0);
    CHECK(config.require_tmpfs == 1);
    CHECK(config.require_root_owner == 1);
    CHECK(config.auto_timeout_ms == LE_RECOVERY_TIMEOUT_DEFAULT_MS);
    CHECK_STR_EQ(config.interface, "wlan1");
    CHECK_STR_EQ(config.ap_address, LE_RECOVERY_AP_ADDRESS_DEFAULT);
    CHECK(strstr(config.net_up_cmd, "libreecho-recovery-net-up") != NULL);
    CHECK(strstr(config.net_down_cmd, "libreecho-recovery-net-down") != NULL);
    config.auto_timeout_ms = 1;   /* below the floor */
    le_recovery_config_bound(&config);
    CHECK(config.auto_timeout_ms == LE_RECOVERY_TIMEOUT_DEFAULT_MS);
    config.auto_timeout_ms = 999999999;
    le_recovery_config_bound(&config);
    CHECK(config.auto_timeout_ms == LE_RECOVERY_TIMEOUT_DEFAULT_MS);
    config.auto_timeout_ms = 60000;
    le_recovery_config_bound(&config);
    CHECK(config.auto_timeout_ms == 60000);
}

static void test_marker(const char *tmpdir)
{
    char dir[512];
    char reason[LE_RECOVERY_REASON_MAX];
    struct le_recovery_config config;
    char link[600];
    char dirpath[600];

    snprintf(dir, sizeof(dir), "%s/marker", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);

    /* No marker is a normal boot. */
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == 0);
    CHECK_STR_EQ(reason, "no-marker");

    /* A well-formed marker is accepted. */
    write_file(config.marker_path, LE_RECOVERY_MARKER_TAG "\nhold_ms=5000\n",
               0600);
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == 1);
    CHECK_STR_EQ(reason, "physical-hold");

    /* World-accessible marker is rejected. */
    (void)chmod(config.marker_path, 0666);
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "marker-world-accessible");
    (void)chmod(config.marker_path, 0600);

    /* Bad content is rejected. */
    write_file(config.marker_path, "something-else\n", 0600);
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "marker-content");

    /* A symlink is never followed. */
    (void)unlink(config.marker_path);
    snprintf(link, sizeof(link), "%s/elsewhere", dir);
    write_file(link, LE_RECOVERY_MARKER_TAG "\n", 0600);
    CHECK(symlink(link, config.marker_path) == 0);
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "marker-symlink");
    (void)unlink(config.marker_path);

    /* A directory at the marker path is not a regular file. */
    snprintf(dirpath, sizeof(dirpath), "%s/adir", dir);
    (void)mkdir(dirpath, 0700);
    snprintf(config.marker_path, sizeof(config.marker_path), "%s", dirpath);
    CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "marker-not-regular");

    /* Strict production policy: a non-tmpfs marker fails closed; a tmpfs one
     * is accepted here but rejected below when it is not root-owned. */
    write_file(link, LE_RECOVERY_MARKER_TAG "\n", 0600);
    snprintf(config.marker_path, sizeof(config.marker_path), "%s", link);
    config.require_tmpfs = 1;
    config.require_root_owner = 0;
    if (fs_is_tmpfs(config.marker_path)) {
        CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == 1);
    } else {
        CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
        CHECK_STR_EQ(reason, "marker-not-tmpfs");
    }
    config.require_tmpfs = 0;
    config.require_root_owner = 1;
    if (geteuid() != 0) {
        CHECK(le_recovery_marker_check(&config, reason, sizeof(reason)) == -1);
        CHECK_STR_EQ(reason, "marker-not-root-owned");
    }
}

static void test_psk(const char *tmpdir)
{
    char dir[512];
    char psk[LE_RECOVERY_PSK_MAX];
    char reason[LE_RECOVERY_REASON_MAX];
    char link[600];
    struct le_recovery_config config;
    struct stat status;
    int fd;

    snprintf(dir, sizeof(dir), "%s/psk", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);

    CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                 sizeof(reason)) == 0);
    CHECK(strlen(psk) == 32);
    CHECK(stat(config.psk_path, &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    CHECK((status.st_mode & 077) == 0);
    CHECK(status.st_size == 32);

    /* Stable across calls: no regeneration, no duplicate identity. */
    {
        char again[LE_RECOVERY_PSK_MAX];
        CHECK(le_recovery_psk_ensure(&config, again, sizeof(again), reason,
                                     sizeof(reason)) == 0);
        CHECK_STR_EQ(psk, again);
    }

    /* A symlink at the secret path is refused, not followed. */
    snprintf(link, sizeof(link), "%s/target", dir);
    write_file(link, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0600);
    (void)unlink(config.psk_path);
    CHECK(symlink(link, config.psk_path) == 0);
    CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                 sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "psk-symlink");
    (void)unlink(config.psk_path);

    /* A too-open secret file is refused. */
    fd = open(config.psk_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 32);
        close(fd);
    }
    CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                 sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "psk-permissions");
}

static void test_capabilities(const char *tmpdir)
{
    char dir[512];
    char reason[LE_RECOVERY_REASON_MAX];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/cap", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);

    reset_fake();
    config.ap_probe_cmd[0] = '\0';
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_probe_capabilities(&recovery, reason, sizeof(reason)) == 0);
    CHECK_STR_EQ(reason, "ap-driver-unverified");

    reset_fake();
    snprintf(config.ap_probe_cmd, sizeof(config.ap_probe_cmd), "%s/ap-probe",
             dir);
    fake.missing_path[0] = '\0';
    snprintf(fake.missing_path, sizeof(fake.missing_path), "%s",
             config.hostapd_bin);
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_probe_capabilities(&recovery, reason, sizeof(reason)) == 0);
    CHECK_STR_EQ(reason, "missing-hostapd");

    reset_fake();
    snprintf(fake.missing_path, sizeof(fake.missing_path), "%s",
             config.dhcp_bin);
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_probe_capabilities(&recovery, reason, sizeof(reason)) == 0);
    CHECK_STR_EQ(reason, "missing-dhcp-dns");

    reset_fake();
    fake.probe_result = -1;
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_probe_capabilities(&recovery, reason, sizeof(reason)) == 0);
    CHECK_STR_EQ(reason, "ap-unsupported");

    reset_fake();
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_probe_capabilities(&recovery, reason, sizeof(reason)) == 1);
    CHECK(recovery.available == 1);
}

static void test_lifecycle(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;
    char status[2048];

    snprintf(dir, sizeof(dir), "%s/life", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    write_file(config.marker_path, LE_RECOVERY_MARKER_TAG "\n", 0600);
    reset_fake();
    init_fake(&recovery, &config, 1000);

    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 1000) == 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ARMED);
    /* A marker alone never reports an active AP. */
    CHECK(recovery.mode != LE_RECOVERY_MODE_ACTIVE);

    le_recovery_tick(&recovery, 1001, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STARTING);
    CHECK(fake.spawn_count == 2);   /* hostapd + dnsmasq */
    CHECK_STR_EQ(fake.spawned[0], config.hostapd_bin);
    CHECK_STR_EQ(fake.spawned[1], config.dns_bin);
    /* The portal address was assigned before the children started. */
    CHECK(recovery.net_configured == 1);
    CHECK(probe_recorded(config.net_up_cmd));
    CHECK(probe_recorded("--address 192.168.4.1/24"));
    CHECK(probe_recorded("--interface wlan0"));

    /* Not yet ready: the start deadline has not elapsed. */
    le_recovery_tick(&recovery, 1050, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STARTING);

    le_recovery_tick(&recovery, 1200, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ACTIVE);
    CHECK(recovery.secret_available == 1);
    CHECK(recovery.led_active == 1);

    CHECK(le_recovery_status_json(&recovery, status, sizeof(status)) > 0);
    CHECK(strstr(status, "\"mode\":\"recovery-ap\"") != NULL);
    CHECK(strstr(status, "\"trigger\":\"physical\"") != NULL);
    CHECK(strstr(status, "\"led_owner\":\"recovery-ap\"") != NULL);
    CHECK(strstr(status, "\"secret_available\":true") != NULL);
    /* The status object never carries the secret value itself. */
    {
        char psk[LE_RECOVERY_PSK_MAX];
        char reason[LE_RECOVERY_REASON_MAX];
        CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                     sizeof(reason)) == 0);
        CHECK(strstr(status, psk) == NULL);
    }

    /* Successful association tears the AP down and clears the marker. */
    le_recovery_tick(&recovery, 1300, 1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STOPPED);
    CHECK(recovery.child_count == 0);
    CHECK(access(config.marker_path, F_OK) != 0);
    /* The portal address was restored on teardown. */
    CHECK(recovery.net_configured == 0);
    CHECK(probe_recorded(config.net_down_cmd));
    /* Teardown signalled exactly the spawned pids, by pid only. */
    CHECK(fake.signal_count >= 2);
    CHECK(fake.signal_pids[0] == 40000);
    CHECK(fake.signal_pids[1] == 40001);
    CHECK(fake.signal_signums[0] == SIGTERM);
}

static void test_child_failure_fails_closed(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/fail", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();
    init_fake(&recovery, &config, 0);
    config.require_tmpfs = 0;
    recovery.config.require_tmpfs = 0;
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == 0);

    /* A capability failure must not report an active AP. */
    recovery.config.ap_probe_cmd[0] = '\0';
    le_recovery_tick(&recovery, 1, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE);
    CHECK(recovery.mode != LE_RECOVERY_MODE_ACTIVE);

    /* A child that exits during startup fails closed too. */
    reset_fake();
    recovery.config.require_tmpfs = 0;
    snprintf(recovery.config.ap_probe_cmd, sizeof(recovery.config.ap_probe_cmd),
             "%s/ap-probe", dir);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 10) == 0);
    le_recovery_tick(&recovery, 11, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STARTING);
    fake.alive = 0;   /* child exits */
    le_recovery_tick(&recovery, 12, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE);
}

static void test_bounded_start_timeout(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;
    long long now;

    snprintf(dir, sizeof(dir), "%s/timeout", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();
    fake.probe_result = -1;   /* readiness probe keeps failing */
    init_fake(&recovery, &config, 0);
    recovery.config.require_tmpfs = 0;
    recovery.config.ready_probe_cmd[0] = 'r';
    recovery.config.ready_probe_cmd[1] = '\0';
    recovery.config.start_timeout_ms = 500;
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == 0);
    le_recovery_tick(&recovery, 1, 0);
    now = 100;
    while (now < 2000) {
        le_recovery_tick(&recovery, now, 0);
        if (recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE)
            break;
        now += 100;
    }
    CHECK(recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE);
    CHECK(now <= 700);   /* bounded by start_timeout_ms, not unlimited */
}

static void test_rate_limit(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;
    int i;

    snprintf(dir, sizeof(dir), "%s/rate", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    config.rate_max = 3;
    config.rate_window_ms = 1000;
    reset_fake();
    init_fake(&recovery, &config, 0);
    for (i = 0; i < 3; ++i)
        CHECK(le_recovery_rate_limit(&recovery, 10) == 0);
    CHECK(le_recovery_rate_limit(&recovery, 10) == -1);
    /* A new window resets the budget. */
    CHECK(le_recovery_rate_limit(&recovery, 2000) == 0);
}

static void test_auto_cutoff(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/auto", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();

    /* Default: automatic fallback is off and the device stays in client mode. */
    init_fake(&recovery, &config, 0);
    le_recovery_tick(&recovery, 1000000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    CHECK(recovery.trigger == LE_RECOVERY_TRIGGER_NONE);

    /* Opted in: the AP is only armed after the bounded window elapses. */
    reset_fake();
    config.auto_enabled = 1;
    config.auto_timeout_ms = 120000;
    init_fake(&recovery, &config, 0);
    le_recovery_tick(&recovery, 1000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    CHECK(recovery.auto_counting == 1);
    le_recovery_tick(&recovery, 1000 + 60000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    le_recovery_tick(&recovery, 1000 + 120000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ARMED);
    CHECK(recovery.trigger == LE_RECOVERY_TRIGGER_AUTO);

    /* A later association cancels the pending fallback. */
    reset_fake();
    init_fake(&recovery, &config, 0);
    le_recovery_tick(&recovery, 1000, 0);
    le_recovery_tick(&recovery, 2000, 1);
    CHECK(recovery.auto_counting == 0);
    le_recovery_tick(&recovery, 300000, 1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
}

static void test_secret_gating(const char *tmpdir)
{
    char dir[512];
    char reason[LE_RECOVERY_REASON_MAX];
    struct le_recovery_config config;
    struct le_recovery recovery;
    char out[512];

    snprintf(dir, sizeof(dir), "%s/gate", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();
    init_fake(&recovery, &config, 0);

    /* Owner can prepare/reveal while client-connected, before it is needed:
     * this is the only point at which the password can be saved. */
    CHECK(le_recovery_secret_prepare(&recovery, reason, sizeof(reason)) == 0);
    CHECK(le_recovery_secret_json(&recovery, out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"psk\":\"") != NULL);
    {
        char psk[LE_RECOVERY_PSK_MAX], r[LE_RECOVERY_REASON_MAX];
        CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), r,
                                     sizeof(r)) == 0);
        CHECK(strstr(out, psk) != NULL);
        CHECK(strlen(psk) == 32);
    }

    /* While the protected captive AP is serving, an unauthenticated client
     * must never receive the secret. */
    config.require_tmpfs = 0;
    recovery.config.require_tmpfs = 0;
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == 0);
    le_recovery_tick(&recovery, 1, 0);
    le_recovery_tick(&recovery, 1000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ACTIVE);
    CHECK(le_recovery_secret_json(&recovery, out, sizeof(out)) == -1);
    CHECK_STR_EQ(out, "{}");
    CHECK(le_recovery_secret_prepare(&recovery, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "captive-ap-active");
}

static void test_disabled_feature(const char *tmpdir)
{
    char dir[512];
    char reason[LE_RECOVERY_REASON_MAX];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/disabled", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    config.enabled = 0;
    reset_fake();
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == -1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    CHECK(le_recovery_secret_prepare(&recovery, reason, sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "recovery-disabled");
    /* Disabled also stops the opt-in automatic fallback. */
    config.auto_enabled = 1;
    init_fake(&recovery, &config, 0);
    le_recovery_tick(&recovery, 500000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
}

static void test_net_helper_fails_closed(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/netfail", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);

    /* No helper configured: the portal address cannot be applied, so the AP
     * must not report itself active. */
    reset_fake();
    config.net_up_cmd[0] = '\0';
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == 0);
    le_recovery_tick(&recovery, 1, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE);
    CHECK_STR_EQ(recovery.unavailable_reason, "ap-net-unconfigured");
    CHECK(fake.spawn_count == 0);

    /* A helper that fails must fail closed with no child left running. */
    reset_fake();
    snprintf(config.net_up_cmd, sizeof(config.net_up_cmd), "%s/net-up", dir);
    snprintf(fake.probe_fail_path, sizeof(fake.probe_fail_path), "%s",
             config.net_up_cmd);
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == 0);
    le_recovery_tick(&recovery, 1, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_UNAVAILABLE);
    CHECK_STR_EQ(recovery.unavailable_reason, "ap-net-up-failed");
    CHECK(fake.spawn_count == 0);
}

static void test_single_interface_handover(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/handover", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    write_file(config.marker_path, LE_RECOVERY_MARKER_TAG "\n", 0600);

    /* Failed credential attempt: the radio is freed, the AP is rebuilt and the
     * marker is kept so the owner can retry. */
    reset_fake();
    init_fake(&recovery, &config, 1000);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 1000) == 0);
    le_recovery_tick(&recovery, 1001, 0);
    le_recovery_tick(&recovery, 1200, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ACTIVE);
    CHECK(fake.spawn_count == 2);

    CHECK(le_recovery_handover_begin(&recovery, 1300) == 1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_HANDOVER);
    CHECK(recovery.child_count == 0);
    CHECK(recovery.net_configured == 0);
    CHECK(probe_recorded(config.net_down_cmd));
    CHECK(access(config.marker_path, F_OK) == 0);   /* marker kept */

    le_recovery_handover_result(&recovery, 1400, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ARMED);
    CHECK(access(config.marker_path, F_OK) == 0);
    le_recovery_tick(&recovery, 1500, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STARTING);
    CHECK(fake.spawn_count == 4);   /* AP rebuilt */

    /* Successful attempt: recovery ends and the marker is cleared. */
    reset_fake();
    init_fake(&recovery, &config, 2000);
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 2000) == 0);
    le_recovery_tick(&recovery, 2001, 0);
    le_recovery_tick(&recovery, 2200, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_ACTIVE);
    CHECK(le_recovery_handover_begin(&recovery, 2300) == 1);
    le_recovery_handover_result(&recovery, 2400, 1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_STOPPED);
    CHECK(recovery.net_configured == 0);
    CHECK(access(config.marker_path, F_OK) != 0);
}

static void test_persistent_paths_and_psk_survives_reinit(const char *tmpdir)
{
    struct le_recovery_config defaults;
    char dir[512];
    char psk_first[LE_RECOVERY_PSK_MAX], psk_again[LE_RECOVERY_PSK_MAX];
    char reason[LE_RECOVERY_REASON_MAX];
    struct le_recovery_config config;

    /* Boot marker stays on the per-boot tmpfs; the password and the owner
     * configuration live under protected /data so they survive a reboot. */
    le_recovery_config_default(&defaults, "wlan0");
    CHECK(strncmp(defaults.marker_path, "/run/", 5) == 0);
    CHECK(strstr(defaults.psk_path, "/data/libreecho/config/recovery-psk") != NULL);
    CHECK(strstr(defaults.config_path,
                 "/data/libreecho/config/recovery.json") != NULL);
    CHECK(defaults.require_root_peer == 1);

    snprintf(dir, sizeof(dir), "%s/persist", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    CHECK(le_recovery_psk_ensure(&config, psk_first, sizeof(psk_first),
                                 reason, sizeof(reason)) == 0);

    /* Simulate a reboot: a fresh configuration pointing at the same persisted
     * paths must recover the identical password, not generate a new one. */
    {
        struct le_recovery_config rebooted;
        make_config(&rebooted, dir);
        CHECK(le_recovery_psk_ensure(&rebooted, psk_again, sizeof(psk_again),
                                     reason, sizeof(reason)) == 0);
        CHECK_STR_EQ(psk_first, psk_again);
    }
}

static void test_configure_persists_validates_and_rolls_back(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config, reloaded;
    struct le_recovery recovery;
    char reason[LE_RECOVERY_REASON_MAX];
    struct stat status;

    snprintf(dir, sizeof(dir), "%s/configure", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();
    init_fake(&recovery, &config, 1000);

    CHECK(recovery.config.enabled == 1);
    CHECK(recovery.config.auto_enabled == 0);

    /* A valid owner configuration is persisted atomically, mode 0600, and
     * committed to the running configuration. */
    CHECK(le_recovery_configure(&recovery, 1, 1, 60000, reason,
                                sizeof(reason)) == 0);
    CHECK(recovery.config.enabled == 1);
    CHECK(recovery.config.auto_enabled == 1);
    CHECK(recovery.config.auto_timeout_ms == 60000);
    CHECK(stat(config.config_path, &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    CHECK((status.st_mode & 077) == 0);

    /* It survives a reload (reboot). */
    make_config(&reloaded, dir);
    CHECK(le_recovery_config_load(&reloaded, reason, sizeof(reason)) == 1);
    CHECK(reloaded.enabled == 1);
    CHECK(reloaded.auto_enabled == 1);
    CHECK(reloaded.auto_timeout_ms == 60000);

    /* Out-of-range values are rejected with a reason, never silently clamped,
     * and the running configuration is unchanged. */
    CHECK(le_recovery_configure(&recovery, 1, 1, 1000, reason,
                                sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "auto-timeout-range");
    CHECK(recovery.config.auto_timeout_ms == 60000);
    CHECK(le_recovery_configure(&recovery, 1, 1, 600001, reason,
                                sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "auto-timeout-range");
    CHECK(recovery.config.auto_timeout_ms == 60000);
    /* A non-boolean flag is rejected too. */
    CHECK(le_recovery_configure(&recovery, 2, 1, 60000, reason,
                                sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "enabled-invalid");

    /* Persistence failure rolls back: the runtime configuration must not take
     * a partial effect when it cannot be stored. */
    {
        char bad_parent[700];
        snprintf(bad_parent, sizeof(bad_parent), "%s/notadir", dir);
        write_file(bad_parent, "x", 0600);   /* a file where a directory is needed */
        snprintf(recovery.config.config_path,
                 sizeof(recovery.config.config_path), "%s/recovery.json",
                 bad_parent);
        CHECK(le_recovery_configure(&recovery, 1, 0, 60000, reason,
                                    sizeof(reason)) == -1);
        CHECK_STR_EQ(reason, "config-dir-not-dir");
        CHECK(recovery.config.enabled == 1);
        CHECK(recovery.config.auto_enabled == 1);   /* unchanged */
        CHECK(recovery.config.auto_timeout_ms == 60000);
    }
}

static void test_disabled_config_blocks_both_entries(const char *tmpdir)
{
    char dir[512];
    struct le_recovery_config config;
    struct le_recovery recovery;
    int i;

    snprintf(dir, sizeof(dir), "%s/disabled-config", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    config.enabled = 0;
    config.auto_enabled = 1;   /* even an opt-in auto must stay blocked */
    write_file(config.marker_path, LE_RECOVERY_MARKER_TAG "\n", 0600);

    reset_fake();
    init_fake(&recovery, &config, 0);
    /* Physical entry (boot marker) is blocked. */
    CHECK(le_recovery_arm(&recovery, LE_RECOVERY_TRIGGER_PHYSICAL, 0) == -1);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    /* Automatic entry never arms, no matter how long the window runs. */
    for (i = 0; i < 8; ++i)
        le_recovery_tick(&recovery, 1000 + (long long)i * 60000, 0);
    CHECK(recovery.mode == LE_RECOVERY_MODE_CLIENT);
    CHECK(recovery.trigger == LE_RECOVERY_TRIGGER_NONE);
    CHECK(fake.spawn_count == 0);
}

static void test_status_hides_secret_and_paths(const char *tmpdir)
{
    char dir[512], status[2048];
    struct le_recovery_config config;
    struct le_recovery recovery;

    snprintf(dir, sizeof(dir), "%s/status", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);
    reset_fake();
    init_fake(&recovery, &config, 0);
    CHECK(le_recovery_status_json(&recovery, status, sizeof(status)) > 0);
    /* The public status never carries the secret path or filesystem paths. */
    CHECK(strstr(status, "psk_path") == NULL);
    CHECK(strstr(status, "/data/") == NULL);
    CHECK(strstr(status, "recovery-psk") == NULL);
    CHECK(strstr(status, "\"enabled\":true") != NULL);
    CHECK(strstr(status, "\"auto_enabled\":false") != NULL);
    CHECK(strstr(status, "\"auto_timeout_ms\":120000") != NULL);
}

static void test_secure_parent_dir(const char *tmpdir)
{
    char dir[512], open_dir[600], link_dir[600], target[600];
    char reason[LE_RECOVERY_REASON_MAX];
    char psk[LE_RECOVERY_PSK_MAX];
    struct le_recovery_config config;

    snprintf(dir, sizeof(dir), "%s/secure", tmpdir);
    (void)mkdir(dir, 0700);
    make_config(&config, dir);

    /* A group/world-accessible parent is refused. */
    snprintf(open_dir, sizeof(open_dir), "%s/open", dir);
    (void)mkdir(open_dir, 0777);
    (void)chmod(open_dir, 0777);
    snprintf(config.psk_path, sizeof(config.psk_path), "%s/recovery-psk",
             open_dir);
    CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                 sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "config-dir-permissions");

    /* A symlinked parent is never followed. */
    snprintf(target, sizeof(target), "%s/real", dir);
    (void)mkdir(target, 0700);
    snprintf(link_dir, sizeof(link_dir), "%s/link", dir);
    CHECK(symlink(target, link_dir) == 0);
    snprintf(config.psk_path, sizeof(config.psk_path), "%s/recovery-psk",
             link_dir);
    CHECK(le_recovery_psk_ensure(&config, psk, sizeof(psk), reason,
                                 sizeof(reason)) == -1);
    CHECK_STR_EQ(reason, "config-dir-symlink");
}

int main(void)
{
    const char *scratch = getenv("LE_RECOVERY_TEST_SCRATCH");
    char tmpdir[4096];
    if (scratch && scratch[0])
        snprintf(tmpdir, sizeof(tmpdir), "%s/le-recovery-test-XXXXXX",
                 scratch);
    else
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/le-recovery-test-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        fprintf(stderr, "mkdtemp failed: %s\n", tmpdir);
        return 2;
    }
    test_config_defaults();
    test_marker(tmpdir);
    test_psk(tmpdir);
    test_capabilities(tmpdir);
    test_lifecycle(tmpdir);
    test_child_failure_fails_closed(tmpdir);
    test_bounded_start_timeout(tmpdir);
    test_rate_limit(tmpdir);
    test_auto_cutoff(tmpdir);
    test_secret_gating(tmpdir);
    test_disabled_feature(tmpdir);
    test_net_helper_fails_closed(tmpdir);
    test_single_interface_handover(tmpdir);
    test_persistent_paths_and_psk_survives_reinit(tmpdir);
    test_configure_persists_validates_and_rolls_back(tmpdir);
    test_disabled_config_blocks_both_entries(tmpdir);
    test_status_hides_secret_and_paths(tmpdir);
    test_secure_parent_dir(tmpdir);
    fprintf(stderr, "recovery unit: %d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("recovery-unit-failures=%d\n", failures);
        return 1;
    }
    printf("recovery-unit-ok=%d\n", checks);
    return 0;
}
