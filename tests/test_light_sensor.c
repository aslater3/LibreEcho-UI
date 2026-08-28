#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_LIGHT_SYSFS_ROOT "/tmp/libreecho-light-sensor"

#include "../src/backend_linux.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const fixture_root = LE_LIGHT_SYSFS_ROOT "/1-0039";

static int write_fixture(const char *name, const char *value)
{
    char path[256];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/%s", fixture_root, name) >=
        (int)sizeof(path))
        return -1;
    file = fopen(path, "w");
    if (!file)
        return -1;
    if (fprintf(file, "%s\n", value) < 0 || fclose(file) != 0)
        return -1;
    return 0;
}

static void cleanup_fixture(void)
{
    static const char *const files[] = {
        "als_lux", "als_calibrated_lux", "als_ch0", "als_ch1", "als_gain",
        "als_power_state", "als_itime", "als_auto_gain"
    };
    char path[256];
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture_root, files[i]) <
            (int)sizeof(path))
            unlink(path);
    }
    rmdir(fixture_root);
    rmdir(LE_LIGHT_SYSFS_ROOT);
}

int main(void)
{
    struct le_light_state reading;

    if (mkdir(LE_LIGHT_SYSFS_ROOT, 0700) != 0 ||
        mkdir(fixture_root, 0700) != 0) {
        perror("mkdir light fixture");
        cleanup_fixture();
        return 1;
    }
    if (atexit(cleanup_fixture) != 0 ||
        write_fixture("als_lux", "0") != 0 ||
        write_fixture("als_calibrated_lux", "0") != 0 ||
        write_fixture("als_ch0", "93") != 0 ||
        write_fixture("als_ch1", "190") != 0 ||
        write_fixture("als_gain", "64") != 0 ||
        write_fixture("als_power_state", "1") != 0 ||
        write_fixture("als_itime", "346ms (346368us)") != 0 ||
        write_fixture("als_auto_gain", "manual") != 0) {
        fprintf(stderr, "failed to create light fixture\n");
        return 1;
    }

    memset(&reading, 0, sizeof(reading));
    if (light(NULL, &reading) != LE_OK || !reading.available ||
        reading.lux != 0 || reading.calibrated_lux != 0 ||
        reading.ch0 != 93 || reading.ch1 != 190 || reading.gain != 64 ||
        reading.integration_us != 346368 || reading.auto_gain ||
        !reading.powered || strcmp(reading.bus, "i2c 1-0039")) {
        fprintf(stderr,
                "light fixture mismatch: available=%d lux=%d calibrated=%d "
                "ch0=%d ch1=%d gain=%d itime=%d auto=%d powered=%d bus=%s\n",
                reading.available, reading.lux, reading.calibrated_lux,
                reading.ch0, reading.ch1, reading.gain,
                reading.integration_us, reading.auto_gain, reading.powered,
                reading.bus);
        return 1;
    }

    puts("light sensor: zero-lux reading and detected I2C bus PASS");
    return 0;
}
