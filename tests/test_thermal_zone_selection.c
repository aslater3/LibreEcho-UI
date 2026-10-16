#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#define main backend_linux_test_main

static char thermal_root[256] = "/tmp/libreecho-thermal-XXXXXX";
#define LE_THERMAL_SYSFS_ROOT thermal_root
#include "../src/backend_linux.c"
#undef LE_THERMAL_SYSFS_ROOT
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_zone_file(const char *root, unsigned zone, const char *name,
                           const char *value)
{
    char path[4096];
    FILE *f;
    int rc = 0;

    if (snprintf(path, sizeof(path), "%s/thermal_zone%u/%s", root, zone,
                 name) >= (int)sizeof(path))
        return -1;
    f = fopen(path, "w");
    if (!f)
        return -1;
    if (fprintf(f, "%s\n", value) < 0)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

static int create_zone(const char *root, unsigned zone, const char *type,
                       const char *temperature)
{
    char path[4096];

    if (snprintf(path, sizeof(path), "%s/thermal_zone%u", root, zone) >=
        (int)sizeof(path))
        return -1;
    if (mkdir(path, 0700) != 0)
        return -1;
    if (write_zone_file(root, zone, "type", type) != 0 ||
        write_zone_file(root, zone, "temp", temperature) != 0)
        return -1;
    return 0;
}

static void cleanup_fixture(void)
{
    char path[4096];
    unsigned zone;
    size_t i;
    const char *names[] = {"type", "temp"};

    for (zone = 0; zone < 2; ++zone) {
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (snprintf(path, sizeof(path), "%s/thermal_zone%u/%s",
                         thermal_root, zone, names[i]) < (int)sizeof(path))
                unlink(path);
        }
        if (snprintf(path, sizeof(path), "%s/thermal_zone%u", thermal_root,
                     zone) < (int)sizeof(path))
            rmdir(path);
    }
    rmdir(thermal_root);
}

int main(void)
{
    int temperature;

    if (!mkdtemp(thermal_root)) {
        perror("mkdtemp");
        return 1;
    }
    if (atexit(cleanup_fixture) != 0)
        return 1;
    if (create_zone(thermal_root, 0, "mtkts_bts0", "70000") != 0 ||
        create_zone(thermal_root, 1, "cpu-thermal", "55000") != 0) {
        fprintf(stderr, "failed to create thermal-zone fixture\n");
        return 1;
    }

    temperature = read_temperature();
    if (temperature != 55) {
        fprintf(stderr,
                "thermal-zone order regression: expected cpu-thermal 55 C, got %d C\n",
                temperature);
        return 1;
    }

    printf("thermal-zone ranking: reversed mtkts_bts0/cpu-thermal order PASS\n");
    return 0;
}
