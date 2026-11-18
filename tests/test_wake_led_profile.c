/*
 * The listening indicator takes its brightness and colour from the Listening
 * theme.
 *
 * The ring's master brightness is what a user turns down when they do not want
 * it lit while idle, and setting it to zero is a reasonable thing to want. The
 * listening indicator must not disappear with it: something has to say the
 * device started listening. So the direction cue carries its own brightness,
 * and both it and the ring colour come from the Listening state theme, which is
 * already editable in the LED settings rather than being a new knob.
 *
 * The pattern path is used rather than `animate` deliberately. `animate`
 * assigns the profile to state.current and persists it, which would overwrite
 * a master brightness of zero permanently; a transient indicator must leave
 * the saved ring settings exactly as it found them.
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
        fprintf(stderr, "listening LED profile regression: %s\n", message);
        _exit(1);
    }
}

/* Exactly what the listening-ring helper puts on the wire for a wake. */
static const char WAKE_DIRECTION[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"direction\",\"azimuth\":270,"
    "\"r\":72,\"g\":185,\"b\":255,\"brightness\":100,"
    "\"repeats\":0,\"profile\":\"listening\",\"owner\":\"wakeword\"}}";

/* The same cue after recognition starts. */
static const char LISTENING_DIRECTION[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"direction\",\"azimuth\":90,"
    "\"r\":72,\"g\":185,\"b\":255,\"brightness\":100,"
    "\"repeats\":0,\"profile\":\"listening\","
    "\"owner\":\"voice-listening\"}}";

/* A direction cue from a caller that names no profile. */
static const char PLAIN_DIRECTION[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"direction\",\"azimuth\":0,"
    "\"r\":1,\"g\":2,\"b\":3,\"brightness\":70,"
    "\"repeats\":0,\"owner\":\"other\"}}";

static void fire(struct daemon_context *ctx, const char *line)
{
    int pair[2];

    require_condition(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                      "socketpair failed");
    (void)handle_request(ctx, pair[1], line);
    close(pair[0]);
    close(pair[1]);
}

static void reset(struct daemon_context *ctx, int listening_brightness)
{
    memset(ctx, 0, sizeof(*ctx));
    default_state(&ctx->state);
    /* The case this exists for: the ring is off at idle. */
    ctx->state.current.brightness = 0;
    ctx->state.profiles[PROFILE_LISTENING].brightness = listening_brightness;
}

int main(void)
{
    struct daemon_context ctx;
    struct colour listening;

    /*
     * With the ring dark, a wake still shows, at the Listening theme's
     * brightness and colour rather than the literal request fields.
     */
    reset(&ctx, 50);
    listening = ctx.state.profiles[PROFILE_LISTENING];
    fire(&ctx, WAKE_DIRECTION);
    require_condition(ctx.pattern_active,
                      "listening cue did not start with the ring at zero");
    require_condition(ctx.pattern_kind == PATTERN_DIRECTION,
                      "wake cue is not a direction pattern");
    require_condition(ctx.pattern_has_azimuth && ctx.pattern_azimuth == 270,
                      "wake cue lost its azimuth");
    require_condition(ctx.pattern_colour.brightness == 50,
                      "wake cue did not take the Listening brightness");
    require_condition(ctx.pattern_colour.r == listening.r &&
                      ctx.pattern_colour.g == listening.g &&
                      ctx.pattern_colour.b == listening.b,
                      "wake cue did not take the Listening colour");

    /* The wake cue is immediately replaced by the recognition cue.  That
     * second request must retain the Listening theme and update the angle. */
    fire(&ctx, LISTENING_DIRECTION);
    require_condition(ctx.pattern_colour.brightness == 50,
                      "recognition cue ignored the Listening brightness");
    require_condition(ctx.pattern_colour.r == listening.r &&
                      ctx.pattern_colour.g == listening.g &&
                      ctx.pattern_colour.b == listening.b,
                      "recognition cue did not take the Listening colour");
    require_condition(ctx.pattern_has_azimuth && ctx.pattern_azimuth == 90,
                      "recognition cue did not update its azimuth");

    /*
     * The indicator is transient: it must not disturb the saved ring
     * settings, least of all the zero the user chose.
     */
    require_condition(ctx.state.current.brightness == 0,
                      "listening cue overwrote the master ring brightness");

    /* Editing the Listening theme changes both dimensions. */
    reset(&ctx, 25);
    ctx.state.profiles[PROFILE_LISTENING].r = 10;
    ctx.state.profiles[PROFILE_LISTENING].g = 20;
    ctx.state.profiles[PROFILE_LISTENING].b = 30;
    fire(&ctx, WAKE_DIRECTION);
    require_condition(ctx.pattern_colour.brightness == 25,
                      "wake cue ignored an edited Listening brightness");
    require_condition(ctx.pattern_colour.r == 10 &&
                      ctx.pattern_colour.g == 20 &&
                      ctx.pattern_colour.b == 30,
                      "wake cue ignored an edited Listening colour");

    /*
     * A profile brightness of zero is honoured rather than silently
     * substituted: someone who turns the indicator off means it.
     */
    reset(&ctx, 0);
    fire(&ctx, WAKE_DIRECTION);
    require_condition(ctx.pattern_colour.brightness == 0,
                      "a zero Listening brightness should be honoured");
    fire(&ctx, LISTENING_DIRECTION);
    require_condition(ctx.pattern_colour.brightness == 0,
                      "recognition cue replaced a zero brightness");

    /*
     * Callers that name no profile are unaffected, so this cannot change the
     * behaviour of any other pattern user.
     */
    reset(&ctx, 25);
    fire(&ctx, PLAIN_DIRECTION);
    require_condition(ctx.pattern_colour.brightness == 70,
                      "a pattern without a profile must use its own brightness");
    require_condition(ctx.pattern_colour.r == 1 &&
                      ctx.pattern_colour.g == 2 &&
                      ctx.pattern_colour.b == 3,
                      "a pattern without a profile must use its own colour");

    /* Stopping the cue leaves the saved ring settings alone. */
    fire(&ctx,
         "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
         "{\"name\":\"stop\",\"owner\":\"other\"}}");
    require_condition(!ctx.pattern_active,
                      "stopping the cue left the pattern active");
    require_condition(ctx.state.current.brightness == 0,
                      "stopping the cue changed the master brightness");

    return 0;
}
