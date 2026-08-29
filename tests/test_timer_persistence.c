#define _POSIX_C_SOURCE 200809L
#define LE_TIMERD_TESTING
#define main timerd_program_main
#include "../src/adapter/timerd.c"
#undef main

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define BOOT_EPOCH 10LL
#define SYNCED_EPOCH 1767225600LL

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

static void assert_audio_refresh_and_snapshot(const char *path,
                                              const char *backup)
{
    struct context context;
    char text[256];

    memset(&context, 0, sizeof(context));
    context.state_path = path;
    context.state_loaded = 1;
    le_timer_set_init(&context.timers);
    unlink(path);
    unlink(backup);
    unlink("/tmp/libreecho-timer-persistence-test.tmp");
    unlink("/tmp/libreecho-timer-persistence-test.bak.tmp");
    assert(le_timer_add_countdown(&context.timers, 1, "ring", 0, NULL) ==
           LE_TIMER_OK);
    assert(le_timer_add_countdown(&context.timers, 10, "pending", 1000,
                                  NULL) == LE_TIMER_OK);
    le_timerd_test_monotonic_enabled = 1;
    le_timerd_test_monotonic_ms = 1000;
    assert(le_timer_step(&context.timers, 1000, SYNCED_EPOCH, NULL, 0) == 1);
    context.next_ring_ms = 0;
    le_timerd_test_block_audio_ms = 2000;
    ring_tick(&context, 1000);
    assert(le_timerd_test_monotonic_ms == 3000);
    assert(context.next_ring_ms == 5000);

    context.dirty = 1;
    save_if_dirty(&context, monotonic_ms(), SYNCED_EPOCH);
    assert(context.dirty == 0);
    read_text(path, text, sizeof(text));
    /* A stale pre-cue snapshot would serialize 1767225610 instead. */
    assert(strstr(text, "v2 countdown 1767225608 @hex:70656e64696e67") != NULL);
    le_timerd_test_monotonic_enabled = 0;
}

static void assert_post_commit_retry(const char *path, const char *backup)
{
    struct context context;
    char text[256];

    memset(&context, 0, sizeof(context));
    context.state_path = path;
    context.state_loaded = 1;
    le_timer_set_init(&context.timers);
    unlink(path);
    unlink(backup);
    write_text(path, "old schedule\n");
    assert(le_timer_add_countdown(&context.timers, 600, "committed", 0,
                                  NULL) == LE_TIMER_OK);
    le_timerd_test_fail_parent_fsync = 1;
    assert(state_save(&context) == STATE_SAVE_COMMITTED_DURABILITY_FAILED);
    assert(context.state_commit_pending == 1);
    read_text(path, text, sizeof(text));
    assert(strstr(text, "@hex:636f6d6d6974746564") != NULL);
    read_text(backup, text, sizeof(text));
    assert(!strcmp(text, "old schedule\n"));

    /* The committed state remains dirty until durability succeeds, but retry
       must not rotate the new file over the old backup. */
    context.dirty = 1;
    save_if_dirty(&context, 1000, SYNCED_EPOCH);
    assert(context.dirty == 0);
    assert(context.state_commit_pending == 0);
    read_text(backup, text, sizeof(text));
    assert(!strcmp(text, "old schedule\n"));
    read_text(path, text, sizeof(text));
    assert(strstr(text, "@hex:636f6d6d6974746564") != NULL);
}

int main(void)
{
    const char *path = "/tmp/libreecho-timer-persistence-test";
    char backup[128];
    char text[256];
    struct context context;
    struct stat info;
    unsigned int fired[LE_TIMER_MAX];

    snprintf(backup, sizeof(backup), "%s.bak", path);
    unlink(path);
    unlink(backup);
    unlink("/tmp/libreecho-timer-persistence-test.tmp");
    unlink("/tmp/libreecho-timer-persistence-test.bak.tmp");

    memset(&context, 0, sizeof(context));
    context.state_path = path;
    le_timer_set_init(&context.timers);
    write_text(path, "alarm 1767225630 wake\ncountdown 1767225630 pasta\n");

    /* An invalid boot clock must leave the file and records untouched. */
    assert(state_load_at(&context, 1000, BOOT_EPOCH) == 0);
    assert(le_timer_active_count(&context.timers) == 0);
    read_text(path, text, sizeof(text));
    assert(strstr(text, "alarm 1767225630 wake") != NULL);

    /* Deferred restoration must be retried before the normal 60-second poll
       cap, otherwise NTP can leave the persisted schedule invisible. */
    assert(timer_poll_timeout(&context, 1000, BOOT_EPOCH) == RESTORE_RETRY_MS);
    context.state_loaded = 1;
    assert(timer_poll_timeout(&context, 1000, BOOT_EPOCH) == POLL_CAP_MS);
    context.state_loaded = 0;

    /* A change made before NTP must remain dirty until a valid save. */
    assert(le_timer_add_countdown(&context.timers, 600, "new", 1000, NULL) ==
           LE_TIMER_OK);
    context.dirty = 1;
    save_if_dirty(&context, 1000, BOOT_EPOCH);
    assert(context.dirty == 1);
    read_text(path, text, sizeof(text));
    assert(strstr(text, "alarm 1767225630 wake") != NULL);

    /* A wall-clock transition must restore deferred state before allowing the
       dirty pre-sync change to replace the persisted schedule. The save path
       owns this ordering so every save caller gets the same protection. */
    assert(context.state_loaded == 0);
    save_if_dirty(&context, 1500, SYNCED_EPOCH);
    assert(context.state_loaded == 1);
    assert(context.dirty == 0);
    assert(le_timer_active_count(&context.timers) == 3);
    read_text(path, text, sizeof(text));
    assert(strstr(text, "v2 alarm 1767225630 @hex:77616b65") != NULL);
    assert(strstr(text, "v2 countdown 1767225630 @hex:7061737461") != NULL);
    assert(strstr(text, "v2 countdown 1767226199 @hex:6e6577") != NULL);

    /* Once NTP makes the clock valid, both records restore deterministically. */
    le_timer_set_init(&context.timers);
    write_text(path, "alarm 1767225630 wake\ncountdown 1767225630 pasta\n");
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
    assert(state_save(&context) == 1);
    assert(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(stat(backup, &info) == 0 && (info.st_mode & 0777) == 0600);
    read_text(backup, text, sizeof(text));
    assert(!strcmp(text, "old state\n"));
    read_text(path, text, sizeof(text));
    assert(strstr(text, "v2 countdown ") == text);
    assert(access("/tmp/libreecho-timer-persistence-test.tmp", F_OK) < 0);
    assert(access("/tmp/libreecho-timer-persistence-test.bak.tmp", F_OK) < 0);

    /* New records encode labels so leading whitespace survives the restart
       boundary that the legacy whitespace-delimited format could not preserve. */
    {
        struct context whitespace;
        struct le_timer *timer;

        memset(&whitespace, 0, sizeof(whitespace));
        whitespace.state_path = path;
        whitespace.state_loaded = 1;
        le_timer_set_init(&whitespace.timers);
        assert(le_timer_add_countdown(&whitespace.timers, 600, "  tea", 0,
                                      NULL) == LE_TIMER_OK);
        assert(state_save_at(&whitespace, 0, SYNCED_EPOCH) == STATE_SAVE_OK);
        read_text(path, text, sizeof(text));
        assert(strstr(text, "v2 countdown 1767226200 @hex:2020746561") != NULL);

        le_timer_set_init(&whitespace.timers);
        assert(state_load_at(&whitespace, 0, SYNCED_EPOCH) == 1);
        timer = le_timer_find(&whitespace.timers, 1);
        assert(timer != NULL && !strcmp(timer->label, "  tea"));
        unlink(path);
        unlink(backup);
    }

    /* Legacy records have no format marker, so an @hex: prefix is literal
       user data rather than the v2 encoding. */
    {
        struct context legacy;
        struct le_timer *timer;

        memset(&legacy, 0, sizeof(legacy));
        legacy.state_path = path;
        le_timer_set_init(&legacy.timers);
        write_text(path, "countdown 1767226200 @hex:tea\n");
        assert(state_load_at(&legacy, 0, SYNCED_EPOCH) == 1);
        timer = le_timer_find(&legacy.timers, 1);
        assert(timer != NULL && !strcmp(timer->label, "@hex:tea"));
        unlink(path);
        unlink(backup);
    }

    unlink(path);
    unlink(backup);

    /* A present malformed id is a protocol error, not dismiss-all. */
    {
        char response[256];
        struct context protocol;

        memset(&protocol, 0, sizeof(protocol));
        le_timer_set_init(&protocol.timers);
        assert(le_timer_add_countdown(&protocol.timers, 1, "ring", 0, NULL) ==
               LE_TIMER_OK);
        assert(le_timer_step(&protocol.timers, 1000, SYNCED_EPOCH, NULL, 0) ==
               1);
        assert(dispatch(&protocol, "dismiss", "{\"id\":\"bad\"}", 7,
                        response, sizeof(response)) > 0);
        assert(strstr(response, "\"ok\":false") != NULL);
        assert(le_timer_ringing_count(&protocol.timers) == 1);
        assert(dispatch(&protocol, "dismiss", "{\"id\":0}", 7, response,
                        sizeof(response)) > 0);
        assert(strstr(response, "\"ok\":false") != NULL);
        assert(le_timer_ringing_count(&protocol.timers) == 1);
        assert(dispatch(&protocol, "dismiss", "{\"id\":1.5}", 7,
                        response, sizeof(response)) > 0);
        assert(strstr(response, "\"ok\":false") != NULL);
        assert(le_timer_ringing_count(&protocol.timers) == 1);
        assert(dispatch(&protocol, "dismiss", "{}", 7, response,
                        sizeof(response)) > 0);
        assert(strstr(response, "\"ok\":true") != NULL);
        assert(le_timer_ringing_count(&protocol.timers) == 0);
    }

    assert_audio_refresh_and_snapshot(path, backup);
    assert_post_commit_retry(path, backup);

    puts("timer persistence: ok");
    return 0;
}
