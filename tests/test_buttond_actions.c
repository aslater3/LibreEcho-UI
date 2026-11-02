#define _POSIX_C_SOURCE 200809L

/*
 * Host regression for the action button's "Start listening" and "Play /
 * pause" behaviours and its short/long-press split (#372). Includes buttond.c
 * so real EV_KEY values go through handle_key() and action_key(); audiod,
 * ledd, waked and radiod are small AF_UNIX mock servers that log every
 * request. The long-press clock is driven explicitly through action_key() and
 * action_long_poll(), so no test waits on wall time. No /dev/input or physical
 * evidence is claimed.
 */
#include "buttond_fixture.h"
#define open buttond_fixture_open
static char test_config_path[256];
#define LE_BUTTOND_CONFIG test_config_path
#define main buttond_program_main
#include "../src/adapter/buttond.c"
#undef main
#undef open

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

enum { MOCK_LED, MOCK_AUDIO, MOCK_WAKE, MOCK_RADIO, MOCK_COUNT };
static const char *mock_names[MOCK_COUNT] = {"led", "audio", "wake", "radio"};
static pid_t mock_pids[MOCK_COUNT], fixture_owner;
static char directory[] = "/tmp/libreecho-button-actions-XXXXXX";
static char sockets[MOCK_COUNT][256], logs[MOCK_COUNT][256];
static int failures;

static void cleanup(void)
{
    int i;
    if (getpid() != fixture_owner) return;
    for (i = 0; i < MOCK_COUNT; ++i) {
        if (mock_pids[i] > 0) { kill(mock_pids[i], SIGTERM); waitpid(mock_pids[i], NULL, 0); }
        unlink(sockets[i]);
        unlink(logs[i]);
    }
    unlink(test_config_path);
    rmdir(directory);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "button action check failed at line %d: %s\n", __LINE__, #condition); \
    failures++; } } while (0)
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "button action setup failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

static int make_listener(const char *path)
{
    struct sockaddr_un address;
    int fd;
    size_t n = strlen(path);

    REQUIRE(n < sizeof(address.sun_path));
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, n + 1);
    unlink(path);
    REQUIRE(bind(fd, (struct sockaddr *)&address,
                 (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1)) == 0);
    REQUIRE(listen(fd, 8) == 0);
    return fd;
}

/*
 * Mock state is seeded from a control file so each case can set up "radio
 * playing", "sleep sound on" or "muted" without restarting the mock:
 * "muted noise radio_playing radio_paused" as four integers.
 */
static char state_path[256];

static void read_state(int *muted, int *noise, int *playing, int *paused)
{
    FILE *f = fopen(state_path, "r");
    *muted = *noise = *playing = *paused = 0;
    if (!f) return;
    if (fscanf(f, "%d %d %d %d", muted, noise, playing, paused) != 4)
        *muted = *noise = *playing = *paused = 0;
    fclose(f);
}

static void write_state(int muted, int noise, int playing, int paused)
{
    FILE *f = fopen(state_path, "w");
    REQUIRE(f != NULL);
    fprintf(f, "%d %d %d %d\n", muted, noise, playing, paused);
    REQUIRE(fclose(f) == 0);
}

static void mock_daemon(int listen_fd, int kind)
{
    FILE *log = fopen(logs[kind], "a"); /* append: the test truncates it */

    if (!log) _exit(2);
    for (;;) {
        char request[4096], response[768];
        ssize_t used = 0;
        int muted, noise, playing, paused, ok = 1;
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0) _exit(2);
        while (used < (ssize_t)sizeof(request) - 1) {
            ssize_t n = read(fd, request + used, sizeof(request) - 1 - (size_t)used);
            if (n <= 0) break;
            used += n;
            if (memchr(request, '\n', (size_t)used)) break;
        }
        request[used > 0 ? used : 0] = '\0';
        fputs(request, log);
        fflush(log);
        read_state(&muted, &noise, &playing, &paused);
        snprintf(response, sizeof(response), "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n");
        if (kind == MOCK_AUDIO && strstr(request, "\"cmd\":\"status\"")) {
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"volume\":50,\"muted\":%s,"
                     "\"noise_active\":%s,\"noise_source\":\"pink\",\"noise_bed\":\"rain\","
                     "\"noise_level\":55}}\n",
                     muted ? "true" : "false", noise ? "true" : "false");
        } else if (kind == MOCK_AUDIO && strstr(request, "\"cmd\":\"noise_stop\"")) {
            write_state(muted, 0, playing, paused);
        } else if (kind == MOCK_AUDIO && strstr(request, "\"cmd\":\"noise_start\"")) {
            write_state(muted, 1, playing, paused);
        } else if (kind == MOCK_RADIO && strstr(request, "\"cmd\":\"status\"")) {
            /* A title that spells a member name must not be read as one. */
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"playing\":%s,\"paused\":%s,"
                     "\"url\":\"http://radio.invalid/\",\"title\":\"\\\"playing\\\":true\"}}\n",
                     playing ? "true" : "false", paused ? "true" : "false");
        } else if (kind == MOCK_RADIO && strstr(request, "\"cmd\":\"pause\"")) {
            ok = playing;
            if (ok) write_state(muted, noise, 0, 1);
        } else if (kind == MOCK_RADIO && strstr(request, "\"cmd\":\"resume\"")) {
            ok = paused;
            if (ok) write_state(muted, noise, 1, 0);
        }
        if (!ok)
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":false,\"error\":\"no player\"}\n");
        if (write(fd, response, strlen(response)) != (ssize_t)strlen(response)) _exit(2);
        close(fd);
    }
}

static int count_in(int kind, const char *needle)
{
    char data[32768];
    FILE *file = fopen(logs[kind], "r");
    size_t n;
    int count = 0;
    const char *p;

    if (!file) return 0;
    n = fread(data, 1, sizeof(data) - 1, file);
    fclose(file);
    data[n] = '\0';
    for (p = data; (p = strstr(p, needle)); p += strlen(needle))
        count++;
    return count;
}

static void truncate_logs(void)
{
    int i;
    for (i = 0; i < MOCK_COUNT; ++i) {
        FILE *f = fopen(logs[i], "r+");
        if (f) { if (ftruncate(fileno(f), 0) != 0) failures++; fclose(f); }
    }
}

int main(void)
{
    struct context ctx;
    int i;

    fixture_owner = getpid();
    REQUIRE(atexit(cleanup) == 0);
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(state_path, sizeof(state_path), "%s/state", directory);
    snprintf(test_config_path, sizeof(test_config_path), "%s/config.json", directory);
    write_state(0, 0, 0, 0);
    for (i = 0; i < MOCK_COUNT; ++i) {
        int fd;
        snprintf(sockets[i], sizeof(sockets[i]), "%s/%s.sock", directory, mock_names[i]);
        snprintf(logs[i], sizeof(logs[i]), "%s/%s.log", directory, mock_names[i]);
        fd = make_listener(sockets[i]);
        mock_pids[i] = fork();
        REQUIRE(mock_pids[i] >= 0);
        if (!mock_pids[i]) mock_daemon(fd, i);
        close(fd);
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.audio_sock = sockets[MOCK_AUDIO];
    ctx.led_sock = sockets[MOCK_LED];
    ctx.wake_sock = sockets[MOCK_WAKE];
    ctx.radio_sock = sockets[MOCK_RADIO];
    ctx.volume = -1;
    ctx.muted = -1;
    ctx.indicated_mute = -1;
    ctx.tones = 1;
    ctx.action_brightness = 70;
    strcpy(ctx.action_sounds, "action-1,action-2");
    strcpy(ctx.action, "listen");
    strcpy(ctx.action_long, "same");

    /* Start listening: a press asks waked to start a turn, at once. */
    handle_key(&ctx, KEY_HELP, 1);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 1);
    handle_key(&ctx, KEY_HELP, 2);                 /* autorepeat never acts */
    handle_key(&ctx, KEY_HELP, 0);                 /* nor does the release */
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 1);
    /* waked draws the listening ring; the action flash must not cover it. */
    CHECK(count_in(MOCK_LED, "\"owner\":\"action\"") == 0);

    /* A muted microphone would send silence: refused, with a tone and flash. */
    write_state(1, 0, 0, 0);
    truncate_logs();
    handle_key(&ctx, KEY_HELP, 1);
    handle_key(&ctx, KEY_HELP, 0);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 0);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"cue\"") == 1);
    CHECK(count_in(MOCK_LED, "\"owner\":\"action\"") == 1);
    write_state(0, 0, 0, 0);

    /* Play / pause, radio: pause, then resume on the next press. */
    strcpy(ctx.action, "playpause");
    write_state(0, 0, 1, 0);
    truncate_logs();
    handle_key(&ctx, KEY_HELP, 1);
    handle_key(&ctx, KEY_HELP, 0);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"pause\"") == 1);
    CHECK(ctx.paused_source == PAUSED_RADIO);
    handle_key(&ctx, KEY_HELP, 1);
    handle_key(&ctx, KEY_HELP, 0);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"resume\"") == 1);
    CHECK(ctx.paused_source == PAUSED_NONE);
    CHECK(count_in(MOCK_LED, "\"owner\":\"action\"") == 2);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"cue\"") == 0);

    /* Play / pause, sleep sound: stopped, then started again as it was. */
    write_state(0, 1, 0, 0);
    truncate_logs();
    CHECK(action_play_pause(&ctx) == 1);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"noise_stop\"") == 1);
    CHECK(ctx.paused_source == PAUSED_NOISE);
    CHECK(action_play_pause(&ctx) == 2);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"noise_start\"") == 1);
    CHECK(count_in(MOCK_AUDIO, "\"source\":\"pink\"") == 1);
    CHECK(count_in(MOCK_AUDIO, "\"bed\":\"rain\"") == 1);
    CHECK(count_in(MOCK_AUDIO, "\"level\":55") == 1);
    CHECK(count_in(MOCK_AUDIO, "\"minutes\":0") == 1);
    CHECK(ctx.paused_source == PAUSED_NONE);

    /* A stopped sleep sound is resumed before a radio someone else paused. */
    write_state(0, 1, 0, 0);
    CHECK(action_play_pause(&ctx) == 1);
    write_state(0, 0, 0, 1);
    truncate_logs();
    CHECK(action_play_pause(&ctx) == 2);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"noise_start\"") == 1);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"resume\"") == 0);

    /* Nothing LibreEcho controls is playing (AirPlay, Bluetooth, silence):
       nothing is sent anywhere, and the press is refused audibly. */
    write_state(0, 0, 0, 0);
    ctx.paused_source = PAUSED_NONE;
    truncate_logs();
    handle_key(&ctx, KEY_HELP, 1);
    handle_key(&ctx, KEY_HELP, 0);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"pause\"") == 0);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"resume\"") == 0);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"noise_") == 0);
    CHECK(count_in(MOCK_AUDIO, "\"cmd\":\"cue\"") == 1);

    /* A radio daemon that is down is not a crash, and not a toggle. */
    ctx.radio_sock = "/tmp/libreecho-button-actions-missing.sock";
    CHECK(action_play_pause(&ctx) == 0);
    ctx.radio_sock = sockets[MOCK_RADIO];

    /* Short and long press. Short: play/pause; long: start listening. */
    strcpy(ctx.action, "playpause");
    strcpy(ctx.action_long, "listen");
    write_state(0, 0, 1, 0);
    truncate_logs();
    action_key(&ctx, 1, 1000);                     /* press: nothing yet */
    CHECK(count_in(MOCK_RADIO, "\"cmd\":") == 0);
    action_long_poll(&ctx, 1000 + ACTION_LONG_PRESS_MS - 1);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 0);
    action_key(&ctx, 0, 1000 + ACTION_LONG_PRESS_MS - 1); /* released in time */
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"pause\"") == 1);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 0);
    action_long_poll(&ctx, 1000 + 5 * ACTION_LONG_PRESS_MS);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 0); /* nothing pending */

    truncate_logs();
    action_key(&ctx, 1, 5000);                     /* held past the threshold */
    action_long_poll(&ctx, 5000 + ACTION_LONG_PRESS_MS);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 1); /* while still held */
    action_long_poll(&ctx, 5000 + 3 * ACTION_LONG_PRESS_MS);
    action_key(&ctx, 2, 5000 + 3 * ACTION_LONG_PRESS_MS);
    action_key(&ctx, 0, 5000 + 4 * ACTION_LONG_PRESS_MS);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 1); /* once per hold */
    CHECK(count_in(MOCK_RADIO, "\"cmd\":") == 0);  /* and no short press too */

    /* A device that disappears mid-hold cannot leave a long press pending. */
    truncate_logs();
    action_key(&ctx, 1, 9000);
    ctx.devices[0].fd = dup(0);
    REQUIRE(ctx.devices[0].fd >= 0);
    ctx.device_count = 1;
    remove_device(&ctx, 0);
    action_long_poll(&ctx, 9000 + 2 * ACTION_LONG_PRESS_MS);
    CHECK(count_in(MOCK_WAKE, "\"cmd\":\"listen\"") == 0);

    /* Long press "disabled": a hold does nothing, a tap still acts. */
    strcpy(ctx.action_long, "disabled");
    write_state(0, 0, 1, 0);
    truncate_logs();
    action_key(&ctx, 1, 20000);
    action_long_poll(&ctx, 20000 + ACTION_LONG_PRESS_MS);
    action_key(&ctx, 0, 20000 + 2 * ACTION_LONG_PRESS_MS);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"pause\"") == 0);
    action_key(&ctx, 1, 30000);
    action_key(&ctx, 0, 30100);
    CHECK(count_in(MOCK_RADIO, "\"cmd\":\"pause\"") == 1);

    /* The settings come from the web config; absent keeps the default. */
    { FILE *saved = fopen(test_config_path, "w");
      REQUIRE(saved != NULL);
      fputs("{\"button_action\":\"listen\",\"button_action_long\":\"playpause\"}", saved);
      REQUIRE(fclose(saved) == 0); }
    strcpy(ctx.action_long, "same");
    refresh_tone_setting(&ctx);
    CHECK(!strcmp(ctx.action, "listen") && !strcmp(ctx.action_long, "playpause"));
    { FILE *saved = fopen(test_config_path, "w");
      REQUIRE(saved != NULL);
      fputs("{\"button_action\":\"sound\"}", saved);
      REQUIRE(fclose(saved) == 0); }
    refresh_tone_setting(&ctx);
    CHECK(!strcmp(ctx.action, "sound") && !strcmp(ctx.action_long, "playpause"));

    if (failures) {
        fprintf(stderr, "button actions: %d check(s) failed\n", failures);
        return 1;
    }
    puts("button actions: start listening, play/pause and short/long press: ok");
    return 0;
}
