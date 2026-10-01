/*
 * Focused ledd-level behavioural tests for the LED base layers, sleep light
 * and the v2 music transport (issues #110, #101, #64, #65).
 *
 * This compiles the real daemon source (ledd.c) with its real request handler,
 * so the tests exercise the shipped command path and the actual output
 * pipeline, not a copy.  Hardware writes are a no-op because the context is
 * left on the stub backend; the daemon's own rendered frame is inspected.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 \
 *       -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/adapter \
 *       -o build/test-led-daemon-core \
 *       tests/test_led_daemon_core.c \
 *       src/adapter/led_output.c src/adapter/music_visualizer_protocol.c \
 *       src/adapter/led_music_director.c src/adapter/led_music_render.c \
 *       src/adapter/adapter_server.c src/log.c
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define main ledd_program_main
/* Redirect the persisted state at a scratch path this harness owns, so the
   success path can actually write and a persist failure can be injected. */
#define LED_STATE_DIR "/tmp/libreecho-ledd-state-test"
#define STATE_PATH LED_STATE_DIR "/led-state.json"
#define STATE_TMP_PATH LED_STATE_DIR "/led-state.json.tmp"
#include "../src/adapter/ledd.c"
#undef main

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "LED daemon core: %s\n", message);
        _exit(1);
    }
}

static char last_response[1024];

static void fire(struct daemon_context *ctx, const char *line)
{
    int pair[2];
    ssize_t n;

    require_condition(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                      "socketpair failed");
    (void)handle_request(ctx, pair[1], line);
    n = recv(pair[0], last_response, sizeof(last_response) - 1U, MSG_DONTWAIT);
    if (n < 0)
        n = 0;
    last_response[n] = '\0';
    close(pair[0]);
    close(pair[1]);
}

static int response_ok(void)
{
    return strstr(last_response, "\"ok\":true") != NULL;
}

static unsigned int max_channel(const struct daemon_context *ctx)
{
    unsigned int maximum = 0U;
    size_t i;

    for (i = 0; i < RING_PIXELS; i++) {
        if (ctx->rendered_pixels[i].r > maximum) maximum = ctx->rendered_pixels[i].r;
        if (ctx->rendered_pixels[i].g > maximum) maximum = ctx->rendered_pixels[i].g;
        if (ctx->rendered_pixels[i].b > maximum) maximum = ctx->rendered_pixels[i].b;
    }
    return maximum;
}

static int all_dark(const struct daemon_context *ctx)
{
    size_t i;

    for (i = 0; i < RING_PIXELS; i++) {
        if (ctx->rendered_pixels[i].r || ctx->rendered_pixels[i].g ||
            ctx->rendered_pixels[i].b)
            return 0;
    }
    return 1;
}

/* A controlled reset: default state, stub hardware, visualizer switchable. */
static void reset(struct daemon_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    default_state(&ctx->state);
    ctx->hw.kind = HW_STUB;
}

/* Read the persisted state file and test for a substring. */
static int state_file_contains(const char *needle)
{
    char buf[8192];
    ssize_t n;
    int fd = open(STATE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Encode the frozen v2 frame with a legacy 24-hex-character levels field. */
static void build_v2(char *out, size_t out_size, unsigned int seq,
                     unsigned int timestamp_ms, unsigned int session)
{
    snprintf(out, out_size,
        "{\"v\":1,\"id\":1,\"cmd\":\"visualizer\",\"args\":{"
        "\"action\":\"frame\",\"owner\":\"music\","
        "\"levels\":\"000000000000000000000000\",\"brightness\":100,"
        "\"feature_version\":2,\"seq\":%u,\"timestamp_ms\":%u,\"session\":%u,"
        "\"energy\":10,\"warmth\":20,\"brightness_axis\":30,\"density\":40,"
        "\"transientness\":50,\"groove\":60,\"build\":70,\"spaciousness\":80,"
        "\"loudness_fast\":90,\"loudness_slow\":100,\"onset_low\":110,"
        "\"onset_mid\":120,\"onset_high\":130,\"beat_strength\":140,"
        "\"beat_confidence\":150,\"novelty\":160,\"event_strength\":170,"
        "\"beat_phase\":32000,\"bpm_x100\":12800,\"events\":1}}",
        seq, timestamp_ms, session);
}

int main(void)
{
    struct daemon_context ctx;
    char frame[1024];

    (void)rmdir(STATE_TMP_PATH);
    require_condition(mkdir(LED_STATE_DIR, 0700) == 0 || errno == EEXIST,
                      "the ledd state directory must be creatable");
    double t0;
    unsigned int peak_a, peak_b;

    /* ---- 1. Default idle is dark; an event lights and returns to base. --- */
    reset(&ctx);
    require_condition(ctx.state.idle_mode == IDLE_MODE_OFF,
                      "idle must default to off");
    apply_base_state(&ctx, 0.0);
    require_condition(all_dark(&ctx), "default idle frame must be dark");

    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"solid\",\"r\":255,\"g\":0,\"b\":0,"
               "\"brightness\":100,\"repeats\":0,\"owner\":\"evt\"}}");
    require_condition(response_ok() && ctx.pattern_active,
                      "an event pattern must take the ring");
    require_condition(max_channel(&ctx) > 0U, "event frame must be lit");
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"stop\",\"owner\":\"evt\"}}");
    require_condition(response_ok() && !ctx.pattern_active,
                      "stopping the event must release the ring");
    require_condition(all_dark(&ctx),
                      "the ring must return to the dark base after an event");

    /* ---- 2. Idle indicator is one dim pixel, never above its ceiling. --- */
    reset(&ctx);
    ctx.state.idle_mode = IDLE_MODE_INDICATOR;
    apply_base_state(&ctx, 0.0);
    require_condition(ctx.rendered_pixels[0].r >= 1U,
                      "the indicator pixel must be visible");
    require_condition(ctx.rendered_pixels[IDLE_INDICATOR_PIXEL].r <=
                          (255U * IDLE_INDICATOR_BRIGHTNESS + 50U) / 100U,
                      "indicator must not exceed the requested dim ceiling");
    for (size_t i = 1; i < RING_PIXELS; i++)
        require_condition(ctx.rendered_pixels[i].r == 0U,
                          "only the single front pixel may be lit at idle");

    /* ---- 3. Night mode caps the actual output, end to end. -------------- */
    reset(&ctx);
    {
        time_t wall = time(NULL);
        struct tm local;
        int minute;
        require_condition(localtime_r(&wall, &local) != NULL,
                          "local time unavailable");
        minute = local.tm_hour * 60 + local.tm_min;
        ctx.state.night_enabled = 1;
        ctx.state.night_start_minute = (minute + 1439) % 1440;
        ctx.state.night_end_minute = (minute + 1) % 1440;
        ctx.state.profiles[PROFILE_NIGHT].brightness = 12;
        ctx.state.idle_mode = IDLE_MODE_ALWAYS;
        ctx.state.current = (struct colour){255U, 255U, 255U, 100};
    }
    apply_base_state(&ctx, 0.0);
    require_condition(max_channel(&ctx) <= 31U,
                      "night cap must bound the rendered frame (12% of 255)");
    require_condition(max_channel(&ctx) >= 1U,
                      "a capped night frame must not be blanked");

    /* ---- 4. Sleep light solid is bounded and server-validated. ---------- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"solid\",\"brightness\":20,\"timer_minutes\":0}}");
    require_condition(response_ok() && ctx.sleep_active,
                      "solid sleep light must activate");
    require_condition(ctx.state.sleep_mode == SLEEP_MODE_SOLID,
                      "solid mode must be recorded");
    apply_base_layer(&ctx, ctx.sleep_started + 1.0);
    require_condition(max_channel(&ctx) <= 51U,
                      "sleep light must respect the 20% brightness cap");
    require_condition(max_channel(&ctx) >= 1U, "sleep light must be visible");

    /* ---- 5. Sleep pulse: absolute phase survives pre-emption. ----------- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"pulse\",\"brightness\":20,\"period_ms\":3000}}");
    require_condition(response_ok() && ctx.sleep_active,
                      "pulse sleep light must activate");
    t0 = ctx.sleep_started;
    apply_sleep_light(&ctx, t0);
    peak_a = max_channel(&ctx);
    apply_sleep_light(&ctx, t0 + 0.75);   /* quarter period: wave crest */
    peak_b = max_channel(&ctx);
    require_condition(peak_b > peak_a, "pulse must rise toward its crest");

    /* A higher-priority owner pre-empts, then releases. */
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"solid\",\"r\":0,\"g\":255,\"b\":0,"
               "\"brightness\":100,\"repeats\":0,\"owner\":\"prio\"}}");
    require_condition(ctx.sleep_active && ctx.state.sleep_mode == SLEEP_MODE_PULSE,
                      "pre-emption must not cancel the sleep settings");
    require_condition(ctx.sleep_started == t0,
                      "pre-emption must not restart the pulse phase");
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"stop\",\"owner\":\"prio\"}}");
    apply_base_layer(&ctx, t0 + 0.75);
    require_condition(max_channel(&ctx) == peak_b,
                      "resume must continue the wave, not replay it");

    /* ---- 6. Sleep timer expiry selects off and returns to base. --------- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"solid\",\"brightness\":20,\"timer_minutes\":1}}");
    require_condition(response_ok() && ctx.sleep_active && ctx.sleep_expires > 0.0,
                      "timed sleep light must arm an expiry");
    sleep_tick(&ctx, ctx.sleep_expires - 0.5);
    require_condition(ctx.sleep_active, "sleep must survive before expiry");
    sleep_tick(&ctx, ctx.sleep_expires + 0.01);
    require_condition(!ctx.sleep_active, "timer expiry must end the sleep light");
    require_condition(ctx.state.sleep_mode == SLEEP_MODE_OFF,
                      "expiry must select off");
    require_condition(all_dark(&ctx), "expiry must return to the dark base");

    /* ---- 6b. A timed sleep light that expires while persistence fails keeps
       the OFF state owed on disk and retries it (Codex review on d650caf): the
       ring went dark in memory but the stale file still said the light was
       active, so a restart turned it back on. ---------------------------- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"solid\",\"brightness\":20,\"timer_minutes\":1,"
               "\"restore_on_boot\":true}}");
    require_condition(response_ok() && ctx.sleep_active &&
                          ctx.sleep_expires > 0.0,
                      "a timed sleep light must arm its expiry");
    require_condition(state_file_contains("\"sleep\":{\"mode\":1"),
                      "the active sleep light must persist before expiry");
    {
        /* A directory where the atomic temp file belongs makes persist_state()
           fail for real, exactly as section 8 does for the handler path. */
        require_condition(mkdir(STATE_TMP_PATH, 0700) == 0,
                          "blocking the sleep-state temp path must succeed");
        sleep_tick(&ctx, ctx.sleep_expires + 0.01);
        require_condition(!ctx.sleep_active,
                          "a persist failure must not keep the light on");
        require_condition(ctx.state.sleep_mode == SLEEP_MODE_OFF,
                          "expiry must select off in memory");
        require_condition(ctx.sleep_persist_pending == 1,
                          "the failed expiry write must stay owed to disk");
        require_condition(state_file_contains("\"sleep\":{\"mode\":1"),
                          "the stale on-disk state still restores the light");
        require_condition(rmdir(STATE_TMP_PATH) == 0,
                          "unblocking the sleep-state temp path must succeed");
        /* The next tick retries and lands the OFF state. */
        sleep_tick(&ctx, ctx.sleep_persist_next + 0.5);
        require_condition(ctx.sleep_persist_pending == 0,
                          "a successful retry must clear the owed write");
        require_condition(state_file_contains("\"sleep\":{\"mode\":0"),
                          "the OFF state must be persisted after the retry");
    }

    /* ---- 7. Malformed sleep bounds are rejected, never clamped. --------- */
    reset(&ctx);
    {
        unsigned int saved = ctx.state.sleep_brightness;
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"brightness\":21}}");
        require_condition(!response_ok(), "brightness 21 must be rejected");
        require_condition(ctx.state.sleep_brightness == saved,
                          "a rejected brightness must not mutate state");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"period_ms\":2000}}");
        require_condition(!response_ok(), "period below 3000 ms must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"period_ms\":15001}}");
        require_condition(!response_ok(), "period above 15000 ms must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"timer_minutes\":721}}");
        require_condition(!response_ok(), "timer above 720 min must be rejected");
        /* Present but malformed values are rejected, not silently ignored. */
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"brightness\":-1}}");
        require_condition(!response_ok(), "a negative brightness must be rejected");
        require_condition(ctx.state.sleep_brightness == saved,
                          "a malformed brightness must not mutate state");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"period_ms\":\"abc\"}}");
        require_condition(!response_ok(), "a non-numeric period must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"timer_minutes\":-5}}");
        require_condition(!response_ok(), "a negative timer must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"solid\",\"restore_on_boot\":\"yes\"}}");
        require_condition(!response_ok(),
                          "a non-boolean restore_on_boot must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"strobe\"}}");
        require_condition(!response_ok(), "an unknown mode must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":{}}");
        require_condition(!response_ok(), "a missing mode must be rejected");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"pulse\",\"brightness\":20,\"period_ms\":3000,"
                   "\"timer_minutes\":720}}");
        require_condition(response_ok(), "in-range bounds must be accepted");
    }

    /* ---- 8. A persist failure is reported and the in-memory state stays put
       (Codex review on bcfef3e): the handler used to ignore persist_state()
       and reply success, so a change such as restore_on_boot could be lost on
       the next boot while the owner had been told it was saved. -------------- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"solid\",\"brightness\":7,\"timer_minutes\":0}}");
    require_condition(response_ok(),
                      "a writable state path must accept the sleep change");
    {
        struct led_state before = ctx.state;
        int before_active = ctx.sleep_active;

        /* A directory where the atomic temp file belongs makes persist_state()
           fail for real -- no simulated return code, and it holds even as root. */
        require_condition(mkdir(STATE_TMP_PATH, 0700) == 0,
                          "blocking the state temp path must succeed");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"pulse\",\"brightness\":20,\"period_ms\":9000,"
                   "\"timer_minutes\":45,\"restore_on_boot\":true}}");
        require_condition(!response_ok(),
                          "a persist failure must be reported, not success");
        require_condition(
            ctx.state.sleep_mode == before.sleep_mode &&
            ctx.state.sleep_brightness == before.sleep_brightness &&
            ctx.state.sleep_period_ms == before.sleep_period_ms &&
            ctx.state.sleep_timer_minutes == before.sleep_timer_minutes &&
            ctx.state.sleep_restore_on_boot == before.sleep_restore_on_boot,
            "a persist failure must revert the in-memory sleep settings");
        require_condition(ctx.sleep_active == before_active,
                          "a persist failure must revert the in-memory activation");
        require_condition(rmdir(STATE_TMP_PATH) == 0,
                          "unblocking the state temp path must succeed");
    }

    /* ---- 8b. The idle-mode change reports a persist failure and reverts
       (Codex review on a3be414): like the sleep-light handler before it, the
       idle-mode handler ignored persist_state() and replied success, so the
       mode silently reverted to the on-disk value on the next boot while the
       owner had been told it was saved. ------------------------------------ */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"set_idle_mode\",\"args\":"
               "{\"mode\":\"indicator\"}}");
    require_condition(response_ok() && ctx.state.idle_mode == IDLE_MODE_INDICATOR,
                      "a writable state path must accept the idle-mode change");
    require_condition(state_file_contains("\"idle\":{\"mode\":1"),
                      "the accepted idle mode must be persisted");
    {
        int before_idle_mode = ctx.state.idle_mode;

        /* A directory where the atomic temp file belongs makes persist_state()
           fail for real -- no simulated return code, and it holds even as root. */
        require_condition(mkdir(STATE_TMP_PATH, 0700) == 0,
                          "blocking the state temp path must succeed");
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"set_idle_mode\",\"args\":"
                   "{\"mode\":\"always\"}}");
        require_condition(!response_ok(),
                          "an idle-mode persist failure must be reported, not success");
        require_condition(ctx.state.idle_mode == before_idle_mode,
                          "an idle-mode persist failure must revert the in-memory mode");
        require_condition(rmdir(STATE_TMP_PATH) == 0,
                          "unblocking the state temp path must succeed");
        /* With the path writable again, the mode change succeeds and persists. */
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"set_idle_mode\",\"args\":"
                   "{\"mode\":\"always\"}}");
        require_condition(response_ok() && ctx.state.idle_mode == IDLE_MODE_ALWAYS,
                          "the idle-mode change must work again once writable");
        require_condition(state_file_contains("\"idle\":{\"mode\":2"),
                          "the recovered idle mode must be persisted");
    }

    /* ---- 9. Real v2 packet path: accept, stale rejection, legacy parity. */
    reset(&ctx);
    ctx.visualizer_enabled = 1;
    build_v2(frame, sizeof(frame), 1U, 1000U, 42U);
    fire(&ctx, frame);
    require_condition(response_ok(), "a valid v2 frame must be accepted");
    require_condition(strstr(last_response, "\"accepted\":true") != NULL,
                      "an accepted v2 frame must report acceptance");
    require_condition(ctx.music_active && ctx.visualizer_active,
                      "an accepted v2 frame must start the music scene");
    require_condition(ctx.music_session == 42U && ctx.music_features.seq == 1U,
                      "session and sequence must be tracked");

    build_v2(frame, sizeof(frame), 1U, 1000U, 42U);   /* duplicate */
    fire(&ctx, frame);
    require_condition(strstr(last_response, "\"accepted\":false") != NULL,
                      "a duplicate frame must be rejected");
    require_condition(ctx.music_features.seq == 1U,
                      "a stale frame must not mutate accepted scene state");
    require_condition(ctx.music_active, "a stale frame must not stop the scene");

    build_v2(frame, sizeof(frame), 5U, 1400U, 42U);   /* in order */
    fire(&ctx, frame);
    require_condition(ctx.music_features.seq == 5U,
                      "a later sequence must advance the stream");

    build_v2(frame, sizeof(frame), 1U, 10U, 43U);     /* new session */
    fire(&ctx, frame);
    require_condition(ctx.music_session == 43U,
                      "a new session must take over");

    /* Legacy v1 frame (no feature_version) still works and is not v2. */
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"visualizer\",\"args\":"
               "{\"action\":\"frame\",\"owner\":\"music\","
               "\"levels\":\"ff0000ff0000ff0000ff0000\",\"brightness\":50}}");
    require_condition(response_ok() && ctx.visualizer_active,
                      "a legacy spectrum frame must still be accepted");
    require_condition(!ctx.music_active,
                      "a legacy frame must not be treated as v2 music");

    /* A malformed v2 frame is rejected outright. */
    reset(&ctx);
    ctx.visualizer_enabled = 1;
    {
        const char bad[] =
            "{\"v\":1,\"id\":1,\"cmd\":\"visualizer\",\"args\":{"
            "\"action\":\"frame\",\"owner\":\"music\","
            "\"levels\":\"000000000000000000000000\",\"brightness\":100,"
            "\"feature_version\":2,\"seq\":1,\"timestamp_ms\":1,\"session\":7,"
            "\"energy\":10}}";
        fire(&ctx, bad);
        require_condition(!response_ok(),
                          "a malformed v2 frame must be a hard error");
    }

    puts("LED daemon core: ok");
    return 0;
}
