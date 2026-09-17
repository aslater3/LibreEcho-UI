#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#include "../src/backend_linux.c"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Real Linux-backend factory-reset fixture.
 *
 * The other reset tests cover the clearing function against a real directory
 * tree and the shape of this file as source text. Neither of them runs the
 * backend's own reset path, so neither can show that the sequence is right:
 * that the state owners are quiesced first, that the supervisor is stopped
 * before the daemons it would otherwise restart, that the reboot happens only
 * after the clear succeeded, and that a refusal puts back what was stopped.
 *
 * This test compiles the real backend and intercepts the four boundaries it
 * crosses -- the init scripts, the reboot, sync and the privilege check -- so
 * the whole path runs on the host with no device, no real init script and no
 * reboot. The intercepted reboot inspects the fixture at the moment it is
 * called, which is what proves "cleared before rebooting" rather than assuming
 * it from source order.
 */

#define WATCHDOG "/etc/init.d/libreecho-watchdogd.init"
#define BTD "/etc/init.d/libreecho-btd.init"
#define LEDD "/etc/init.d/libreecho-ledd.init"
#define NETWORKD "/etc/init.d/libreecho-networkd.init"
#define TIMERD "/etc/init.d/libreecho-timerd.init"
#define AGENTD "/etc/init.d/libreecho-agentd.init"

#define MAX_EVENTS 128
#define MAX_PATH 512

#define INIT_PREFIX "/etc/init.d/"
#define PRODUCT_INIT_PREFIX INIT_PREFIX "libreecho-"

/* Every file the reset is expected to remove. */
static const char *const reset_files[] = {
    "config/web-config.json",
    "config/web-config.json.bak",
    "config/web-config.json.setup-complete",
    "config/users",
    "config/users.tmp",
    "config/wpa_supplicant.conf",
    "config/ble-profiles",
    "config/agent.json",
    "config/agent.json.bak",
    "config/tts-voice",
    "config/vad-floor-rms",
    "config/timers",
    "config/timers.tmp",
    "config/led-state.json",
    "config/bluetooth.devices",
    "config/bluetooth.keys",
    "secrets/openai-codex.json"
};

/* Installed features, OTA state and release identity are out of reset scope. */
static const char *const preserved_files[] = {
    "features/assistant/payload.squashfs",
    "features/voice/payload.squashfs",
    "update/state",
    "update/pending",
    "data-manifest.json"
};

struct event {
    const char *script;
    const char *action;
};

struct fake_service {
    const char *script;
    int running;
};

static struct fake_service services[] = {
    {WATCHDOG, 1},
    {BTD, 1},
    {LEDD, 1},
    {NETWORKD, 1},
    {TIMERD, 1},
    {AGENTD, 1}
};

#define SERVICE_COUNT (sizeof(services) / sizeof(services[0]))

static struct event events[MAX_EVENTS];
static size_t event_count;
static unsigned sync_calls;
static unsigned reboot_calls;
static unsigned legacy_unlink_calls;
static int reboot_result;
static int reboot_saw_cleared;
static int test_euid;
static const char *stop_refusal;
static const char *fixture_root;
static const char *legacy_paths[4];
static size_t legacy_path_count;

extern int __real_access(const char *path, int mode);
extern int __real_unlink(const char *path);

static size_t events_for(const char *script, const char *action)
{
    size_t i;
    size_t total = 0;

    for (i = 0; i < event_count; ++i)
        if (!strcmp(events[i].script, script) && !strcmp(events[i].action, action))
            ++total;
    return total;
}

static size_t first_event(const char *script, const char *action)
{
    size_t i;

    for (i = 0; i < event_count; ++i)
        if (!strcmp(events[i].script, script) && !strcmp(events[i].action, action))
            return i;
    return (size_t)-1;
}

static void record_event(const char *script, const char *action)
{
    assert(event_count < MAX_EVENTS);
    events[event_count].script = script;
    events[event_count].action = action;
    ++event_count;
}

static struct fake_service *find_service(const char *script)
{
    size_t i;

    for (i = 0; i < SERVICE_COUNT; ++i)
        if (!strcmp(services[i].script, script))
            return &services[i];
    return NULL;
}

/* The init-script boundary: this is what the backend talks to. */
int __wrap_le_service_command(const char *path, const char *const *argv)
{
    struct fake_service *service;
    const char *action = argv && argv[1] ? argv[1] : "";

    record_event(path, action);
    service = find_service(path);
    if (!service)
        return -1;
    if (!strcmp(action, "status"))
        return service->running ? 0 : -1;
    if (!strcmp(action, "stop")) {
        if (stop_refusal && !strcmp(stop_refusal, path))
            return -1;
        service->running = 0;
        return 0;
    }
    if (!strcmp(action, "start")) {
        service->running = 1;
        return 0;
    }
    return -1;
}

int __wrap_geteuid(void)
{
    return test_euid;
}

void __wrap_sync(void)
{
    ++sync_calls;
}

static int fixture_cleared(const char *root)
{
    char name[MAX_PATH];
    size_t i;

    for (i = 0; i < sizeof(reset_files) / sizeof(reset_files[0]); ++i) {
        snprintf(name, sizeof(name), "%s/%s", root, reset_files[i]);
        if (access(name, F_OK) == 0)
            return 0;
    }
    return 1;
}

static int fixture_preserved(const char *root)
{
    char name[MAX_PATH];
    size_t i;

    for (i = 0; i < sizeof(preserved_files) / sizeof(preserved_files[0]); ++i) {
        snprintf(name, sizeof(name), "%s/%s", root, preserved_files[i]);
        if (access(name, F_OK) != 0)
            return 0;
    }
    return 1;
}

/*
 * A reboot may only be reached once the state above is actually gone. The
 * observation is taken here, at the moment the backend asks for it, rather
 * than inferred from the call order afterwards.
 */
int __wrap_reboot(int command)
{
    (void)command;
    ++reboot_calls;
    reboot_saw_cleared = fixture_cleared(fixture_root) && fixture_preserved(fixture_root);
    return reboot_result;
}

int __wrap_access(const char *path, int mode)
{
    /* The host has no LibreEcho init scripts, and creating them under /etc is
       not this test's business. The backend's own existence check is answered
       from the fixture instead. */
    if (!strncmp(path, PRODUCT_INIT_PREFIX, sizeof(PRODUCT_INIT_PREFIX) - 1))
        return 0;
    if (!strncmp(path, INIT_PREFIX, sizeof(INIT_PREFIX) - 1)) {
        errno = ENOENT;
        return -1;
    }
    return __real_access(path, mode);
}

int __wrap_unlink(const char *path)
{
    /* clear_legacy_bluetooth_state targets /etc, which must never be written
       by a host test. Record the attempt, report the files as absent, and let
       the source contract test keep the two paths themselves under review. */
    if (!strncmp(path, "/etc/", 5)) {
        ++legacy_unlink_calls;
        assert(legacy_path_count < sizeof(legacy_paths) / sizeof(legacy_paths[0]));
        legacy_paths[legacy_path_count++] = path;
        errno = ENOENT;
        return -1;
    }
    return __real_unlink(path);
}

static void write_ok(const char *name)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    assert(fd >= 0);
    assert(write(fd, "state\n", 6) == 6);
    assert(close(fd) == 0);
}

static void mkdir_ok(const char *name)
{
    assert(mkdir(name, 0700) == 0);
}

static void path_of(char *out, size_t size, const char *root, const char *relative)
{
    int n = snprintf(out, size, "%s/%s", root, relative);

    assert(n > 0 && (size_t)n < size);
}

static void build_tree(const char *root)
{
    char name[MAX_PATH];
    size_t i;

    path_of(name, sizeof(name), root, "config");
    mkdir_ok(name);
    path_of(name, sizeof(name), root, "secrets");
    mkdir_ok(name);
    path_of(name, sizeof(name), root, "features");
    mkdir_ok(name);
    path_of(name, sizeof(name), root, "features/assistant");
    mkdir_ok(name);
    path_of(name, sizeof(name), root, "features/voice");
    mkdir_ok(name);
    path_of(name, sizeof(name), root, "update");
    mkdir_ok(name);
    for (i = 0; i < sizeof(reset_files) / sizeof(reset_files[0]); ++i) {
        path_of(name, sizeof(name), root, reset_files[i]);
        write_ok(name);
    }
    for (i = 0; i < sizeof(preserved_files) / sizeof(preserved_files[0]); ++i) {
        path_of(name, sizeof(name), root, preserved_files[i]);
        write_ok(name);
    }
}

static void reset_simulation(void)
{
    size_t i;

    event_count = 0;
    sync_calls = 0;
    reboot_calls = 0;
    legacy_unlink_calls = 0;
    legacy_path_count = 0;
    reboot_result = 0;
    reboot_saw_cleared = -1;
    test_euid = 0;
    stop_refusal = NULL;
    for (i = 0; i < SERVICE_COUNT; ++i)
        services[i].running = 1;
}

static void use_root(const char *root)
{
    fixture_root = root;
    assert(setenv("LIBREECHO_DATA_ROOT", root, 1) == 0);
}

/* Every service is probed, stopped and confirmed stopped, in table order. */
static void assert_quiesced(void)
{
    size_t i;
    size_t previous = 0;

    for (i = 0; i < SERVICE_COUNT; ++i) {
        size_t status = first_event(services[i].script, "status");
        size_t stop = first_event(services[i].script, "stop");

        assert(status != (size_t)-1);
        assert(stop != (size_t)-1);
        assert(status < stop);
        assert(events_for(services[i].script, "status") == 2);
        assert(events_for(services[i].script, "stop") == 1);
        assert(stop > previous);
        previous = stop;
    }
}

static void assert_supervisor_stopped_first(void)
{
    size_t i;

    assert(first_event(WATCHDOG, "stop") == 0 || first_event(WATCHDOG, "status") == 0);
    for (i = 1; i < SERVICE_COUNT; ++i) {
        size_t other = first_event(services[i].script, "stop");

        assert(other != (size_t)-1);
        assert(first_event(WATCHDOG, "stop") < other);
    }
}

static void assert_restarted(void)
{
    size_t i;

    for (i = 0; i < SERVICE_COUNT; ++i)
        assert(events_for(services[i].script, "start") == 1);
    for (i = 0; i < SERVICE_COUNT; ++i)
        assert(services[i].running == 1);
}

static void assert_untouched(size_t from)
{
    size_t i;

    for (i = from; i < SERVICE_COUNT; ++i) {
        assert(events_for(services[i].script, "status") == 0);
        assert(events_for(services[i].script, "stop") == 0);
        assert(events_for(services[i].script, "start") == 0);
    }
}

static void assert_legacy_state_targeted(void)
{
    assert(legacy_unlink_calls == 2);
    assert(!strcmp(legacy_paths[0], "/etc/libreecho/bluetooth.devices"));
    assert(!strcmp(legacy_paths[1], "/etc/libreecho/bluetooth.keys"));
}

static void test_successful_reset(void)
{
    char template[] = "/tmp/libreecho-reset-linux.XXXXXX";
    char *root = mkdtemp(template);

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    use_root(root);
    assert(factory_reset(NULL) == LE_OK);
    assert_quiesced();
    assert_supervisor_stopped_first();
    assert(fixture_cleared(root));
    assert(fixture_preserved(root));
    assert_legacy_state_targeted();
    assert(sync_calls >= 1);
    assert(reboot_calls == 1);
    assert(reboot_saw_cleared == 1);
    assert(event_count == 3 * SERVICE_COUNT);
}

static void test_reboot_refusal_restores_writers(void)
{
    char template[] = "/tmp/libreecho-reset-linux-reboot.XXXXXX";
    char *root = mkdtemp(template);

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    use_root(root);
    reboot_result = -1;
    assert(factory_reset(NULL) == LE_IO);
    assert(reboot_calls == 1);
    assert(fixture_cleared(root));
    assert_restarted();
}

static void test_clear_failure_restores_writers(void)
{
    char template[] = "/tmp/libreecho-reset-linux-clear.XXXXXX";
    char *root = mkdtemp(template);
    char missing[MAX_PATH];

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    path_of(missing, sizeof(missing), root, "absent");
    use_root(missing);
    assert(factory_reset(NULL) == LE_IO);
    assert(reboot_calls == 0);
    assert_restarted();
    /* The fixture itself was never the configured root, so it is intact. */
    assert(!fixture_cleared(root));
}

static void test_unprivileged_refusal(void)
{
    char template[] = "/tmp/libreecho-reset-linux-euid.XXXXXX";
    char *root = mkdtemp(template);
    char name[MAX_PATH];

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    use_root(root);
    test_euid = 1000;
    assert(factory_reset(NULL) == LE_NOT_SUPPORTED);
    assert(event_count == 0);
    assert(reboot_calls == 0);
    path_of(name, sizeof(name), root, "config/users");
    assert(access(name, F_OK) == 0);
}

static void test_stop_refusal_aborts_before_clearing(void)
{
    char template[] = "/tmp/libreecho-reset-linux-refuse.XXXXXX";
    char *root = mkdtemp(template);
    char name[MAX_PATH];

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    use_root(root);
    stop_refusal = LEDD;
    assert(factory_reset(NULL) == LE_IO);
    assert(reboot_calls == 0);
    /* Everything stopped before the refusal is put back, and nothing after it
       is touched at all. */
    assert(events_for(WATCHDOG, "start") == 1);
    assert(events_for(BTD, "start") == 1);
    assert(events_for(LEDD, "stop") == 1);
    assert(events_for(LEDD, "start") == 0);
    /* The daemon that refused to stop is still running, and the reset stops
       there rather than clearing state underneath a live writer. */
    assert(services[2].running == 1);
    assert_untouched(3);
    /* The refusal happens before anything is removed. */
    assert(!fixture_cleared(root));
    path_of(name, sizeof(name), root, "secrets/openai-codex.json");
    assert(access(name, F_OK) == 0);
    path_of(name, sizeof(name), root, "config/users");
    assert(access(name, F_OK) == 0);
}

static void test_late_stop_refusal_leaves_earlier_writers_done(void)
{
    char template[] = "/tmp/libreecho-reset-linux-late.XXXXXX";
    char *root = mkdtemp(template);

    assert(root != NULL);
    reset_simulation();
    build_tree(root);
    use_root(root);
    stop_refusal = AGENTD;
    assert(factory_reset(NULL) == LE_IO);
    assert(reboot_calls == 0);
    assert(events_for(LEDD, "start") == 1);
    assert(events_for(NETWORKD, "start") == 1);
    assert(events_for(TIMERD, "start") == 1);
    assert(events_for(AGENTD, "start") == 0);
    assert(events_for(AGENTD, "stop") == 1);
    assert(!fixture_cleared(root));
}

int main(void)
{
    test_successful_reset();
    test_reboot_refusal_restores_writers();
    test_clear_failure_restores_writers();
    test_unprivileged_refusal();
    test_stop_refusal_aborts_before_clearing();
    test_late_stop_refusal_leaves_earlier_writers_done();
    puts("linux backend factory reset: quiesce, clear, durability, reboot and refusals: ok");
    return 0;
}
