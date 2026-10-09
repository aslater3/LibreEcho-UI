/* Private, hardware-free daemon fixture: remote expiry is owned by HA. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/adapter/adapter.h"
static int fixture_open(const char *path, int flags, ...);
#define open fixture_open
#define LE_TIMERD_TESTING
#define main timerd_program_main
#include "../src/adapter/timerd.c"
#undef main
#undef open

static const char *private_root;
static int open_calls, cues, audio_result, audio_missing, timeout_calls;
static int fixture_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    assert(private_root && !strncmp(path, private_root, strlen(private_root)));
    ++open_calls;
    return open(path, flags, mode);
}
struct le_adapter *le_adapter_connect(const char *path, int timeout_ms)
{
    assert(!strcmp(path, "fixture-audio"));
    assert(timeout_ms == AUDIO_TIMEOUT_MS);
    return audio_missing ? NULL : (struct le_adapter *)&cues;
}
void le_adapter_set_io_timeout(struct le_adapter *a, int ms)
{ assert(a != NULL && ms == AUDIO_TIMEOUT_MS); ++timeout_calls; }
int le_adapter_call(struct le_adapter *a, const char *cmd, const char *args,
                    char *out, size_t size)
{
    assert(a != NULL && !strcmp(cmd, "cue"));
    assert(strstr(args, "\"ms\":250")); (void)out; (void)size;
    ++cues; return audio_result;
}
void le_adapter_close(struct le_adapter *a) { assert(a != NULL); }

static char response[LE_ADAPTER_MSG_MAX];
static void command(struct context *ctx, const char *cmd, const char *args, int ok)
{
    assert(dispatch(ctx, cmd, args, 7, response, sizeof(response)) > 0);
    if (!strstr(response, ok ? "\"ok\":true" : "\"ok\":false")) {
        fprintf(stderr, "%s %s -> %s", cmd, args ? args : "{}", response);
        assert(0 && "daemon command result");
    }
    assert(json_valid_object(response, strlen(response)));
}
static void event(struct context *ctx, int type, const char *id, int total,
                  int left, int active, int ok)
{
    char args[512];
    snprintf(args, sizeof(args),
             "{\"event_type\":%d,\"timer_id\":\"%s\",\"name\":\"tea\","
             "\"total_seconds\":%d,\"seconds_left\":%d,\"is_active\":%s}",
             type, id, total, left, active ? "true" : "false");
    command(ctx, "remote_event", args, ok);
}
static void init(struct context *ctx)
{
    memset(ctx, 0, sizeof(*ctx)); le_timer_set_init(&ctx->timers);
    ctx->audio_sock = "fixture-audio"; ctx->state_loaded = 1;
    le_timerd_test_monotonic_enabled = 1; le_timerd_test_monotonic_ms = 1000;
    cues = 0; audio_result = LE_ADAPTER_OK; audio_missing = 0;
}
int main(void)
{
    struct context ctx, restored;
    unsigned int local, alarm;
    char dir[512], state[560], text[4096], args[1024], id[80], name[80];
    const char *scratch = getenv("TMPDIR");
    FILE *file;
    size_t length;
    int i;
    assert(scratch && snprintf(dir, sizeof(dir), "%s/remote-timer-XXXXXX", scratch) < (int)sizeof(dir));
    assert(mkdtemp(dir)); private_root = dir;
    snprintf(state, sizeof(state), "%s/state", dir);
    init(&ctx); ctx.state_path = state;
    assert(le_timer_add_countdown(&ctx.timers, 30, "local", 1000, &local) == LE_TIMER_OK);
    assert(le_timer_add_alarm(&ctx.timers, 1767225660, "alarm", 1767225600, &alarm) == LE_TIMER_OK);
    event(&ctx, 0, "ha-a", 10, 0, 1, 1); /* Zero does not expire. */
    event(&ctx, 0, "ha-b", 60, 22, 0, 1); /* Paused does not expire. */
    assert(ctx.dirty == 0); /* HA state is not persisted. */
    assert(le_timer_step(&ctx.timers, 15000, 1767225600, NULL, 0) == 0);
    le_timerd_test_monotonic_ms = 15000; ring_tick(&ctx, 15000); assert(cues == 0);
    command(&ctx, "status", "{}", 1);
    assert(strstr(response, "\"kind\":\"remote\"") && strstr(response, "\"external_id\":\"ha-a\""));
    assert(strstr(response, "\"seconds_left\":22") && strstr(response, "\"state\":\"paused\""));
    event(&ctx, 1, "ha-a", 10, 2, 0, 1);
    event(&ctx, 1, "absent", 10, 2, 1, 0);
    event(&ctx, 3, "absent", 10, 0, 0, 0);
    event(&ctx, 3, "ha-a", 10, 0, 0, 1);
    ring_tick(&ctx, 15000); assert(cues == 1 && timeout_calls == 1);
    command(&ctx, "status", "{}", 1);
    assert(strstr(response, "\"remote_ringing\":1") && strstr(response, "\"ringing\":0"));
    assert(timer_poll_timeout(&ctx, 15000, 1767225600) <= LE_TIMER_RING_SECONDS * 1000LL);
    event(&ctx, 2, "ha-b", 60, 22, 0, 1);
    command(&ctx, "status", "{}", 1); assert(!strstr(response, "ha-b") && strstr(response, "ha-a"));
    command(&ctx, "dismiss", "{}", 1); assert(strstr(response, "\"dismissed\":1"));
    assert(le_timer_find(&ctx.timers, local) && le_timer_find(&ctx.timers, alarm));
    event(&ctx, 0, "ha-a", 10, 0, 1, 1); event(&ctx, 3, "ha-a", 10, 0, 0, 1);
    command(&ctx, "remote_clear", "{}", 1);
    command(&ctx, "status", "{}", 1); assert(!strstr(response, "ha-a") && strstr(response, "local"));
    event(&ctx, 0, "ha-b", 10, 0, 0, 1); event(&ctx, 3, "ha-b", 10, 0, 0, 1);
    command(&ctx, "dismiss_all", "{}", 1);
    command(&ctx, "status", "{}", 1); assert(!strstr(response, "ha-b"));
    event(&ctx, 0, "ha-a", 10, 0, 1, 1); event(&ctx, 3, "ha-a", 10, 0, 0, 1);
    audio_result = LE_ADAPTER_ERR_REJECTED; ctx.next_ring_ms = 0;
    ring_tick(&ctx, 15000); assert(cues == 2 && ctx.next_ring_ms == 17000);
    audio_missing = 1; ctx.next_ring_ms = 0; ring_tick(&ctx, 15000); assert(cues == 2);
    le_timerd_test_monotonic_ms = 16000;
    event(&ctx, 3, "ha-a", 10, 0, 0, 1); /* Duplicate finish cannot renew its ring. */
    event(&ctx, 1, "ha-a", 10, 9, 1, 1); /* Late update cannot silence it either. */
    command(&ctx, "status", "{}", 1); assert(strstr(response, "\"remote_ringing\":1"));
    le_timerd_test_monotonic_ms = 15000 + LE_TIMER_RING_SECONDS * 1000LL;
    ring_tick(&ctx, monotonic_ms());
    command(&ctx, "status", "{}", 1); assert(!strstr(response, "ha-a"));
    audio_missing = 0;
    /* Local and remote rings coexist; clearing/cancelling HA leaves local alone. */
    le_timerd_test_monotonic_ms = 31000;
    assert(le_timer_step(&ctx.timers, 31000, 1767225600, NULL, 0) == 1);
    event(&ctx, 0, "ha-a", 10, 1, 1, 1); event(&ctx, 3, "ha-a", 10, 0, 0, 1);
    event(&ctx, 2, "ha-a", 10, 0, 0, 1);
    assert(le_timer_ringing_count(&ctx.timers) == 1);
    event(&ctx, 0, "ha-a", 10, 1, 1, 1); event(&ctx, 3, "ha-a", 10, 0, 0, 1);
    command(&ctx, "dismiss", "{}", 1); assert(strstr(response, "\"dismissed\":2"));
    assert(le_timer_find(&ctx.timers, alarm));
    /* Reject malformed, duplicate, missing, negative, overflowing fields atomically. */
    event(&ctx, -1, "bad", 10, 0, 1, 0); event(&ctx, 4, "bad", 10, 0, 1, 0);
    event(&ctx, 0, "", 10, 0, 1, 0); event(&ctx, 0, "bad", -1, 0, 1, 0);
    event(&ctx, 0, "bad", 10, -1, 1, 0); event(&ctx, 0, "bad", 10, 11, 1, 0);
    command(&ctx, "remote_event", "{\"event_type\":0,\"timer_id\":-1}", 0);
    command(&ctx, "remote_event", "{\"event_type\":0,\"timer_id\":\"bad\",\"name\":\"x\",\"total_seconds\":4294967296,\"seconds_left\":0,\"is_active\":true}", 0);
    command(&ctx, "remote_event", "{\"event_type\":0,\"timer_id\":\"bad\",\"name\":\"x\",\"total_seconds\":10,\"seconds_left\":0,\"is_active\":1}", 0);
    command(&ctx, "remote_event", "{\"event_type\":0,\"event_type\":3,\"timer_id\":\"bad\",\"name\":\"x\",\"total_seconds\":10,\"seconds_left\":0,\"is_active\":true}", 0);
    command(&ctx, "remote_event", "{\"nested\":{\"event_type\":0},\"timer_id\":\"bad\",\"name\":\"x\",\"total_seconds\":10,\"seconds_left\":0,\"is_active\":true}", 0);
    memset(id, 'i', 64); id[64] = 0; event(&ctx, 0, id, 10, 0, 1, 0);
    memset(name, 'n', 49); name[49] = 0;
    snprintf(args, sizeof(args), "{\"event_type\":0,\"timer_id\":\"long-name\",\"name\":\"%s\",\"total_seconds\":10,\"seconds_left\":0,\"is_active\":true}", name);
    command(&ctx, "remote_event", args, 0);
    for (i = 0; i < 8; ++i) { snprintf(id, sizeof(id), "slot-%d", i); event(&ctx, 0, id, 10, 0, 0, 1); }
    event(&ctx, 0, "overflow", 10, 0, 0, 0);
    event(&ctx, 1, "slot-7", 10, 1, 0, 1); /* Full table still updates existing id. */
    {
        char small[128];
        assert(dispatch(&ctx, "status", "{}", 7, small, sizeof(small)) > 0);
        assert(strstr(small, "\"ok\":false") && json_valid_object(small, strlen(small)));
    }
    assert(state_save_at(&ctx, 31000, 1767225600) == STATE_SAVE_OK);
    file = fopen(state, "r"); assert(file); length = fread(text, 1, sizeof(text)-1, file); text[length] = 0; fclose(file);
    assert(strstr(text, "alarm") && !strstr(text, "slot-") && !strstr(text, "remote"));
    init(&restored); restored.state_path = state;
    assert(state_load_at(&restored, 31000, 1767225600));
    command(&restored, "status", "{}", 1); assert(strstr(response, "alarm") && strstr(response, "\"remote_timers\":[]"));
    /* Exact limits are accepted, not silently shortened. */
    command(&ctx, "remote_clear", "{}", 1);
    memset(id, 'i', 63); id[63] = 0;
    memset(name, 'n', 48); name[48] = 0;
    snprintf(args, sizeof(args), "{\"event_type\":0,\"timer_id\":\"%s\",\"name\":\"%s\",\"total_seconds\":4294967295,\"seconds_left\":0,\"is_active\":false}", id, name);
    command(&ctx, "remote_event", args, 1);
    command(&ctx, "status", "{}", 1);
    assert(strstr(response, id) && strstr(response, name) && strstr(response, "4294967295"));
    command(&ctx, "remote_clear", "{}", 1);
    /* Real AF_UNIX server parsing/dispatch/response on a private socketpair. */
    {
        int pair[2];
        const char request[] = "{\"v\":1,\"id\":91,\"cmd\":\"remote_event\",\"args\":{\"event_type\":0,\"timer_id\":\"wire-id\",\"name\":\"wire\",\"total_seconds\":1,\"seconds_left\":0,\"is_active\":false}}\n";
        ssize_t n;
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        ctx.clients[0] = pair[0];
        assert(write(pair[1], request, sizeof(request) - 1) == sizeof(request) - 1);
        serve_client(&ctx, 0);
        n = read(pair[1], response, sizeof(response) - 1); assert(n > 0); response[n] = 0;
        assert(strstr(response, "\"ok\":true") && strstr(response, "\"id\":91"));
        close_client(&ctx, 0); assert(ctx.clients[0] == -1); close(pair[1]);
        command(&ctx, "status", "{}", 1); assert(strstr(response, "wire-id"));
    }
    assert(open_calls > 0);
    assert(unlink(state) == 0 && rmdir(dir) == 0);
    puts("remote timers: HA-owned expiry, bounded IDs/table, local isolation, ephemeral state, ring cleanup: ok");
    return 0;
}
