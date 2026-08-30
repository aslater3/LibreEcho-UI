#define _POSIX_C_SOURCE 200809L

#include "factory_reset.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *const reset_files[] = {
    "config/web-config.json",
    "config/web-config.json.bak",
    "config/web-config.json.tmp",
    "config/web-config.json.setup-complete",
    "config/users",
    "config/users.tmp",
    "config/wpa_supplicant.conf",
    "config/ntp.conf",
    "config/agent.json",
    "config/agent.json.bak",
    "config/agent.json.tmp",
    "config/tts-voice",
    "config/vad-floor-rms",
    "config/timers",
    "config/timers.bak",
    "config/timers.tmp",
    "config/timers.bak.tmp",
    "config/led-state.json",
    "config/led-state.json.tmp",
    "secrets/openai-codex.json",
    "secrets/openai-codex.json.tmp"
};

static void path(char *out, size_t size, const char *root, const char *relative)
{
    int n = snprintf(out, size, "%s/%s", root, relative);
    assert(n > 0 && (size_t)n < size);
}

static void mkdir_ok(const char *name)
{
    assert(mkdir(name, 0700) == 0);
}

static void write_ok(const char *name)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, "state\n", 6) == 6);
    assert(close(fd) == 0);
}

static void build_tree(const char *root)
{
    char name[512];
    size_t i;

    path(name, sizeof(name), root, "config");
    mkdir_ok(name);
    path(name, sizeof(name), root, "secrets");
    mkdir_ok(name);
    path(name, sizeof(name), root, "features");
    mkdir_ok(name);
    path(name, sizeof(name), root, "features/assistant");
    mkdir_ok(name);
    path(name, sizeof(name), root, "update");
    mkdir_ok(name);
    path(name, sizeof(name), root, "config/future-component");
    mkdir_ok(name);
    path(name, sizeof(name), root, "secrets/future-provider");
    mkdir_ok(name);
    for (i = 0; i < sizeof(reset_files) / sizeof(reset_files[0]); ++i) {
        path(name, sizeof(name), root, reset_files[i]);
        write_ok(name);
    }
    path(name, sizeof(name), root, "features/assistant/payload.squashfs");
    write_ok(name);
    path(name, sizeof(name), root, "update/state");
    write_ok(name);
    path(name, sizeof(name), root, "data-manifest.json");
    write_ok(name);
    path(name, sizeof(name), root, "config/future-component/state.json");
    write_ok(name);
    path(name, sizeof(name), root, "config/future-setting.json");
    write_ok(name);
    path(name, sizeof(name), root, "secrets/future-provider/token.json");
    write_ok(name);
    path(name, sizeof(name), root, "config/nonblocking-fifo");
    assert(mkfifo(name, 0600) == 0);
}

static void assert_reset(const char *root)
{
    char name[512];
    size_t i;

    for (i = 0; i < sizeof(reset_files) / sizeof(reset_files[0]); ++i) {
        path(name, sizeof(name), root, reset_files[i]);
        assert(access(name, F_OK) != 0 && errno == ENOENT);
    }
    path(name, sizeof(name), root, "features/assistant/payload.squashfs");
    assert(access(name, F_OK) == 0);
    path(name, sizeof(name), root, "update/state");
    assert(access(name, F_OK) == 0);
    path(name, sizeof(name), root, "data-manifest.json");
    assert(access(name, F_OK) == 0);
    path(name, sizeof(name), root, "config/future-component");
    assert(access(name, F_OK) != 0 && errno == ENOENT);
    path(name, sizeof(name), root, "config/future-setting.json");
    assert(access(name, F_OK) != 0 && errno == ENOENT);
    path(name, sizeof(name), root, "secrets/future-provider");
    assert(access(name, F_OK) != 0 && errno == ENOENT);
}

int main(void)
{
    char template[] = "/tmp/libreecho-factory-reset.XXXXXX";
    char failed_template[] = "/tmp/libreecho-factory-reset-fail.XXXXXX";
    char symlink_template[] = "/tmp/libreecho-factory-reset-link.XXXXXX";
    char target_template[] = "/tmp/libreecho-factory-reset-target.XXXXXX";
    char name[512];
    char target_name[512];
    char *root = mkdtemp(template);
    char *failed_root = mkdtemp(failed_template);
    char *symlink_root = mkdtemp(symlink_template);
    char *target_root = mkdtemp(target_template);

    assert(root != NULL && failed_root != NULL);
    assert(symlink_root != NULL && target_root != NULL);
    build_tree(root);
    path(target_name, sizeof(target_name), target_root, "preserved-target");
    write_ok(target_name);
    path(name, sizeof(name), root, "config/external-link");
    assert(symlink(target_name, name) == 0);
    assert(le_factory_reset_clear(root) == 0);
    assert_reset(root);
    assert(access(target_name, F_OK) == 0);

    build_tree(failed_root);
    path(name, sizeof(name), failed_root, "config");
    assert(chmod(name, 0500) == 0);
    assert(le_factory_reset_clear(failed_root) != 0);
    assert(chmod(name, 0700) == 0);
    path(name, sizeof(name), failed_root, "features/assistant/payload.squashfs");
    assert(access(name, F_OK) == 0);

    path(name, sizeof(name), symlink_root, "secrets");
    mkdir_ok(name);
    path(target_name, sizeof(target_name), target_root, "users");
    write_ok(target_name);
    path(name, sizeof(name), symlink_root, "config");
    assert(symlink(target_root, name) == 0);
    assert(le_factory_reset_clear(symlink_root) != 0);
    assert(access(target_name, F_OK) == 0);

    puts("factory reset clears all config and secrets only: ok");
    return 0;
}
