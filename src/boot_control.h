#ifndef LE_BOOT_CONTROL_H
#define LE_BOOT_CONTROL_H

#include <stddef.h>

/* Boot-slot diagnostics written by libreecho-init.
 *
 * The preloader refuses a slot whose retry count reaches zero while success is
 * 0, and only `libreecho-bootctl confirm` clears that countdown, so a device
 * that is never confirmed boots a finite number of times and then stops with no
 * ADB and no other evidence.  init records the countdown and its verdict on the
 * persistent filesystem while the device still boots; this module renders those
 * records for the API and the diagnostic bundle.  The record is a small flat
 * key=value file and the history is a bounded ring, so every field is bounded
 * here too. */

#define LE_BOOT_CONTROL_TEXT 64
#define LE_BOOT_CONTROL_HISTORY_MAX 8
#define LE_BOOT_CONTROL_HISTORY_LINE 256
#define LE_BOOT_CONTROL_JSON_MAX 4096

struct le_boot_control {
    int available;
    char state[LE_BOOT_CONTROL_TEXT];
    char mode[LE_BOOT_CONTROL_TEXT];
    char running_slot[LE_BOOT_CONTROL_TEXT];
    char selected_slot[LE_BOOT_CONTROL_TEXT];
    char pending_slot[LE_BOOT_CONTROL_TEXT];
    char last_check[LE_BOOT_CONTROL_TEXT];
    int slot_a_priority, slot_a_tries, slot_a_success;
    int slot_b_priority, slot_b_tries, slot_b_success;
    int boot_count, attempt, passed, strict_graph;
    int confirmed;
    int running_slot_tries, running_slot_success;
    size_t history_count;
    char history[LE_BOOT_CONTROL_HISTORY_MAX][LE_BOOT_CONTROL_HISTORY_LINE];
};

void le_boot_control_read(struct le_boot_control *value);
int le_boot_control_json(char *out, size_t out_size, const struct le_boot_control *value);

#endif
