#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define main ledd_program_main
#include "../src/adapter/ledd.c"
#undef main

#include <stdio.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "LED review regression: %s\n", message);
        _exit(1);
    }
}

int main(void)
{
    struct daemon_context ctx;
    struct pixel pixels[RING_PIXELS];
    struct pixel original[RING_PIXELS];
    time_t now;
    struct tm local;
    int minute;
    size_t i;

    memset(&ctx, 0, sizeof(ctx));
    now = time(NULL);
    require_condition(localtime_r(&now, &local) != NULL,
                      "local time unavailable");
    minute = local.tm_hour * 60 + local.tm_min;
    ctx.state.night_enabled = 1;
    ctx.state.night_start_minute = (minute + 1439) % 1440;
    ctx.state.night_end_minute = (minute + 1) % 1440;
    ctx.state.profiles[PROFILE_NIGHT].brightness = 12;
    for (i = 0; i < RING_PIXELS; i++)
        pixels[i] = (struct pixel){255, 220, 100};
    /* This models a visualizer frame whose nominal brightness was 70% but
       whose post-FX contribution saturated a channel to 255. */
    night_cap_pixels(&ctx, pixels);
    require_condition(pixels[0].r == 31 && pixels[0].g == 27 &&
                      pixels[0].b == 12,
                      "night mode did not cap actual pixel intensity");

    memcpy(original, pixels, sizeof(original));
    ctx.state.night_enabled = 0;
    night_cap_pixels(&ctx, pixels);
    require_condition(memcmp(original, pixels, sizeof(original)) == 0,
                      "disabled night mode changed pixel frames");
    require_condition(NIGHT_POLL_MS == 60000,
                      "night mode idle wake interval is not bounded to one minute");

    ctx.state.night_enabled = 1;
    ctx.night_schedule_initialized = 1;
    ctx.night_last_active = night_mode_active(&ctx.state);
    ctx.night_next_check = 100.0;
    night_schedule_tick(&ctx, 10.0);
    require_condition(ctx.night_next_check == 100.0,
                      "status traffic moved the absolute night deadline");
    night_schedule_tick(&ctx, 50.0);
    require_condition(ctx.night_next_check == 100.0,
                      "repeated status traffic reset the night deadline");
    require_condition(night_schedule_timeout(&ctx, 50.0) == 50000,
                      "night timeout was not calculated from the absolute deadline");
    night_schedule_tick(&ctx, 100.0);
    require_condition(ctx.night_next_check == 160.0,
                      "night deadline was not advanced after the boundary");

    puts("LED night cap and idle wake contract: ok");
    return 0;
}
