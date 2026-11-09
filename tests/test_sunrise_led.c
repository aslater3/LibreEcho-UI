/*
 * Sunrise wake light (UI #447), exercised through the real ledd handler.
 *
 * timerd asks ledd for a warm fade-up over the owner's lead time before an
 * alarm, and ends it when the alarm is dismissed or snoozed. The fade is a
 * base-layer owner: voice, playback and the visualiser keep priority over it,
 * and a safety cap ends it if timerd never says stop.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define main ledd_program_main
#include "../src/adapter/ledd.c"
#undef main

#include <stdio.h>
#include <sys/socket.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "sunrise LED regression: %s\n", message);
        _exit(1);
    }
}

static void fire(struct daemon_context *ctx, const char *line)
{
    int pair[2];

    require_condition(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                      "socketpair failed");
    (void)handle_request(ctx, pair[1], line);
    close(pair[0]);
    close(pair[1]);
}

static int status_has(const struct daemon_context *ctx, const char *text)
{
    static char buffer[4096];

    require_condition(status_json(ctx, buffer, sizeof(buffer)) >= 0,
                      "status did not fit");
    return strstr(buffer, text) != NULL;
}

static void reset(struct daemon_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    default_state(&ctx->state);
    ctx->state.current.brightness = 0;
}

static const char START_120[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"sunrise\",\"args\":"
    "{\"action\":\"start\",\"seconds\":120}}";
static const char STOP[] =
    "{\"v\":1,\"id\":2,\"cmd\":\"sunrise\",\"args\":{\"action\":\"stop\"}}";
static const char START_TOO_SHORT[] =
    "{\"v\":1,\"id\":3,\"cmd\":\"sunrise\",\"args\":"
    "{\"action\":\"start\",\"seconds\":30}}";

int main(void)
{
    struct daemon_context ctx;
    double now;

    /* Off by default: nothing is active until timerd asks. */
    reset(&ctx);
    require_condition(!ctx.sunrise_active, "sunrise must be off by default");
    require_condition(status_has(&ctx, "\"sunrise\":{\"active\":false,"
                                       "\"progress_percent\":0}"),
                      "status should report sunrise off");

    /* A start brings the fade up, and status reports it while it runs. */
    fire(&ctx, START_120);
    require_condition(ctx.sunrise_active, "start did not activate sunrise");
    require_condition(ctx.sunrise_seconds == 120.0,
                      "start did not keep the requested fade length");
    require_condition(status_has(&ctx, "\"sunrise\":{\"active\":true"),
                      "status should report sunrise active during the fade");

    /* An out-of-range fade is refused and leaves the state alone. */
    fire(&ctx, START_TOO_SHORT);
    require_condition(ctx.sunrise_seconds == 120.0,
                      "a too-short fade replaced the running one");

    /* Ramp: progress rises from the start and clamps at the end. */
    now = ctx.sunrise_started + 60.0;
    require_condition(sunrise_progress(&ctx, now) > 0.49 &&
                      sunrise_progress(&ctx, now) < 0.51,
                      "fade should be half way at half the lead");
    require_condition(sunrise_progress(&ctx, ctx.sunrise_started + 500.0) == 1.0,
                      "fade progress should clamp at the warm white end");

    /* Voice and other pattern owners outrank the fade: it is hidden while a
     * pattern runs, so the frame loop does not spin for it. */
    ctx.pattern_active = 1;
    require_condition(!base_layer_visible(&ctx),
                      "a running pattern should hide the base layer");
    require_condition(sunrise_timeout(&ctx, now) != FRAME_MS,
                      "a hidden fade should not ask for frame-rate wakeups");
    ctx.pattern_active = 0;

    /* Once the ramp has run, it holds and stops asking for frames. */
    ctx.sunrise_started = monotonic_seconds() - 130.0;
    sunrise_tick(&ctx, monotonic_seconds());
    require_condition(ctx.sunrise_active && ctx.sunrise_settled,
                      "a finished ramp should hold warm white");
    require_condition(sunrise_timeout(&ctx, monotonic_seconds()) > FRAME_MS,
                      "a held fade should not ask for frame-rate wakeups");

    /* A stop ends it, and the ring is released to the base layer. */
    fire(&ctx, STOP);
    require_condition(!ctx.sunrise_active && !ctx.sunrise_settled,
                      "stop did not end the fade");
    require_condition(status_has(&ctx, "\"sunrise\":{\"active\":false"),
                      "status should report the fade ended");

    /* Safety cap: a fade timerd forgot to stop still ends on its own. */
    fire(&ctx, START_120);
    ctx.sunrise_started = monotonic_seconds() -
                          (120.0 + SUNRISE_HOLD_MAX_SECONDS + 5.0);
    sunrise_tick(&ctx, monotonic_seconds());
    require_condition(!ctx.sunrise_active,
                      "the safety cap did not end an orphaned fade");

    /* A start is activity, so it wakes an auto-off base that was dark. */
    reset(&ctx);
    ctx.idle_base_asleep = 1;
    fire(&ctx, START_120);
    require_condition(ctx.sunrise_active && !ctx.idle_base_asleep,
                      "a wake fade must wake an auto-off base");
    fire(&ctx, STOP);

    return 0;
}
