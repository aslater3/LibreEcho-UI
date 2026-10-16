/*
 * ledd hardware write pacing and music transfer (Radar visualiser freeze /
 * thin ring).
 *
 * 1. The IS31FL3236 frame write is 37 I2C register writes on a bus shared
 *    with the audio codecs; ledd must skip identical frames and pace writes,
 *    while always delivering the newest frame.
 * 2. Music frames bypass the gamma-2 transfer (their levels are already
 *    perceptually shaped); status/idle output keeps it.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define main ledd_program_main
#define LED_STATE_DIR "/tmp/libreecho-ledd-pacing-test"
#define STATE_PATH LED_STATE_DIR "/led-state.json"
#define STATE_TMP_PATH LED_STATE_DIR "/led-state.json.tmp"
#include "../src/adapter/ledd.c"
#undef main

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char frame_path[] = "/tmp/libreecho-ledd-pacing-frame-XXXXXX";

static void require_condition(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "ledd pacing: %s\n", message);
        exit(1);
    }
}

/* Size of the fake sysfs frame file; truncated to 0 between checks so a
   nonzero size means "a hardware write happened". */
static long frame_written(void)
{
    struct stat st;
    long size = stat(frame_path, &st) == 0 ? (long)st.st_size : -1;
    require_condition(truncate(frame_path, 0) == 0, "truncate fake frame");
    return size;
}

static void reset(struct daemon_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    default_state(&ctx->state);
    ctx->hw.kind = HW_IS31FL3236;
    snprintf(ctx->hw.is31_frame_path, sizeof(ctx->hw.is31_frame_path), "%s",
             frame_path);
}

static void solid(struct led_rgb frame[RING_PIXELS], unsigned int r,
                  unsigned int g, unsigned int b)
{
    size_t i;
    for (i = 0; i < RING_PIXELS; i++)
        frame[i] = (struct led_rgb){r, g, b};
}

int main(void)
{
    struct daemon_context ctx;
    struct led_rgb frame[RING_PIXELS], out[RING_PIXELS];
    struct led_output_diag diag;
    struct led_output_state state;
    double now;
    int fd;
    size_t i;

    fd = mkstemp(frame_path);
    require_condition(fd >= 0, "create fake frame file");
    close(fd);
    require_condition(mkdir(LED_STATE_DIR, 0700) == 0 || errno == EEXIST,
                      "state dir");

    /* ---- 1. Transfer: music is linear, status keeps gamma 2. ---------- */
    memset(&state, 0, sizeof(state));
    solid(frame, 40, 20, 60);
    led_output_process_transfer(led_output_default_calibration(), &state,
                                frame, 100U, LE_OUTPUT_TRANSFER_LINEAR, out,
                                &diag);
    for (i = 0; i < RING_PIXELS; i++)
        require_condition(out[i].r == 40 && out[i].g == 20 && out[i].b == 60,
                          "linear transfer must pass music levels through");
    led_output_process(led_output_default_calibration(), &state, frame, 100U,
                       out, &diag);
    require_condition(out[0].r == led_output_gamma(40) && out[0].r < 40,
                      "status output must keep the gamma-2 transfer");
    led_output_process_transfer(led_output_default_calibration(), &state,
                                frame, 50U, LE_OUTPUT_TRANSFER_LINEAR, out,
                                &diag);
    require_condition(out[0].r == 20, "linear transfer still honours brightness");

    reset(&ctx);
    solid(frame, 40, 0, 0);
    output_music_rgb(&ctx, frame, 100U);
    require_condition(ctx.rendered_pixels[0].r == 40,
                      "music frames must reach the ring without gamma");
    reset(&ctx);
    output_logical_rgb(&ctx, frame, 100U);
    require_condition(ctx.rendered_pixels[0].r == led_output_gamma(40),
                      "non-music frames must keep gamma");

    /* ---- 2. Pacing. ---------------------------------------------------- */
    reset(&ctx);
    (void)frame_written();
    solid(frame, 200, 0, 0);
    output_commit(&ctx, frame, 100U);
    require_condition(frame_written() > 0, "first frame must be written at once");
    require_condition(!ctx.hw_pending, "nothing pending after a write");
    require_condition(ctx.hw_next_write > 0.0, "a pacing deadline is set");

    /* Identical frame: never rewritten, even when the interval has passed. */
    ctx.hw_next_write = 0.0;
    output_commit(&ctx, frame, 100U);
    require_condition(frame_written() == 0, "identical frame must be skipped");
    require_condition(!ctx.hw_pending, "identical frame leaves nothing pending");

    /* A changed frame inside the interval is deferred, not dropped. */
    now = monotonic_seconds();
    ctx.hw_next_write = now + 10.0;
    solid(frame, 0, 200, 0);
    output_commit(&ctx, frame, 100U);
    solid(frame, 0, 0, 200);
    output_commit(&ctx, frame, 100U);
    require_condition(frame_written() == 0, "changed frame inside the interval waits");
    require_condition(ctx.hw_pending, "deferred frame stays pending");
    require_condition(output_flush_timeout(&ctx, now) > 0,
                      "a pending frame must bound the poll timeout");
    require_condition(output_flush(&ctx, now, 0) == 0,
                      "flush before the deadline must not write");
    ctx.hw_next_write = now - 0.001;
    require_condition(output_flush(&ctx, now, 0) == 1, "due frame must flush");
    require_condition(frame_written() > 0, "due frame reaches the hardware");
    require_condition(ctx.hw_written[0].b == ctx.rendered_pixels[0].b &&
                      ctx.hw_written[0].g == 0,
                      "the newest frame wins; intermediate frames coalesce");
    require_condition(output_flush_timeout(&ctx, now) == -1,
                      "no timeout once nothing is pending");

    /* Shutdown forces the final frame regardless of pacing. */
    ctx.hw_next_write = monotonic_seconds() + 10.0;
    hardware_apply(&ctx, &(struct colour){0, 0, 0, 0});
    require_condition(output_flush(&ctx, monotonic_seconds(), 1) == 1 &&
                      frame_written() > 0, "forced flush writes the dark frame");

    /* Pacing interval tracks measured cost, bounded both ways. */
    ctx.hw_write_cost = 0.060;
    ctx.hw_next_write = 0.0;
    solid(frame, 10, 10, 10);
    output_commit(&ctx, frame, 100U);
    require_condition(ctx.hw_next_write - monotonic_seconds() <=
                          HW_WRITE_MAX_INTERVAL + 0.01,
                      "pacing interval is capped");

    unlink(frame_path);
    puts("ledd hardware pacing and music transfer: ok");
    return 0;
}
