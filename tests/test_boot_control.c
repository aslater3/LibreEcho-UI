/* Boot-slot diagnostics reader: bounds, hostile content, and JSON shape.
 *
 * The records are written by libreecho-init and read back on a device that may
 * already be on its last boot, so the reader must never trust their contents:
 * every field is bounded, numbers must be numbers, tokens must be tokens. */
#define _POSIX_C_SOURCE 200809L
#include "boot_control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char root_template[] = "/tmp/libreecho-boot-control-XXXXXX";
static char root[sizeof(root_template)];

static void write_text(const char *name, const char *text)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void setup(void)
{
    strcpy(root, root_template);
    assert(mkdtemp(root));
    assert(setenv("LIBREECHO_UPDATE_ROOT", root, 1) == 0);
}

static void test_missing_records(void)
{
    struct le_boot_control boot;
    char json[LE_BOOT_CONTROL_JSON_MAX];
    setup();
    le_boot_control_read(&boot);
    assert(boot.available == 0);
    assert(!strcmp(boot.state, "unavailable"));
    assert(boot.confirmed == 0);
    assert(boot.boot_count == 0);
    assert(boot.running_slot_tries == -1);
    assert(boot.history_count == 0);
    assert(le_boot_control_json(json, sizeof(json), &boot) == 0);
    assert(strstr(json, "\"available\":false"));
}

static void test_unconfirmed_failed_record(void)
{
    struct le_boot_control boot;
    setup();
    write_text("boot-health",
               "schema=1\n"
               "state=failed\n"
               "mode=first-boot\n"
               "running_slot=a\n"
               "selected_slot=a\n"
               "pending_slot=-\n"
               "slot_a_priority=15\n"
               "slot_a_tries=0\n"
               "slot_a_success=0\n"
               "slot_b_priority=14\n"
               "slot_b_tries=7\n"
               "slot_b_success=0\n"
               "boot_count=4\n"
               "attempt=6\n"
               "passed=1\n"
               "last_check=startup-ready-marker-missing\n"
               "strict_graph=0\n"
               "epoch=1700000000\n");
    write_text("boot-history",
               "epoch=1699999000 boot=3 mode=first-boot running=a selected=a pending=- "
               "a=15/1/0 b=14/7/0 prev=pending:none prev_slot=a\n"
               "epoch=1700000000 boot=4 mode=first-boot running=a selected=a pending=- "
               "a=15/0/0 b=14/7/0 prev=failed:startup-ready-marker-missing prev_slot=a\n");
    le_boot_control_read(&boot);
    assert(boot.available == 1);
    assert(!strcmp(boot.state, "failed"));
    assert(!strcmp(boot.mode, "first-boot"));
    assert(boot.confirmed == 0);
    assert(!strcmp(boot.running_slot, "a"));
    assert(!strcmp(boot.selected_slot, "a"));
    assert(!strcmp(boot.pending_slot, "-"));
    assert(!strcmp(boot.last_check, "startup-ready-marker-missing"));
    assert(boot.slot_a_priority == 15 && boot.slot_a_tries == 0 && boot.slot_a_success == 0);
    assert(boot.slot_b_priority == 14 && boot.slot_b_tries == 7 && boot.slot_b_success == 0);
    assert(boot.running_slot_tries == 0);
    assert(boot.running_slot_success == 0);
    assert(boot.boot_count == 4);
    assert(boot.attempt == 6 && boot.passed == 1);
    assert(boot.strict_graph == 0);
    assert(boot.history_count == 2);
    assert(strstr(boot.history[1], "prev=failed:startup-ready-marker-missing"));
}

static void test_confirmed_record_and_json(void)
{
    struct le_boot_control boot;
    char json[LE_BOOT_CONTROL_JSON_MAX];
    char small[64];
    setup();
    write_text("boot-health",
               "state=confirmed\nmode=first-boot\nrunning_slot=b\nselected_slot=b\n"
               "pending_slot=-\nslot_a_priority=14\nslot_a_tries=0\nslot_a_success=1\n"
               "slot_b_priority=15\nslot_b_tries=0\nslot_b_success=1\nboot_count=2\n"
               "attempt=1\npassed=3\nlast_check=none\nstrict_graph=1\n");
    le_boot_control_read(&boot);
    assert(boot.confirmed == 1);
    assert(boot.running_slot_tries == 0);
    assert(boot.running_slot_success == 1);
    assert(boot.strict_graph == 1);
    assert(le_boot_control_json(json, sizeof(json), &boot) == 0);
    assert(strstr(json, "\"confirmed\":true"));
    assert(strstr(json, "\"running_slot\":\"b\""));
    assert(strstr(json, "\"slot_b\":{\"priority\":15,\"tries\":0,\"success\":1}"));
    assert(strstr(json, "\"history\":[]"));
    /* A buffer that cannot hold the record must fail rather than truncate. */
    assert(le_boot_control_json(small, sizeof(small), &boot) == -1);
}

static void test_hostile_record_is_filtered(void)
{
    struct le_boot_control boot;
    char json[LE_BOOT_CONTROL_JSON_MAX];
    setup();
    write_text("boot-health",
               "state=<script>alert(1)</script>\n"
               "mode=$(reboot)\n"
               "running_slot=\"a\"\n"
               "last_check=../../etc/passwd\n"
               "slot_a_tries=99999999999999999999\n"
               "slot_a_success=\n"
               "boot_count=not-a-number\n");
    write_text("boot-count", "7\n");
    write_text("boot-history", "epoch=1 boot=\"2\" \x01\x02<script>\n");
    le_boot_control_read(&boot);
    assert(boot.available == 1);
    assert(!strcmp(boot.state, "unknown"));
    assert(!strcmp(boot.mode, "unknown"));
    assert(!strcmp(boot.running_slot, "-"));
    assert(boot.slot_a_tries == -1);
    assert(boot.slot_a_success == -1);
    /* The counter file is the fallback identity when the record lacks it. */
    assert(boot.boot_count == 7);
    assert(boot.history_count == 1);
    /* History is rendered as text: structural characters are dropped. */
    assert(strchr(boot.history[0], '<') == NULL);
    assert(strchr(boot.history[0], '"') == NULL);
    assert(strchr(boot.history[0], '\x01') == NULL);
    assert(le_boot_control_json(json, sizeof(json), &boot) == 0);
    /* last_check is a free-form reason token, so it is filtered to a character
       set that cannot carry a path or break out of JSON rather than matched
       against a vocabulary. */
    assert(strchr(json, '<') == NULL);
    assert(strchr(json, '/') == NULL);
    assert(strstr(json, "\"state\":\"unknown\""));
}

static void test_history_is_bounded(void)
{
    struct le_boot_control boot;
    char text[LE_BOOT_CONTROL_HISTORY_MAX * LE_BOOT_CONTROL_HISTORY_LINE * 2];
    size_t used = 0;
    int i;
    setup();
    for (i = 0; i < 20 && used < sizeof(text) - 64; ++i) {
        int n = snprintf(text + used, sizeof(text) - used, "epoch=%d boot=%d\n", 1700000000 + i, i);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    write_text("boot-history", text);
    le_boot_control_read(&boot);
    assert(boot.history_count == LE_BOOT_CONTROL_HISTORY_MAX);
}

int main(void)
{
    test_missing_records();
    test_unconfirmed_failed_record();
    test_confirmed_record_and_json();
    test_hostile_record_is_filtered();
    test_history_is_bounded();
    printf("boot control diagnostics: ok\n");
    return 0;
}
