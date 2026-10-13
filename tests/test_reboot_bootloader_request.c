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
#include <unistd.h>

/*
 * Focused check for the restart-to-installer request file. The daemon only
 * publishes "fastboot\n" to the request path; init does the rest. This test
 * drives linux_reboot_bootloader() itself against a private directory through
 * LIBREECHO_REBOOT_REQUEST, with geteuid() and reboot() wrapped so it runs
 * without root and proves the daemon never reboots on its own.
 */

static int test_euid = 0;
static int reboot_calls = 0;

int __wrap_geteuid(void)
{
    return test_euid;
}

int __wrap_reboot(int command)
{
    (void)command;
    ++reboot_calls;
    return -1;
}

static char root[256];
static char request[320];
static char temporary[330];

static void make_root(void)
{
    const char *base = getenv("TMPDIR");

    if (!base || !base[0])
        base = "/tmp";
    snprintf(root, sizeof(root), "%s/libreecho-reboot-bootloader-XXXXXX", base);
    assert(mkdtemp(root) != NULL);
    snprintf(request, sizeof(request), "%s/reboot.request", root);
    snprintf(temporary, sizeof(temporary), "%s.webd", request);
    assert(setenv("LIBREECHO_REBOOT_REQUEST", request, 1) == 0);
}

static void remove_tree(void)
{
    unlink(request);
    unlink(temporary);
    rmdir(root);
}

static int read_file(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
    return (int)n;
}

static int write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    fputs(contents, f);
    return fclose(f);
}

static void test_unprivileged_is_refused(void)
{
    struct le_backend backend;

    test_euid = 1000;
    reboot_calls = 0;
    assert(linux_reboot_bootloader(&backend) == LE_NOT_SUPPORTED);
    assert(access(request, F_OK) != 0);
    assert(reboot_calls == 0);
    test_euid = 0;
}

static void test_writes_fastboot_request(void)
{
    struct le_backend backend;
    char contents[32];
    struct stat status;

    test_euid = 0;
    reboot_calls = 0;
    assert(linux_reboot_bootloader(&backend) == LE_OK);
    assert(read_file(request, contents, sizeof(contents)) == 9);
    assert(strcmp(contents, "fastboot\n") == 0);
    assert(stat(request, &status) == 0);
    assert((status.st_mode & 0777) == 0600);
    assert(access(temporary, F_OK) != 0);
    assert(reboot_calls == 0);
}

static void test_repeat_request_is_idempotent(void)
{
    struct le_backend backend;

    test_euid = 0;
    assert(linux_reboot_bootloader(&backend) == LE_OK);
    assert(linux_reboot_bootloader(&backend) == LE_OK);
}

static void test_conflicting_request_is_refused(void)
{
    struct le_backend backend;
    char contents[32];

    test_euid = 0;
    assert(write_file(request, "reboot\n") == 0);
    assert(linux_reboot_bootloader(&backend) == LE_IO);
    assert(read_file(request, contents, sizeof(contents)) == 7);
    assert(strcmp(contents, "reboot\n") == 0);
    assert(access(temporary, F_OK) != 0);
    unlink(request);
}

static void test_stale_temporary_is_replaced(void)
{
    struct le_backend backend;
    char contents[32];

    test_euid = 0;
    assert(write_file(temporary, "stale\n") == 0);
    assert(linux_reboot_bootloader(&backend) == LE_OK);
    assert(read_file(request, contents, sizeof(contents)) == 9);
    assert(strcmp(contents, "fastboot\n") == 0);
    assert(access(temporary, F_OK) != 0);
    unlink(request);
}

static void test_missing_directory_fails(void)
{
    struct le_backend backend;

    test_euid = 0;
    snprintf(request, sizeof(request), "%s/missing/reboot.request", root);
    assert(setenv("LIBREECHO_REBOOT_REQUEST", request, 1) == 0);
    assert(linux_reboot_bootloader(&backend) == LE_IO);
    assert(reboot_calls == 0);
    snprintf(request, sizeof(request), "%s/reboot.request", root);
    assert(setenv("LIBREECHO_REBOOT_REQUEST", request, 1) == 0);
}

int main(void)
{
    make_root();
    test_unprivileged_is_refused();
    test_writes_fastboot_request();
    test_repeat_request_is_idempotent();
    test_conflicting_request_is_refused();
    test_stale_temporary_is_replaced();
    test_missing_directory_fails();
    remove_tree();
    puts("reboot to bootloader request: privilege gate, atomic fastboot write, conflict refusal: ok");
    return 0;
}
