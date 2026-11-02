#define _POSIX_C_SOURCE 200809L

/*
 * Behavioral host regression for buttond.  This includes the dispatcher so
 * the test feeds real EV_KEY codes through handle_key(), while the adapter
 * endpoints are small AF_UNIX mock servers.  It does not require /dev/uinput
 * or claim physical evdev evidence.
 */
#include "buttond_fixture.h"
#define open buttond_fixture_open
static char test_config_path[256];
#define LE_BUTTOND_CONFIG test_config_path
#define main buttond_program_main
#include "../src/adapter/buttond.c"
#undef main
#undef open

#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static pid_t audio_pid, led_pid, fixture_owner;
static char directory[] = "/tmp/libreecho-button-events-XXXXXX";

static void cleanup(void)
{
    const char *files[] = {"audio.sock", "led.sock", "audio.log", "led.log", "config.json",
                           "esphome-status.json", "esphome-control.sock"};
    size_t i;
    char path[256];
    if (getpid() != fixture_owner) return;
    if (audio_pid > 0) { kill(audio_pid, SIGTERM); waitpid(audio_pid, NULL, 0); }
    if (led_pid > 0) { kill(led_pid, SIGTERM); waitpid(led_pid, NULL, 0); }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        unlink(path);
    }
    rmdir(directory);
}

/* Unlike abort(), exit() invokes fixture cleanup on an assertion failure. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "button event assertion failed at line %d: %s\\n", __LINE__, #condition); \
    exit(1); \
} } while (0)

static int make_listener(const char *path)
{
    struct sockaddr_un address;
    int fd;
    size_t n = strlen(path);

    assert(n < sizeof(address.sun_path));
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, n + 1);
    unlink(path);
    assert(bind(fd, (struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1)) == 0);
    assert(listen(fd, 8) == 0);
    return fd;
}

static void mock_daemon(int listen_fd, const char *log_path, int audio)
{
    FILE *log = fopen(log_path, "w");
    int volume = 50;
    int muted = 0;
    int noise = 0;


    assert(log != NULL);
    for (;;) {
        char request[4096];
        char response[512];
        ssize_t used = 0;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
            _exit(2);
        while (used < (ssize_t)sizeof(request) - 1) {
            ssize_t n = read(fd, request + used, sizeof(request) - 1 - (size_t)used);
            if (n <= 0)
                break;
            used += n;
            if (memchr(request, '\n', (size_t)used))
                break;
        }
        request[used > 0 ? used : 0] = '\0';
        fputs(request, log);
        fflush(log);
        if (audio && strstr(request, "\"cmd\":\"status\"")) {
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"volume\":%d,\"muted\":%s,\"noise_active\":%s}}\n",
                     volume, muted ? "true" : "false", noise ? "true" : "false");
        } else {
            if (audio && strstr(request, "\"cmd\":\"set_volume\"")) {
                const char *value = strstr(request, "\"volume\":");
                if (value)
                    volume = atoi(value + strlen("\"volume\":"));
            }
            if (audio && strstr(request, "\"cmd\":\"noise_start\""))
                noise = 1;
            if (audio && strstr(request, "\"cmd\":\"noise_stop\""))
                noise = 0;
            if (audio && strstr(request, "\"cmd\":\"set_mute\""))
                muted = strstr(request, "\"muted\":true") != NULL;
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n");
        }
        /* Reserved fixture brightness injects a real adapter rejection. */
        if (!audio && strstr(request, "\"brightness\":26"))
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":false,\"error\":\"fixture rejection\"}\n");
        assert(write(fd, response, strlen(response)) == (ssize_t)strlen(response));
        close(fd);

    }
}

static void write_voice_status(const char *path, int busy)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    fprintf(file, "{\"ready\":true,\"connected\":true,\"in_progress\":%s}\n",
            busy ? "true" : "false");
    assert(fclose(file) == 0);
}

/* What the esphomed control socket received, or "" if nothing arrived. */
static void control_received(int fd, char *out, size_t size)
{
    ssize_t n = recv(fd, out, size - 1, MSG_DONTWAIT);
    out[n > 0 ? n : 0] = '\0';
}

static int log_contains(const char *path, const char *needle)
{
    char data[16384];
    FILE *file = fopen(path, "r");
    size_t n;

    if (!file)
        return 0;
    n = fread(data, 1, sizeof(data) - 1, file);
    fclose(file);
    data[n] = '\0';
    return strstr(data, needle) != NULL;
}

int main(void)
{

    char audio_socket[256], led_socket[256], audio_log[256], led_log[256];
    struct context ctx;
    struct timespec pause = {0, 100000000L};
    int audio_fd, led_fd;
    fixture_owner = getpid();
    assert(atexit(cleanup) == 0);
    assert(mkdtemp(directory) != NULL);
    snprintf(audio_socket, sizeof(audio_socket), "%s/audio.sock", directory);
    snprintf(led_socket, sizeof(led_socket), "%s/led.sock", directory);
    snprintf(audio_log, sizeof(audio_log), "%s/audio.log", directory);
    snprintf(led_log, sizeof(led_log), "%s/led.log", directory);
    audio_fd = make_listener(audio_socket);
    led_fd = make_listener(led_socket);
    audio_pid = fork();
    assert(audio_pid >= 0);
    if (!audio_pid)
        mock_daemon(audio_fd, audio_log, 1);
    led_pid = fork();
    assert(led_pid >= 0);
    if (!led_pid)
        mock_daemon(led_fd, led_log, 0);
    close(audio_fd);
    close(led_fd);

    memset(&ctx, 0, sizeof(ctx));
    ctx.audio_sock = audio_socket;
    ctx.led_sock = led_socket;
    ctx.volume = -1;
    ctx.muted = -1;
    ctx.step = 5;
    ctx.hold_ms = 1000;
    ctx.brightness = 70;
    ctx.indicated_mute = -1;
    ctx.tones = 1;
    strcpy(ctx.action, "sound");
    strcpy(ctx.action_sounds, "action-1,action-2");
    ctx.action_brightness = 70;
    ctx.mute_brightness = 60;

    handle_key(&ctx, KEY_VOLUMEUP, 1);
    assert(ctx.volume == 55);
    handle_key(&ctx, KEY_VOLUMEDOWN, 1);
    assert(ctx.volume == 50);
    handle_key(&ctx, KEY_VOLUMEDOWN, 0);
    assert(ctx.volume == 50);
    handle_key(&ctx, KEY_POWER, 1);
    assert(ctx.muted == 1);
    handle_key(&ctx, KEY_POWER, 1);
    assert(ctx.muted == 0);
    handle_key(&ctx, KEY_MUTE, 1);
    assert(ctx.muted == 1);
    handle_key(&ctx, KEY_MICMUTE, 1);
    assert(ctx.muted == 0);
    handle_key(&ctx, KEY_HELP, 1);
    assert(ctx.action_index == 1);
    handle_key(&ctx, KEY_HELP, 2);
    handle_key(&ctx, KEY_HELP, 0);
    assert(ctx.action_index == 1);
    handle_key(&ctx, KEY_HELP, 1);
    assert(ctx.action_index == 0);
    strcpy(ctx.action, "disabled");
    handle_key(&ctx, KEY_HELP, 1);
    assert(ctx.action_index == 0);

    /* Action "noise" is a sleep-sound switch: on, then off on the next press,
       reading the state from audiod each time. */
    strcpy(ctx.action, "noise");
    assert(action_noise_toggle(&ctx) == 1);         /* nothing playing: start */
    assert(log_contains(audio_log, "\"cmd\":\"noise_start\""));
    assert(log_contains(audio_log, "\"source\":\"white\""));
    assert(log_contains(audio_log, "\"minutes\":0"));  /* until stopped */
    assert(!log_contains(audio_log, "\"cmd\":\"noise_stop\""));
    assert(action_noise_toggle(&ctx) == 0);         /* playing: stop */
    assert(log_contains(audio_log, "\"cmd\":\"noise_stop\""));
    handle_key(&ctx, KEY_HELP, 1);                  /* through the dispatcher: on */
    handle_key(&ctx, KEY_HELP, 2);                  /* autorepeat must not toggle */
    handle_key(&ctx, KEY_HELP, 0);
    assert(action_noise_toggle(&ctx) == 0);         /* so it is still on: stop */
    assert(ctx.action_index == 0);                  /* the sound rotation is untouched */

    /* Read saved preferences and force an already-muted ring to be refreshed. */
    snprintf(test_config_path, sizeof(test_config_path), "%s/config.json", directory);
    { FILE *saved = fopen(test_config_path, "w");
      assert(saved != NULL);
      fputs("{\"button_tones\":false,\"button_action\":\"disabled\","
            "\"button_action_sounds\":\"\",\"button_mute_brightness\":25}", saved);
      assert(fclose(saved) == 0);
    }
    ctx.muted = ctx.indicated_mute = 1;
    refresh_tone_setting(&ctx);
    assert(ctx.tones == 0 && !strcmp(ctx.action, "disabled"));
    assert(!ctx.action_sounds[0]);
    assert(ctx.mute_brightness == 25 && ctx.indicated_mute == -1);
    mute_indicator(&ctx, ctx.muted);
    assert(ctx.indicated_mute == 1);
    assert(log_contains(led_log, "\"brightness\":25"));
    ctx.indicated_mute = -1;
    ctx.mute_brightness = 26;
    mute_indicator(&ctx, 1);
    assert(ctx.indicated_mute == -1);
    ctx.mute_brightness = 25;
    mute_indicator(&ctx, 1);
    assert(ctx.indicated_mute == 1);
    nanosleep(&pause, NULL);
    assert(log_contains(audio_log, "\"cmd\":\"set_mute\""));
    assert(log_contains(audio_log, "\"cmd\":\"sample\""));
    assert(log_contains(audio_log, "action-1"));
    assert(log_contains(audio_log, "action-2"));
    assert(buttond_fixture_opens > 0);

    /* #404: while a voice request is in progress the action press cancels it
       instead of doing its usual job; otherwise the usual job is untouched. */
    {
        char status_path[256], control_path[256], got[32];
        struct sockaddr_un address;
        int control = socket(AF_UNIX, SOCK_DGRAM, 0);
        unsigned int index;

        snprintf(status_path, sizeof(status_path), "%s/esphome-status.json", directory);
        snprintf(control_path, sizeof(control_path), "%s/esphome-control.sock", directory);
        assert(control >= 0);
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        assert(strlen(control_path) < sizeof(address.sun_path));
        memcpy(address.sun_path, control_path, strlen(control_path) + 1);
        assert(bind(control, (struct sockaddr *)&address, sizeof(address)) == 0);
        ctx.esphome_status = status_path;
        ctx.esphome_control = control_path;
        strcpy(ctx.action, "sound");
        strcpy(ctx.action_sounds, "action-1,action-2");
        ctx.action_index = 0;

        write_voice_status(status_path, 1);
        handle_key(&ctx, KEY_HELP, 1);
        control_received(control, got, sizeof(got));
        assert(!strcmp(got, "cancel"));
        assert(ctx.action_index == 0);        /* no action sound over the cancel */
        handle_key(&ctx, KEY_HELP, 2);        /* autorepeat sends nothing more */
        control_received(control, got, sizeof(got));
        assert(!got[0]);

        write_voice_status(status_path, 0);   /* idle: the usual action */
        handle_key(&ctx, KEY_HELP, 1);
        control_received(control, got, sizeof(got));
        assert(!got[0]);
        assert(ctx.action_index == 1);

        strcpy(ctx.action, "disabled");       /* a disabled button stays inert */
        write_voice_status(status_path, 1);
        handle_key(&ctx, KEY_HELP, 1);
        control_received(control, got, sizeof(got));
        assert(!got[0]);

        /* A stale "in progress" from a stopped satellite must not eat the press. */
        strcpy(ctx.action, "sound");
        close(control);
        unlink(control_path);
        index = ctx.action_index;
        handle_key(&ctx, KEY_HELP, 1);
        assert(ctx.action_index != index);
        unlink(status_path);
        handle_key(&ctx, KEY_HELP, 1);        /* no status file at all */
        assert(ctx.action_index == index);
    }


    puts("button event dispatch: volume, KEY_POWER mute sync, KEY_HELP rotation, sleep-sound toggle and voice cancel: ok");
    return 0;
}
