/*
 * The wake indicator takes its brightness from the Listening theme.
 *
 * The ring's master brightness is what a user turns down when they do not want
 * it lit while idle, and setting it to zero is a reasonable thing to want. The
 * wake indicator must not disappear with it: something has to say the device
 * started listening. So the wake pulse carries its own brightness, and it
 * comes from the Listening state theme, which is already editable in the LED
 * settings rather than being a new knob.
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
        fprintf(stderr, "wake LED profile regression: %s\n", message);
        _exit(1);
    }
}

/* Exactly what wake_led.c puts on the wire for a wake. */
static const char WAKE_PULSE[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"pulse\",\"r\":255,\"g\":0,\"b\":0,"
    "\"brightness\":70,\"repeats\":0,"
    "\"profile\":\"listening\",\"owner\":\"wakeword\"}}";

/* Exactly what voice_listening_led.c puts on the wire after wake detection. */
static const char LISTENING_PULSE[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"pulse\",\"r\":255,\"g\":0,\"b\":0,"
    "\"brightness\":70,\"repeats\":0,"
    "\"profile\":\"listening\",\"owner\":\"voice-listening\"}}";

/* The same pulse from a caller that names no profile. */
static const char PLAIN_PULSE[] =
    "{\"v\":1,\"id\":1,\"cmd\":\"pattern\",\"args\":"
    "{\"name\":\"pulse\",\"r\":255,\"g\":0,\"b\":0,"
    "\"brightness\":70,\"repeats\":0,\"owner\":\"other\"}}";

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

    /*
     * With the ring dark, a wake still shows, and it shows at the Listening
     * theme's brightness rather than the literal in the request.
     */
    reset(&ctx, 50);
    fire(&ctx, WAKE_PULSE);
    require_condition(ctx.pattern_active,
                      "wake pulse did not start with the ring at zero");
    require_condition(ctx.pattern_colour.brightness == 50,
                      "wake pulse did not take the Listening brightness");
    require_condition(ctx.pattern_colour.r == 255 &&
                      ctx.pattern_colour.g == 0 &&
                      ctx.pattern_colour.b == 0,
                      "wake pulse colour should stay the caller's red");

    /* The wake pulse is immediately replaced by the voice-listening pulse.
     * That second request must retain the Listening theme brightness. */
    fire(&ctx, LISTENING_PULSE);
    require_condition(ctx.pattern_colour.brightness == 50,
                      "voice-listening pulse ignored the Listening brightness");
    require_condition(ctx.pattern_colour.r == 255 &&
                      ctx.pattern_colour.g == 0 &&
                      ctx.pattern_colour.b == 0,
                      "voice-listening pulse colour should stay red");

    /*
     * The indicator is transient: it must not disturb the saved ring
     * settings, least of all the zero the user chose.
     */
    require_condition(ctx.state.current.brightness == 0,
                      "wake pulse overwrote the master ring brightness");

    /* Editing the Listening theme changes the indicator. */
    reset(&ctx, 25);
    fire(&ctx, WAKE_PULSE);
    require_condition(ctx.pattern_colour.brightness == 25,
                      "wake pulse ignored an edited Listening brightness");

    /*
     * A profile brightness of zero is honoured rather than silently
     * substituted: someone who turns the indicator off means it.
     */
    reset(&ctx, 0);
    fire(&ctx, WAKE_PULSE);
    require_condition(ctx.pattern_colour.brightness == 0,
                      "a zero Listening brightness should be honoured");
    fire(&ctx, LISTENING_PULSE);
    require_condition(ctx.pattern_colour.brightness == 0,
                      "voice-listening pulse replaced a zero brightness");

    /*
     * Callers that name no profile are unaffected, so this cannot change the
     * behaviour of any other pattern user.
     */
    reset(&ctx, 25);
    fire(&ctx, PLAIN_PULSE);
    require_condition(ctx.pattern_colour.brightness == 70,
                      "a pattern without a profile must use its own brightness");

    return 0;
}
