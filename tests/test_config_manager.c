#include "../config_manager.h"
#include "../json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } else { \
        printf("  ok: %s\n", msg); \
    } \
} while (0)

static void write_test_config(const char *path)
{
    FILE *f = fopen(path, "w");
    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"system\": {\n");
    fprintf(f, "    \"hostname\": \"test-echo\",\n");
    fprintf(f, "    \"log_level\": \"debug\"\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"audio\": {\n");
    fprintf(f, "    \"volume\": 42,\n");
    fprintf(f, "    \"microphone_gain\": 65,\n");
    fprintf(f, "    \"microphone_muted\": false\n");
    fprintf(f, "  },\n");
    fprintf(f, "  \"led\": {\n");
    fprintf(f, "    \"brightness\": 80\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);
}

int main(void)
{
    const char *test_path = "/tmp/le_config_test.json";
    char section[4096];
    char str_val[128];
    int int_val;
    int bool_val;

    write_test_config(test_path);

    /* Test init */
    printf("=== le_config_init ===\n");
    CHECK(le_config_init(test_path) == 0, "init with valid config");
    CHECK(le_config_init("/nonexistent/config.json") != 0, "init with missing config fails");

    /* Test section extraction */
    printf("=== le_config_section ===\n");
    le_config_init(test_path);
    CHECK(le_config_section("audio", section, sizeof(section)) == 0, "extract audio section");
    CHECK(strstr(section, "\"volume\"") != NULL, "audio section contains volume");
    CHECK(le_config_section("nonexistent", section, sizeof(section)) != 0, "missing section fails");

    /* Test typed getters */
    printf("=== le_config_get_int ===\n");
    CHECK(le_config_get_int("audio", "volume", &int_val) == 0, "get audio.volume");
    CHECK(int_val == 42, "audio.volume == 42");
    CHECK(le_config_get_int("audio", "microphone_gain", &int_val) == 0, "get audio.microphone_gain");
    CHECK(int_val == 65, "audio.microphone_gain == 65");
    CHECK(le_config_get_int("led", "brightness", &int_val) == 0, "get led.brightness");
    CHECK(int_val == 80, "led.brightness == 80");
    CHECK(le_config_get_int("audio", "nonexistent", &int_val) != 0, "missing key fails");

    printf("=== le_config_get_bool ===\n");
    CHECK(le_config_get_bool("audio", "microphone_muted", &bool_val) == 0, "get audio.microphone_muted");
    CHECK(bool_val == 0, "audio.microphone_muted == false");

    printf("=== le_config_get_string ===\n");
    CHECK(le_config_get_string("system", "hostname", str_val, sizeof(str_val)) == 0, "get system.hostname");
    CHECK(strcmp(str_val, "test-echo") == 0, "system.hostname == 'test-echo'");
    CHECK(le_config_get_string("system", "log_level", str_val, sizeof(str_val)) == 0, "get system.log_level");
    CHECK(strcmp(str_val, "debug") == 0, "system.log_level == 'debug'");

    /* Test reload */
    printf("=== le_config_reload ===\n");
    le_config_init(test_path);
    CHECK(le_config_reload() == 0, "reload same config");

    /* Modify and reload */
    write_test_config(test_path);
    FILE *f = fopen(test_path, "r");
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Change volume 42 -> 99 */
    char *p = strstr(buf, "\"volume\": 42");
    if (p) {
        p[10] = '9';
        p[11] = '9';
        f = fopen(test_path, "w");
        fwrite(buf, 1, n, f);
        fclose(f);
        CHECK(le_config_reload() == 0, "reload modified config");
        CHECK(le_config_get_int("audio", "volume", &int_val) == 0, "get updated audio.volume");
        CHECK(int_val == 99, "audio.volume == 99 after reload");
    } else {
        CHECK(0, "could not find volume to modify");
    }

    /* Test write */
    printf("=== le_config_write ===\n");
    const char *new_config = "{\"version\":1,\"audio\":{\"volume\":77}}";
    const char *write_path = "/tmp/le_config_write_test.json";
    le_config_init(write_path); /* may fail, that's ok */
    CHECK(le_config_write(new_config, strlen(new_config)) == 0, "write new config");
    CHECK(le_config_init(write_path) == 0, "reload written config");
    CHECK(le_config_get_int("audio", "volume", &int_val) == 0, "get written audio.volume");
    CHECK(int_val == 77, "audio.volume == 77 after write");

    /* Cleanup */
    unlink(test_path);
    unlink(write_path);

    if (failures) {
        printf("\n%d failures\n", failures);
        return 1;
    }
    printf("\nAll config_manager tests passed\n");
    return 0;
}
