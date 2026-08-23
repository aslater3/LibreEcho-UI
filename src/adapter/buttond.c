#define _POSIX_C_SOURCE 200809L

/*
 * LibreEcho physical button daemon.
 *
 * The device carries volume-up, volume-down and microphone-mute buttons on
 * the top face.  The kernel exposes them through evdev (CONFIG_KEYBOARD_GPIO
 * and CONFIG_KEYBOARD_MTK are both enabled, and libreecho-init creates the
 * /dev/input/event* nodes), but nothing in userspace ever opened them, so
 * the buttons did nothing at all: /api/v1/buttons stored an action name and
 * no code read it back.
 *
 * This daemon is a client only.  It translates key events into audiod calls
 * and asks ledd to draw the resulting level on the ring, so a press produces
 * both an audible change and visible feedback.
 */

#include "adapter.h"
#include "buttond_timing.h"

#include "../json.h"
#include "../log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define MAX_DEVICES 8
#define CONNECT_TIMEOUT_MS 400
#define METER_OWNER "buttons"

/* Hold-to-repeat.  gpio-keys does not enable autorepeat by default, so the
   repeat is generated here rather than relying on EV_KEY value 2. */
#define REPEAT_DELAY_MS 400
#define REPEAT_INTERVAL_MS 150

#define DEFAULT_STEP 5
#define DEFAULT_HOLD_MS 1500

/* The input nodes are created by libreecho-init before the services start,
   but nothing guarantees that ordering, and a device can disappear if its
   driver is unbound.  Rescan rather than exiting, so a late-appearing button
   is still picked up without an operator having to restart the daemon. */
#define RESCAN_INTERVAL_MS 5000
#define STATUS_PATH "/run/libreecho/buttond-status"
#define STATUS_TMP_PATH "/run/libreecho/buttond-status.tmp"

#define BITS_PER_LONG (8 * (int)sizeof(long))
#define NBITS(x) (((x) - 1) / BITS_PER_LONG + 1)
#define TEST_BIT(bit, array) \
    (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct device {
    int fd;
    char name[64];
    int volume_capable;
    int mute_capable;
};

struct context {
    const char *audio_sock;
    const char *led_sock;
    struct device devices[MAX_DEVICES];
    size_t device_count;
    int volume;          /* cached; -1 when unknown */
    int muted;           /* cached; -1 when unknown */
    unsigned int step;
    unsigned int hold_ms;
    unsigned int brightness;
    int held_key;        /* key code being held, 0 when idle */
    int rescan_requested;
    int volume_capable;
    int mute_capable;
    long long next_repeat_ms;
};

static volatile sig_atomic_t stop_requested;
static void write_capability_status(const struct context *ctx);

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int environment_unsigned(const char *name, unsigned int fallback,
                                         unsigned int minimum,
                                         unsigned int maximum)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;

    if (!value || !value[0])
        return fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || *end || parsed < minimum || parsed > maximum)
        return fallback;
    return (unsigned int)parsed;
}

/* ---------------------------- Device discovery -------------------------- */

static void recompute_capabilities(struct context *ctx)
{
    size_t i;

    ctx->volume_capable = 0;
    ctx->mute_capable = 0;
    for (i = 0; i < ctx->device_count; i++) {
        ctx->volume_capable |= ctx->devices[i].volume_capable;
        ctx->mute_capable |= ctx->devices[i].mute_capable;
    }
}

static int device_is_interesting(int fd, char *name, size_t name_size,
                                 int *volume_capable, int *mute_capable)
{
    unsigned long ev_bits[NBITS(EV_MAX)];
    unsigned long key_bits[NBITS(KEY_MAX)];

    memset(ev_bits, 0, sizeof(ev_bits));
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
        !TEST_BIT(EV_KEY, ev_bits))
        return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return 0;
    if (!TEST_BIT(KEY_VOLUMEUP, key_bits) &&
        !TEST_BIT(KEY_VOLUMEDOWN, key_bits) &&
        !TEST_BIT(KEY_MUTE, key_bits) &&
        !TEST_BIT(KEY_MICMUTE, key_bits))
        return 0;
    if (volume_capable)
        *volume_capable = TEST_BIT(KEY_VOLUMEUP, key_bits) ||
                          TEST_BIT(KEY_VOLUMEDOWN, key_bits);
    if (mute_capable)
        *mute_capable = TEST_BIT(KEY_MUTE, key_bits) ||
                        TEST_BIT(KEY_MICMUTE, key_bits);
    if (ioctl(fd, EVIOCGNAME(name_size), name) < 0)
        snprintf(name, name_size, "unknown");
    name[name_size - 1] = '\0';
    return 1;
}

static void discover(struct context *ctx)
{
    DIR *dir = opendir(INPUT_DIR);
    struct dirent *entry;

    if (!dir) {
        le_log_debug("buttond: unable to open %s: %s", INPUT_DIR,
                     strerror(errno));
        return;
    }
    while ((entry = readdir(dir)) != NULL &&
           ctx->device_count < MAX_DEVICES) {
        char path[286];
        char name[64] = "";
        int fd;
        int volume_capable = 0;
        int mute_capable = 0;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, entry->d_name) >=
            (int)sizeof(path))
            continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            le_log_debug("buttond: %s not readable: %s", path,
                         strerror(errno));
            continue;
        }
        if (!device_is_interesting(fd, name, sizeof(name),
                                   &volume_capable, &mute_capable)) {
            close(fd);
            continue;
        }
        ctx->devices[ctx->device_count].fd = fd;
        snprintf(ctx->devices[ctx->device_count].name,
                 sizeof(ctx->devices[ctx->device_count].name), "%s", name);
        ctx->devices[ctx->device_count].volume_capable = volume_capable;
        ctx->devices[ctx->device_count].mute_capable = mute_capable;
        ctx->device_count++;
        le_log_info("buttond: watching %s [%s]", path, name);
    }
    closedir(dir);
    recompute_capabilities(ctx);
    if (ctx->device_count)
        le_log_info("buttond: %zu input device(s) with volume or mute keys",
                    ctx->device_count);
    write_capability_status(ctx);
}

static void write_capability_status(const struct context *ctx)
{
    FILE *file;
    int fd;
    int connected = ctx && ctx->device_count > 0;

    if (!ctx)
        return;
    (void)mkdir("/run/libreecho", 0755);
    file = fopen(STATUS_TMP_PATH, "w");
    if (!file)
        return;
    fprintf(file, "schema=1\nstate=%s\nvolume=%d\nmicrophone_mute=%d\naction=0\n",
            connected ? "connected" : "unavailable",
            connected && ctx->volume_capable,
            connected && ctx->mute_capable);
    fflush(file);
    fd = fileno(file);
    if (fd >= 0)
        (void)fsync(fd);
    fclose(file);
    (void)rename(STATUS_TMP_PATH, STATUS_PATH);
}

static void remove_device(struct context *ctx, size_t index)
{
    size_t i;

    close(ctx->devices[index].fd);
    for (i = index + 1; i < ctx->device_count; i++)
        ctx->devices[i - 1] = ctx->devices[i];
    --ctx->device_count;
    recompute_capabilities(ctx);
    ctx->held_key = 0;
    ctx->rescan_requested = 1;
    write_capability_status(ctx);
}

/* ------------------------------ Adapter calls --------------------------- */

static int audio_call(struct context *ctx, const char *cmd, const char *args,
                      char *out, size_t out_size)
{
    struct le_adapter *adapter;
    int result;

    adapter = le_adapter_connect(ctx->audio_sock, CONNECT_TIMEOUT_MS);
    if (!adapter)
        return LE_ADAPTER_ERR_CONNECT;
    result = le_adapter_call(adapter, cmd, args, out, out_size);
    le_adapter_close(adapter);
    return result;
}

static int refresh_audio(struct context *ctx)
{
    char data[512];
    int volume, muted;

    if (audio_call(ctx, "status", NULL, data, sizeof(data)) != LE_ADAPTER_OK ||
        json_get_int(data, "volume", &volume) < 1) {
        ctx->volume = -1;
        ctx->muted = -1;
        return -1;
    }
    ctx->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    ctx->muted = json_get_bool(data, "muted", &muted) > 0 ? muted : -1;
    return 0;
}

static void show_meter(struct context *ctx, unsigned int value, unsigned int r,
                       unsigned int g, unsigned int b)
{
    struct le_adapter *adapter;
    char args[192];

    adapter = le_adapter_connect(ctx->led_sock, CONNECT_TIMEOUT_MS);
    if (!adapter) {
        le_log_debug("buttond: LED daemon unavailable; no visual feedback");
        return;
    }
    snprintf(args, sizeof(args),
             "{\"value\":%u,\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%u,"
             "\"hold_ms\":%u,\"owner\":\"" METER_OWNER "\"}",
             value, r, g, b, ctx->brightness, ctx->hold_ms);
    (void)le_adapter_call(adapter, "meter", args, NULL, 0);
    le_adapter_close(adapter);
}

/* ------------------------------- Actions -------------------------------- */

static void adjust_volume(struct context *ctx, int direction)
{
    char args[64];
    int target;

    if (ctx->volume < 0 && refresh_audio(ctx) != 0) {
        le_log_warn("buttond: audio daemon unavailable; volume key ignored");
        return;
    }
    target = ctx->volume + direction * (int)ctx->step;
    if (target < 0)
        target = 0;
    if (target > 100)
        target = 100;
    snprintf(args, sizeof(args), "{\"volume\":%d}", target);
    if (audio_call(ctx, "set_volume", args, NULL, 0) != LE_ADAPTER_OK) {
        le_log_warn("buttond: set_volume %d failed", target);
        ctx->volume = -1;
        return;
    }
    ctx->volume = target;
    le_log_info("buttond: volume %s -> %d",
                direction > 0 ? "up" : "down", target);
    /*
     * Warm white for a normal level, amber as it approaches full, so the
     * top of the range is distinguishable without counting pixels.
     */
    if (target >= 90)
        show_meter(ctx, (unsigned int)target, 255, 140, 0);
    else
        show_meter(ctx, (unsigned int)target, 120, 200, 255);
}

static void toggle_mute(struct context *ctx)
{
    char args[48];
    int target;

    if (ctx->muted < 0 && refresh_audio(ctx) != 0) {
        le_log_warn("buttond: audio daemon unavailable; mute key ignored");
        return;
    }
    target = ctx->muted > 0 ? 0 : 1;
    snprintf(args, sizeof(args), "{\"muted\":%s}", target ? "true" : "false");
    if (audio_call(ctx, "set_mute", args, NULL, 0) != LE_ADAPTER_OK) {
        le_log_warn("buttond: set_mute failed");
        ctx->muted = -1;
        return;
    }
    ctx->muted = target;
    le_log_info("buttond: microphone %s", target ? "muted" : "unmuted");
    if (target)
        show_meter(ctx, 100, 255, 0, 0);
    else if (ctx->volume >= 0 || refresh_audio(ctx) == 0)
        show_meter(ctx, (unsigned int)ctx->volume, 120, 200, 255);
}

static void handle_key(struct context *ctx, int code, int value)
{
    /* value: 0 release, 1 press, 2 kernel autorepeat. */
    if (value == 0) {
        if (ctx->held_key == code)
            ctx->held_key = 0;
        return;
    }
    switch (code) {
    case KEY_VOLUMEUP:
        if (value == 1)
            (void)refresh_audio(ctx);
        adjust_volume(ctx, 1);
        break;
    case KEY_VOLUMEDOWN:
        if (value == 1)
            (void)refresh_audio(ctx);
        adjust_volume(ctx, -1);
        break;
    case KEY_MUTE:
    case KEY_MICMUTE:
        if (value == 1) {
            (void)refresh_audio(ctx);
            toggle_mute(ctx);
        }
        return;              /* mute does not repeat while held */
    default:
        return;
    }
    if (value == 1) {
        ctx->held_key = code;
        ctx->next_repeat_ms = monotonic_ms() + REPEAT_DELAY_MS;
    }
}

int main(int argc, char **argv)
{
    struct context ctx;
    struct sigaction sa;
    long long next_rescan_ms;
    long long next_status_ms;
    size_t i;

    memset(&ctx, 0, sizeof(ctx));
    le_log_init("buttond", argc, argv);
    ctx.audio_sock = getenv("LE_AUDIO_SOCK");
    if (!ctx.audio_sock || !ctx.audio_sock[0])
        ctx.audio_sock = LE_ADAPTER_AUDIO_SOCK;
    ctx.led_sock = getenv("LE_LED_SOCK");
    if (!ctx.led_sock || !ctx.led_sock[0])
        ctx.led_sock = LE_ADAPTER_LED_SOCK;
    ctx.volume = -1;
    ctx.muted = -1;
    ctx.step = environment_unsigned("LE_BUTTON_VOLUME_STEP", DEFAULT_STEP,
                                    1, 50);
    ctx.hold_ms = environment_unsigned("LE_BUTTON_METER_HOLD_MS",
                                       DEFAULT_HOLD_MS, 200, 10000);
    ctx.brightness = environment_unsigned("LE_BUTTON_METER_BRIGHTNESS", 70,
                                          1, 100);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    discover(&ctx);
    if (!ctx.device_count)
        le_log_warn("buttond: no input device reports volume or mute keys yet; "
                    "rescanning every %d ms", RESCAN_INTERVAL_MS);
    next_rescan_ms = monotonic_ms() + RESCAN_INTERVAL_MS;
    next_status_ms = monotonic_ms() + RESCAN_INTERVAL_MS;
    write_capability_status(&ctx);
    (void)refresh_audio(&ctx);

    while (!stop_requested) {
        struct pollfd fds[MAX_DEVICES];
        int timeout = -1;
        int ready;

        for (i = 0; i < ctx.device_count; i++) {
            fds[i].fd = ctx.devices[i].fd;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }
        timeout = (int)buttond_poll_timeout_ms(monotonic_ms(), next_status_ms,
                                               next_rescan_ms,
                                               ctx.next_repeat_ms,
                                               (int)ctx.device_count,
                                               ctx.held_key);
        ready = poll(fds, (nfds_t)ctx.device_count, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            le_log_error("buttond: poll failed: %s", strerror(errno));
            break;
        }
        if (monotonic_ms() >= next_status_ms) {
            write_capability_status(&ctx);
            next_status_ms = monotonic_ms() + RESCAN_INTERVAL_MS;
        }
        if (!ready && ctx.held_key) {
            handle_key(&ctx, ctx.held_key, 2);
            ctx.next_repeat_ms = monotonic_ms() + REPEAT_INTERVAL_MS;
            continue;
        }
        if (!ctx.device_count &&
            (ctx.rescan_requested || monotonic_ms() >= next_rescan_ms)) {
            ctx.rescan_requested = 0;
            discover(&ctx);
            next_rescan_ms = monotonic_ms() + RESCAN_INTERVAL_MS;
            continue;
        }
        for (i = 0; i < ctx.device_count; i++) {
            struct input_event events[16];
            ssize_t n;
            size_t k;

            if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                le_log_warn("buttond: input device %s disconnected",
                            ctx.devices[i].name);
                remove_device(&ctx, i);
                --i;
                continue;
            }
            if (!(fds[i].revents & POLLIN))
                continue;
            n = read(ctx.devices[i].fd, events, sizeof(events));
            if (n < (ssize_t)sizeof(events[0]))
                continue;
            for (k = 0; k < (size_t)n / sizeof(events[0]); k++) {
                if (events[k].type != EV_KEY)
                    continue;
                handle_key(&ctx, events[k].code, events[k].value);
            }
        }
    }

    for (i = 0; i < ctx.device_count; i++)
        close(ctx.devices[i].fd);
    le_log_info("buttond: stopped");
    return 0;
}
