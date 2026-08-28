#define _POSIX_C_SOURCE 200809L
#define main timerd_program_main
#include "../src/adapter/timerd.c"
#undef main

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SYNCED_EPOCH 1767225600LL
#define BOOT_EPOCH 10LL

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static void read_text(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "r");
    size_t length;

    assert(file != NULL);
    length = fread(out, 1, size - 1, file);
    assert(!ferror(file));
    out[length] = '\0';
    assert(fclose(file) == 0);
}

int main(void)
{
    const char *path = "/tmp/libreecho-timer-persistence-test";
    const char *dirty_path = "/tmp/libreecho-timer-persistence-dirty-test";
    char backup[128];
    char dirty_backup[128];
    char text[256];
    struct context context;
    struct context dirty_context;
    struct stat info;
    unsigned int fired[LE_TIMER_MAX];

    snprintf(backup, sizeof(backup), "%s.bak", path);
    snprintf(dirty_backup, sizeof(dirty_backup), "%s.bak", dirty_path);
    unlink(path);
    unlink(backup);
    unlink("/tmp/libreecho-timer-persistence-test.tmp");
    unlink("/tmp/libreecho-timer-persistence-test.bak.tmp");
    unlink(dirty_path);
    unlink(dirty_backup);
    unlink("/tmp/libreecho-timer-persistence-dirty-test.tmp");
    unlink("/tmp/libreecho-timer-persistence-dirty-test.bak.tmp");

    memset(&context, 0, sizeof(context));
    context.state_path = path;
    le_timer_set_init(&context.timers);
    write_text(path, "alarm 1767225630 wake\ncountdown 1767225630 pasta\n");

    /* An invalid boot clock must leave the file and records untouched. */
    assert(state_load_at(&context, 1000, 10) == 0);
    assert(le_timer_active_count(&context.timers) == 0);
    read_text(path, text, sizeof(text));
    assert(strstr(text, "alarm 1767225630 wake") != NULL);

    /* A countdown created before NTP remains dirty until a valid-clock save
       succeeds; an invalid-clock save must not make the caller forget it. */
    memset(&dirty_context, 0, sizeof(dirty_context));
    dirty_context.state_path = dirty_path;
    le_timer_set_init(&dirty_context.timers);
    write_text(dirty_path, "old state\n");
    assert(le_timer_add_countdown(&dirty_context.timers, 600, "pre-ntp", 0,
                                  NULL) == LE_TIMER_OK);
    dirty_context.dirty = 1;
    save_dirty_state_at(&dirty_context, 1000, BOOT_EPOCH);
    assert(dirty_context.dirty == 1);
    read_text(dirty_path, text, sizeof(text));
    assert(!strcmp(text, "old state\n"));
    save_dirty_state_at(&dirty_context, 1000, SYNCED_EPOCH);
    assert(dirty_context.dirty == 0);
    read_text(dirty_path, text, sizeof(text));
    assert(strstr(text, "countdown ") == text);

    /* Once NTP makes the clock valid, both records restore deterministically. */
    assert(state_load_at(&context, 5000, SYNCED_EPOCH + 60) == 1);
    assert(le_timer_active_count(&context.timers) == 2);
    assert(le_timer_step(&context.timers, 5000, SYNCED_EPOCH + 60,
                         fired, LE_TIMER_MAX) == 2);
    assert(le_timer_ringing_count(&context.timers) == 2);

    /* Records older than the scheduler grace are reported as missed. */
    le_timer_set_init(&context.timers);
    write_text(path, "alarm 1767225599 stale\n");
    assert(state_load_at(&context, 5000, SYNCED_EPOCH + 60) == 1);
    assert(le_timer_active_count(&context.timers) == 0);
    assert(context.timers.missed == 1);

    /* Saves are private, preserve the previous copy, and leave no temp file. */
    le_timer_set_init(&context.timers);
    assert(le_timer_add_countdown(&context.timers, 600, "new", 0, NULL) ==
           LE_TIMER_OK);
    write_text(path, "old state\n");
    assert(chmod(path, 0644) == 0);
    state_save(&context);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(stat(backup, &info) == 0 && (info.st_mode & 0777) == 0600);
    read_text(backup, text, sizeof(text));
    assert(!strcmp(text, "old state\n"));
    read_text(path, text, sizeof(text));
    assert(strstr(text, "countdown ") == text);
    assert(access("/tmp/libreecho-timer-persistence-test.tmp", F_OK) < 0);
    assert(access("/tmp/libreecho-timer-persistence-test.bak.tmp", F_OK) < 0);

    unlink(path);
    unlink(backup);
    unlink(dirty_path);
    unlink(dirty_backup);
    puts("timer persistence: ok");
    return 0;
}
