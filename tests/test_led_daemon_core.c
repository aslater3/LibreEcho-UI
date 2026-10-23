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

static void write_state_file(const char *text)
{
    FILE *f = fopen(STATE_PATH, "w");
    require_condition(f != NULL, "the test state file must be writable");
    fputs(text, f);
    fclose(f);
}

/* Boot-time load of a given on-disk state, as main() does it. */
static void boot_with_state(struct led_state *state, const char *text)
{
    write_state_file(text);
    memset(state, 0, sizeof(*state));
    default_state(state);
    load_state(state);
    finish_idle_policy_upgrade(state);
}

/* The idle indicator: exactly the two front-centre pixels lit green, at equal
   level, nothing else.  Pixels 10 and 11 were identified on hardware as the
   front centre of the ring. */
static int is_indicator_pixel(size_t i)
{
    return i == 10U || i == 11U;
}

static int indicator_only(const struct daemon_context *ctx)
{
    size_t i;

    if (ctx->rendered_pixels[10].g != ctx->rendered_pixels[11].g)
        return 0;
    for (i = 0; i < RING_PIXELS; i++) {
        if (!is_indicator_pixel(i))
            continue;
        if (ctx->rendered_pixels[i].g == 0U ||
            ctx->rendered_pixels[i].r != 0U || ctx->rendered_pixels[i].b != 0U)
            return 0;
    }
    for (i = 0; i < RING_PIXELS; i++) {
        if (is_indicator_pixel(i))
            continue;
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

    /* ---- 1. Default idle is the dim green front indicator; an event lights
       the ring and returns to that base.  Off stays a dark base. ------------ */
    reset(&ctx);
    require_condition(ctx.state.idle_mode == IDLE_MODE_INDICATOR,
                      "idle must default to the single-pixel indicator");
    apply_base_state(&ctx, 0.0);
    require_condition(indicator_only(&ctx),
                      "default idle frame must be the green front indicator");

    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"solid\",\"r\":255,\"g\":0,\"b\":0,"
               "\"brightness\":100,\"repeats\":0,\"owner\":\"evt\"}}");
    require_condition(response_ok() && ctx.pattern_active,
                      "an event pattern must take the ring");
    require_condition(!indicator_only(&ctx), "event frame must replace the idle base");
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
               "{\"name\":\"stop\",\"owner\":\"evt\"}}");
    require_condition(response_ok() && !ctx.pattern_active,
                      "stopping the event must release the ring");
    require_condition(indicator_only(&ctx),
                      "the ring must return to the indicator base after an event");

    reset(&ctx);
    ctx.state.idle_mode = IDLE_MODE_OFF;
    apply_base_state(&ctx, 0.0);
    require_condition(all_dark(&ctx), "off idle frame must be dark");

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

    /* ---- 2. Idle indicator is the two front-centre pixels, green at 20%,
       never above the ceiling. -------------------------------------------- */
    reset(&ctx);
    ctx.state.idle_mode = IDLE_MODE_INDICATOR;
    apply_base_state(&ctx, 0.0);
    require_condition(IDLE_INDICATOR_BRIGHTNESS == 20U,
                      "the idle indicator must be 20% brightness");
    require_condition(ctx.rendered_pixels[10].g >= 1U &&
                          ctx.rendered_pixels[11].g >= 1U,
                      "both indicator pixels must be visible");
    require_condition(ctx.rendered_pixels[10].g <=
                          (255U * IDLE_INDICATOR_BRIGHTNESS + 50U) / 100U,
                      "indicator must not exceed the requested dim ceiling");
    require_condition(indicator_only(&ctx),
                      "only pixels 10 and 11 may be lit, pure green, at idle");

    /* ---- 2b. Upgrading to 0.14 resets every device to the green indicator
       once, then honours the owner's later choice. ------------------------- */
    {
        struct led_state s;
        static const char *const upgrades[] = {
            /* 0.13: no idle object; the whole ring was always lit. */
            "{\"current\":{\"r\":0,\"g\":96,\"b\":255,\"brightness\":60}}",
            /* 0.14 pre-release with the dark default saved. */
            "{\"current\":{\"r\":0,\"g\":96,\"b\":255,\"brightness\":60},"
            "\"idle\":{\"mode\":0}}",
            /* 0.14 pre-release with always-on saved. */
            "{\"idle\":{\"mode\":2}}"
        };
        size_t k;

        for (k = 0; k < sizeof(upgrades) / sizeof(upgrades[0]); k++) {
            boot_with_state(&s, upgrades[k]);
            require_condition(s.idle_mode == IDLE_MODE_INDICATOR,
                              "an upgraded device must start on the indicator");
            require_condition(!s.idle_policy_upgraded,
                              "the upgrade must be persisted on first boot");
            require_condition(state_file_contains(
                                  "\"idle\":{\"mode\":1,\"policy\":2}"),
                              "the upgraded idle policy must be written back");
            require_condition(s.current.brightness == 60U ||
                                  k == 2,
                              "the upgrade must keep the owner's other settings");
        }

        /* The owner then chooses off (or always) in the UI: kept across boots. */
        reset(&ctx);
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"set_idle_mode\",\"args\":"
                   "{\"mode\":\"off\"}}");
        require_condition(response_ok(), "the owner may choose off");
        require_condition(state_file_contains("\"idle\":{\"mode\":0,\"policy\":2}"),
                          "an owner choice must be saved under the new policy");
        memset(&s, 0, sizeof(s));
        default_state(&s);
        load_state(&s);
        finish_idle_policy_upgrade(&s);
        require_condition(s.idle_mode == IDLE_MODE_OFF,
                          "a choice made after the upgrade must survive a reboot");
        boot_with_state(&s, "{\"idle\":{\"mode\":2,\"policy\":2}}");
        require_condition(s.idle_mode == IDLE_MODE_ALWAYS,
                          "always-on chosen after the upgrade must survive a reboot");
        (void)unlink(STATE_PATH);
    }

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

    /* ---- 5b. Default-brightness pulse is smooth, not two steps (#364). -- */
    reset(&ctx);
    fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
               "{\"mode\":\"pulse\",\"brightness\":8,\"period_ms\":4000}}");
    require_condition(response_ok() && ctx.sleep_active,
                      "default pulse sleep light must activate");
    {
        /* The stepped pulse drove red code 1 at the trough and 2 at the
           crest; those endpoints and the cap must not move. */
        unsigned int trough = led_output_gamma((255U * 1U + 50U) / 100U);
        unsigned int crest = led_output_gamma((255U * 8U + 50U) / 100U);
        unsigned int seen[12 * 2 + 1] = {0};
        unsigned int distinct = 0U, previous = 0U, k, i;
        int monotonic = 1;

        for (k = 0U; k <= 100U; k++) {
            /* Phase 0.75 is the trough, 0.25 the crest: walk trough->crest. */
            double t = ctx.sleep_started + 4.0 * (0.75 + 0.5 * k / 100.0);
            unsigned int sum = 0U;

            apply_sleep_light(&ctx, t);
            for (i = 0U; i < RING_PIXELS; i++) {
                require_condition(ctx.rendered_pixels[i].r >= trough &&
                                      ctx.rendered_pixels[i].r <= crest,
                                  "a dithered pixel left the stepped pulse's range");
                sum += ctx.rendered_pixels[i].r;
            }
            if (k == 0U)
                require_condition(sum == trough * RING_PIXELS,
                                  "the trough must stay at the old code");
            if (k == 100U)
                require_condition(sum == crest * RING_PIXELS,
                                  "the crest must stay at the old code");
            if (k > 0U && sum < previous)
                monotonic = 0;
            previous = sum;
            if (sum < sizeof(seen) / sizeof(seen[0]) && !seen[sum]++)
                distinct++;
        }
        require_condition(monotonic, "the rising half of the breath must not dip");
        require_condition(distinct >= 10U,
                          "the ring average must move in fine steps, not jump 1->2");
    }

    /* ---- 5c. With the kernel's current scale, the pulse uses the fine
       levels between codes (#364). ------------------------------------- */
    {
        unsigned int fine, code, scale, k, i, distinct = 0U, previous = 0U;
        unsigned int trough = led_output_gamma((255U * 1U + 50U) / 100U);
        unsigned int crest = led_output_gamma((255U * 8U + 50U) / 100U);
        unsigned int seen[2U * 12U * 12U + 1U] = {0};
        unsigned int max_step = 0U;
        int monotonic = 1;
        char frame_file[] = "/tmp/libreecho-ledd-frame-XXXXXX";
        char scaled_file[] = "/tmp/libreecho-ledd-scaled-XXXXXX";
        char text[256];
        FILE *fp;
        int fd;

        /* Every reachable level maps to a (code, scale) that emits it
           exactly, preferring the highest current. */
        for (fine = 0U; fine <= 255U * FINE_UNITS; fine++) {
            reset(&ctx);
            ctx.hw.kind = HW_IS31FL3236;
            snprintf(ctx.hw.is31_scaled_path, sizeof(ctx.hw.is31_scaled_path),
                     "x");
            if (!fine_level_reachable(&ctx.hw, fine))
                continue;
            fine_to_is31(fine, &code, &scale);
            require_condition(scale <= 3U && code <= 255U &&
                                  code * FINE_UNITS == fine * (scale + 1U),
                              "a fine level must map to an exact code/scale");
            require_condition(fine % FINE_UNITS != 0U || scale == 0U,
                              "whole codes must stay at full current");
        }

        fd = mkstemp(frame_file);
        require_condition(fd >= 0, "create fake frame file");
        close(fd);
        fd = mkstemp(scaled_file);
        require_condition(fd >= 0, "create fake scaled_frame file");
        close(fd);
        reset(&ctx);
        ctx.hw.kind = HW_IS31FL3236;
        snprintf(ctx.hw.is31_frame_path, sizeof(ctx.hw.is31_frame_path), "%s",
                 frame_file);
        snprintf(ctx.hw.is31_scaled_path, sizeof(ctx.hw.is31_scaled_path),
                 "%s", scaled_file);
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"sleep_light\",\"args\":"
                   "{\"mode\":\"pulse\",\"brightness\":8,\"period_ms\":4000}}");
        require_condition(response_ok() && ctx.sleep_active,
                          "scaled pulse sleep light must activate");
        for (k = 0U; k <= 200U; k++) {
            double t = ctx.sleep_started + 4.0 * (0.75 + 0.5 * k / 200.0);
            unsigned int sum = 0U, lo = ~0U, hi = 0U;

            apply_sleep_light(&ctx, t);
            for (i = 0U; i < RING_PIXELS; i++) {
                fine = ctx.rendered_fine[i].r;
                require_condition(fine >= trough * FINE_UNITS &&
                                      fine <= crest * FINE_UNITS,
                                  "a fine pixel left the pulse's range");
                require_condition(fine_level_reachable(&ctx.hw, fine),
                                  "a pixel asked for a level the chip lacks");
                require_condition(ctx.rendered_pixels[i].r == fine_to_code(fine),
                                  "status pixels must be the rounded level");
                sum += fine;
                if (fine < lo) lo = fine;
                if (fine > hi) hi = fine;
            }
            /* Two neighbouring levels at most, a quarter code apart at
               the bottom of the breath. */
            if (hi - lo > max_step)
                max_step = hi - lo;
            if (k == 0U)
                require_condition(sum == trough * FINE_UNITS * RING_PIXELS,
                                  "the scaled trough must stay at the old code");
            if (k == 200U)
                require_condition(sum == crest * FINE_UNITS * RING_PIXELS,
                                  "the scaled crest must stay at the old code");
            if (k > 0U && sum < previous)
                monotonic = 0;
            previous = sum;
            sum /= RING_PIXELS; /* mean, in fine units, rounded down */
            if (sum < sizeof(seen) / sizeof(seen[0]) && !seen[sum]++)
                distinct++;
        }
        require_condition(monotonic, "the scaled breath must not dip");
        require_condition(max_step <= FINE_UNITS / 4U,
                          "neighbouring pixels must differ by a quarter code at most");
        require_condition(distinct >= 10U,
                          "the scaled breath must pass through the fine levels");

        /* The hardware gets the scaled frame: 36 codes then 36 scales, and
           each channel re-emits the rendered level. */
        require_condition(truncate(scaled_file, 0) == 0, "truncate scaled");
        apply_sleep_light(&ctx, ctx.sleep_started + 4.0 * 0.80);
        ctx.hw_written_valid = 0;
        ctx.hw_pending = 1;
        require_condition(output_flush(&ctx, monotonic_seconds(), 1) == 1,
                          "forced flush must write");
        fp = fopen(scaled_file, "r");
        require_condition(fp != NULL && fgets(text, sizeof(text), fp) != NULL,
                          "scaled_frame must be written");
        fclose(fp);
        require_condition(strlen(text) == 36U * 4U + 1U && text[144] == '\n',
                          "scaled_frame is 144 hex characters and a newline");
        for (i = 0U; i < 36U; i++) {
            unsigned int c, s, want;
            const struct pixel *px = &ctx.rendered_fine[i / 3U];

            require_condition(sscanf(text + i * 2U, "%2x", &c) == 1 &&
                                  sscanf(text + 72U + i * 2U, "%2x", &s) == 1,
                              "scaled_frame must be hex");
            want = i % 3U == 0U ? px->r : i % 3U == 1U ? px->g : px->b;
            require_condition(s <= 3U && c * FINE_UNITS == want * (s + 1U),
                              "each channel must emit its rendered level");
        }
        fp = fopen(frame_file, "r");
        require_condition(fp != NULL && fgetc(fp) == EOF,
                          "the legacy frame file must not be written too");
        fclose(fp);

        /* Without scaled_frame the legacy 36-byte frame carries whole codes. */
        ctx.hw.is31_scaled_path[0] = '\0';
        apply_sleep_light(&ctx, ctx.sleep_started + 4.0 * 0.80);
        for (i = 0U; i < RING_PIXELS; i++)
            require_condition(ctx.rendered_fine[i].r % FINE_UNITS == 0U,
                              "an old kernel must only get whole codes");
        ctx.hw_written_valid = 0;
        ctx.hw_pending = 1;
        require_condition(output_flush(&ctx, monotonic_seconds(), 1) == 1,
                          "legacy forced flush must write");
        fp = fopen(frame_file, "r");
        require_condition(fp != NULL && fgets(text, sizeof(text), fp) != NULL &&
                              strlen(text) == 73U,
                          "legacy frame must be 72 hex characters");
        fclose(fp);
        unlink(frame_file);
        unlink(scaled_file);
    }

    /* ---- 5d. Every gamma frame takes the smoothed route when the kernel
       has scaled_frame: a low-brightness pulse pattern moves through fine
       levels instead of whole codes, never exceeds the integer frame, and a
       kernel without scaled_frame still gets whole codes. -------------- */
    {
        char scaled_file[] = "/tmp/libreecho-ledd-scaled2-XXXXXX";
        unsigned int k, i, distinct = 0U, previous = 0U, max_step = 0U;
        unsigned int seen[255U * 12U * 12U + 1U];
        int monotonic = 1, fd;

        memset(seen, 0, sizeof(seen));
        fd = mkstemp(scaled_file);
        require_condition(fd >= 0, "create fake scaled_frame file");
        close(fd);
        reset(&ctx);
        ctx.hw.kind = HW_IS31FL3236;
        snprintf(ctx.hw.is31_scaled_path, sizeof(ctx.hw.is31_scaled_path),
                 "%s", scaled_file);
        fire(&ctx, "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
                   "{\"name\":\"pulse\",\"r\":255,\"g\":180,\"b\":0,"
                   "\"brightness\":15,\"repeats\":0,\"owner\":\"evt\"}}");
        require_condition(response_ok() && ctx.pattern_active,
                          "a pulse pattern must take the ring");
        /* Rising half of the 1.5 s pulse: phase 0.75 -> 1.25. */
        for (k = 0U; k <= 200U; k++) {
            double t = ctx.pattern_started + 1.5 * (0.75 + 0.5 * k / 200.0);
            unsigned int sum = 0U;

            update_pattern(&ctx, t);
            for (i = 0U; i < RING_PIXELS; i++) {
                unsigned int fine = ctx.rendered_fine[i].r;

                require_condition(fine_level_reachable(&ctx.hw, fine),
                                  "a smoothed pixel asked for a level the chip lacks");
                require_condition(fine <= ctx.rendered_pixels[i].r * FINE_UNITS,
                                  "smoothing must never boost past the whole code");
                require_condition(ctx.rendered_pixels[i].r == 0U || fine > 0U,
                                  "smoothing must never turn a lit channel off");
                sum += fine;
            }
            if (k > 0U && sum + FINE_UNITS < previous)
                monotonic = 0;
            if (k > 0U && sum > previous && sum - previous > max_step)
                max_step = sum - previous;
            previous = sum;
            if (!seen[sum]++)
                distinct++;
        }
        require_condition(monotonic, "the smoothed pulse must not dip while rising");
        require_condition(distinct >= 60U,
                          "the smoothed pulse must pass through many fine levels");
        require_condition(max_step <= 3U * FINE_UNITS * RING_PIXELS / 4U,
                          "the smoothed pulse must not jump between frames");

        /* A kernel without scaled_frame gets whole codes, as before. */
        ctx.hw.is31_scaled_path[0] = '\0';
        update_pattern(&ctx, ctx.pattern_started + 1.5 * 0.9);
        for (i = 0U; i < RING_PIXELS; i++)
            require_condition(ctx.rendered_fine[i].r ==
                                  ctx.rendered_pixels[i].r * FINE_UNITS,
                              "an old kernel must only get whole codes");
        unlink(scaled_file);
    }

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
    require_condition(indicator_only(&ctx),
                      "expiry must return to the default indicator base");

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
