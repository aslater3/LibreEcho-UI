/*
 * Regression harness for UI#65: the legacy v1 twelve-level spectrum path must
 * never start a major music FX overlay.
 *
 * A major effect that appears purely because elapsed time or a cooldown
 * expired is explicitly forbidden, and a generic transient overlay cannot
 * supply the structural (feature-vector) confidence that the v2 director path
 * requires.  The compatibility path must nevertheless keep compatible
 * rendering -- the twelve-level spectrum, rhythm step/pulse, beat halo, mood
 * palettes -- and output priority over the v1 stream.
 *
 * This compiles the real daemon source (ledd.c) into the test translation
 * unit, so it drives the shipped compatibility path and inspects the daemon's
 * own state and rendered frame.  Hardware writes are a no-op because the
 * context stays on the stub backend.
 *
 * Build (from the repository root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L \
 *      -Isrc -Isrc/adapter -o build/led-core/test-led-no-timer-fx \
 *      tests/test_led_no_timer_fx.c \
 *      src/adapter/adapter_server.c src/log.c src/adapter/led_output.c \
 *      src/adapter/music_visualizer_protocol.c \
 *      src/adapter/led_music_director.c src/adapter/led_music_render.c
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define main ledd_program_main
#include "../src/adapter/ledd.c"
#undef main

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "LED no-timer-fx: %s\n", message);
        _exit(1);
    }
}

static void reset(struct daemon_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    default_state(&ctx->state);
    ctx->hw.kind = HW_STUB;
    ctx->visualizer_enabled = 1;
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

static unsigned int distinct_colours(const struct daemon_context *ctx)
{
    unsigned int count = 0U;
    size_t i, j;

    for (i = 0; i < RING_PIXELS; i++) {
        int seen = 0;
        for (j = 0; j < i; j++) {
            if (ctx->rendered_pixels[i].r == ctx->rendered_pixels[j].r &&
                ctx->rendered_pixels[i].g == ctx->rendered_pixels[j].g &&
                ctx->rendered_pixels[i].b == ctx->rendered_pixels[j].b) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            count++;
    }
    return count;
}

int main(void)
{
    struct daemon_context ctx;
    unsigned int levels[RING_PIXELS];
    double now;
    size_t i;
    unsigned int frames = 0U;

    /*
     * ---- 1. Steady energetic v1 frames over >= 40 s never start major FX.
     *
     * The removed periodic comet armed itself at first_frame + 8 s and then
     * re-armed roughly every 12 s, so this steady loop crosses several old
     * cadences while the mood stays energetic and no transient is present.
     * Only elapsed time and an expired cooldown could have fired it.
     */
    reset(&ctx);
    for (i = 0; i < RING_PIXELS; i++)
        levels[i] = 200U + (unsigned int)(i % 3U) * 8U;   /* energetic */
    now = 100.0;
    start_visualizer(&ctx, levels, 100U, "music", now);
    require_condition(ctx.visualizer_active,
                      "the first v1 spectrum frame must activate the ring");
    require_condition(ctx.visualizer_fx_kind == 0U && ctx.visualizer_fx_frames == 0U,
                      "the first v1 frame must not start a major FX overlay");

    while (now - 100.0 < 40.0) {
        now += 0.2;
        start_visualizer(&ctx, levels, 100U, "music", now);
        frames++;
        require_condition(ctx.visualizer_fx_kind == 0U,
                          "steady energetic v1 frames must never select a major FX kind");
        require_condition(ctx.visualizer_fx_frames == 0U,
                          "steady energetic v1 frames must never arm major FX frames");
    }
    require_condition(frames >= 150U,
                      "the regression must cover well over 30 s of v1 frames");
    require_condition(max_channel(&ctx) > 0U,
                      "ordinary spectrum rendering must stay lit after 40 s");
    require_condition(distinct_colours(&ctx) > 3U,
                      "ordinary spectrum rendering must keep its palette");

    /*
     * ---- 2. A sharp v1 transient must not start a generic major overlay
     * either, while ordinary beat and rhythm motion still responds to it.
     */
    reset(&ctx);
    for (i = 0; i < RING_PIXELS; i++)
        levels[i] = 20U;
    now = 200.0;
    start_visualizer(&ctx, levels, 100U, "music", now);
    for (i = 0; i < 4U; i++)
        levels[i] = 230U;   /* bass slam: raw peak jump of 210 */
    now += 0.2;
    start_visualizer(&ctx, levels, 100U, "music", now);
    require_condition(ctx.visualizer_fx_kind == 0U && ctx.visualizer_fx_frames == 0U,
                      "a sharp v1 transient must not start a major FX overlay");
    require_condition(ctx.visualizer_beat > 0U,
                      "a sharp transient must still produce ordinary beat motion");
    require_condition(ctx.visualizer_rhythm_pulse > 0U ||
                          ctx.visualizer_rhythm_step != 0U,
                      "a sharp transient must still advance ordinary rhythm motion");
    require_condition(max_channel(&ctx) > 0U,
                      "a transient frame must still render the spectrum");

    /*
     * ---- 3. Output priority is preserved.  A pattern owns the ring, so the
     * v1 spectrum stays armed underneath but never paints over the owner.
     */
    reset(&ctx);
    for (i = 0; i < RING_PIXELS; i++) {
        ctx.rendered_pixels[i].r = 7U;
        ctx.rendered_pixels[i].g = 8U;
        ctx.rendered_pixels[i].b = 9U;
    }
    ctx.pattern_active = 1;
    for (i = 0; i < RING_PIXELS; i++)
        levels[i] = 180U;
    start_visualizer(&ctx, levels, 100U, "music", 300.0);
    require_condition(ctx.visualizer_active,
                      "the v1 spectrum must stay armed underneath a pattern owner");
    for (i = 0; i < RING_PIXELS; i++)
        require_condition(ctx.rendered_pixels[i].r == 7U &&
                              ctx.rendered_pixels[i].g == 8U &&
                              ctx.rendered_pixels[i].b == 9U,
                          "the pattern owner must keep the ring from the v1 spectrum");

    /*
     * ---- 4. v1 beat release and rhythm cooldown follow elapsed time, so a
     * 23.4 fps producer does not decay twice as fast or re-trigger the
     * rhythm on a latched second frame.
     */
    {
        static const double periods[2] = {0.0854, 0.0427};
        unsigned int beat_at[2];
        int k;

        for (k = 0; k < 2; k++) {
            double t0 = 400.0, t;
            reset(&ctx);
            for (i = 0; i < RING_PIXELS; i++)
                levels[i] = 20U;
            start_visualizer(&ctx, levels, 100U, "music", t0);
            for (i = 4; i < 9; i++)
                levels[i] = 230U;          /* mid-band hit, bass unchanged */
            t0 += periods[k];
            start_visualizer(&ctx, levels, 100U, "music", t0);
            require_condition(ctx.visualizer_beat > 0U,
                              "the v1 hit must raise the beat halo");
            for (t = t0 + periods[k]; t < t0 + 0.12 - 1e-9; t += periods[k])
                start_visualizer(&ctx, levels, 100U, "music", t);   /* held */
            apply_visualizer(&ctx, t0 + 0.12);
            beat_at[k] = ctx.visualizer_beat;
        }
        require_condition((beat_at[0] > beat_at[1] ? beat_at[0] - beat_at[1]
                                                   : beat_at[1] - beat_at[0])
                              <= 6U,
                          "v1 beat release must not depend on frame rate");

        /* Rhythm cooldown: hits 42.7 and 85.4 ms after a hit stay blocked
           (the old 2-frame cooldown at 11.7 fps), one 171 ms later fires. */
        {
            unsigned int step;
            double t0 = 500.0;
            reset(&ctx);
            for (i = 0; i < RING_PIXELS; i++)
                levels[i] = 20U;
            start_visualizer(&ctx, levels, 100U, "music", t0);
            for (i = 4; i < 9; i++)
                levels[i] = 230U;
            start_visualizer(&ctx, levels, 100U, "music", t0 + 0.0427);
            step = ctx.visualizer_rhythm_step;
            for (i = 4; i < 9; i++)
                levels[i] = 20U;
            for (i = 9; i < RING_PIXELS; i++)
                levels[i] = 230U;
            start_visualizer(&ctx, levels, 100U, "music", t0 + 0.0854);
            for (i = 0; i < RING_PIXELS; i++)
                levels[i] = i < 4 ? 230U : 20U;
            start_visualizer(&ctx, levels, 100U, "music", t0 + 0.1281);
            require_condition(ctx.visualizer_rhythm_step == step,
                              "a hit within the cooldown must not step the rhythm");
            for (i = 0; i < RING_PIXELS; i++)
                levels[i] = (i >= 4 && i < 9) ? 230U : 20U;
            start_visualizer(&ctx, levels, 100U, "music", t0 + 0.2135);
            require_condition(ctx.visualizer_rhythm_step != step,
                              "a hit after the cooldown must step the rhythm");
        }
    }

    puts("LED no-timer-fx: ok");
    return 0;
}
