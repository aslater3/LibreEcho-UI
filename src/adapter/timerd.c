/*
 * Timer and alarm daemon.
 *
 * Owns the schedule, rings when something comes due, and stops when told.
 * The scheduling rules are in timer_schedule.c so they can be tested without
 * clocks or sockets; this file is the part that talks to the world.
 *
 * Ringing is a repeated two-tone cue on audiod rather than a bundled sound
 * file. Raw PCM barely compresses and every second of it is about 96 KB of
 * boot image, which is a poor trade for a tone the device can synthesise.
 *
 * "Stop" dismisses every ringing timer and nothing else. Cancelling a timer
 * that has not gone off is a separate command, because someone silencing an
 * alarm at 07:00 has not asked to lose the 07:15 one.
 */

#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "timer_schedule.h"
#include "../json.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TIMER_SOCK "/run/libreecho/timer.sock"
#define STATE_PATH "/data/libreecho/config/timers"
#define MAX_CLIENTS 4
#define POLL_CAP_MS 60000
#define AUDIO_TIMEOUT_MS 1000

/* The ring: a two-tone cue every RING_PERIOD_MS. Long enough to be a pattern
   rather than a siren, short enough that a room notices. */
#define RING_PERIOD_MS 2000
#define RING_LOW_HZ 880
#define RING_HIGH_HZ 1320
#define RING_CUE_MS 250

struct context {
    struct le_timer_set timers;
    const char *socket_path;
    const char *state_path;
    const char *audio_sock;
    int listen_fd;
    int clients[MAX_CLIENTS];
    long long next_ring_ms;
    int dirty;
};

static volatile sig_atomic_t stop_requested;

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
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static long long wall_epoch(void)
{
    return (long long)time(NULL);
}

/* ------------------------------ persistence ----------------------------- */

/*
 * One record per line: "kind due_epoch label".
 *
 * Countdowns are stored as a wall-clock instant even though they run on the
 * monotonic clock, because the monotonic clock restarts at boot and a stored
 * monotonic value would mean nothing afterwards. That conversion needs a
 * credible wall clock, so with an unset clock countdowns are simply not
 * persisted -- better than writing a due time that is decades wrong.
 */
static void state_save(struct context *ctx)
{
    char temporary[256];
    FILE *file;
    long long now_ms = monotonic_ms();
    long long now_epoch = wall_epoch();
    size_t i;

    if (!ctx->state_path || !ctx->state_path[0])
        return;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", ctx->state_path) >=
        (int)sizeof(temporary))
        return;

    file = fopen(temporary, "w");
    if (!file) {
        le_log_warn("timerd: cannot write %s: %s", temporary,
                    strerror(errno));
        return;
    }
    for (i = 0; i < LE_TIMER_MAX; ++i) {
        const struct le_timer *timer = &ctx->timers.timers[i];
        long long due;

        if (timer->state == LE_TIMER_STATE_FREE)
            continue;
        /* A ringing timer is not saved: it has already gone off, and coming
           back from a reboot still ringing would be a surprise, not a
           service. */
        if (timer->state == LE_TIMER_STATE_RINGING)
            continue;
        if (timer->kind == LE_TIMER_ALARM) {
            due = timer->due_epoch;
        } else {
            if (now_epoch < LE_TIMER_CLOCK_VALID_EPOCH)
                continue;
            due = now_epoch + (timer->due_monotonic_ms - now_ms) / 1000LL;
        }
        fprintf(file, "%s %lld %s\n",
                timer->kind == LE_TIMER_ALARM ? "alarm" : "countdown",
                due, timer->label);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        le_log_warn("timerd: cannot flush %s", temporary);
        fclose(file);
        unlink(temporary);
        return;
    }
    fclose(file);
    /* Rename over the real file so a crash mid-write cannot leave a partial
       schedule behind. */
    if (rename(temporary, ctx->state_path) != 0) {
        le_log_warn("timerd: cannot replace %s: %s", ctx->state_path,
                    strerror(errno));
        unlink(temporary);
    }
}

static void state_load(struct context *ctx)
{
    char line[256];
    FILE *file;
    long long now_ms = monotonic_ms();
    long long now_epoch = wall_epoch();

    if (!ctx->state_path || !ctx->state_path[0])
        return;
    file = fopen(ctx->state_path, "r");
    if (!file)
        return;

    while (fgets(line, sizeof(line), file)) {
        char kind[16];
        long long due = 0;
        char label[LE_TIMER_LABEL_MAX];
        unsigned int id = 0;
        int consumed = 0;

        label[0] = '\0';
        if (sscanf(line, "%15s %lld %n", kind, &due, &consumed) < 2)
            continue;
        if (consumed > 0) {
            size_t length;

            strncpy(label, line + consumed, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
            length = strlen(label);
            while (length && (label[length - 1] == '\n' ||
                              label[length - 1] == '\r'))
                label[--length] = '\0';
        }

        if (!strcmp(kind, "alarm")) {
            if (le_timer_add_alarm(&ctx->timers, due, label, now_epoch, &id) !=
                LE_TIMER_OK)
                ++ctx->timers.missed;
        } else if (!strcmp(kind, "countdown")) {
            long long remaining = due - now_epoch;

            /* Restored against the wall clock it was saved with, then handed
               back to the monotonic clock it runs on. A countdown whose time
               passed while the device was down is dropped by the same rule
               that drops one missed while it was running. */
            if (now_epoch < LE_TIMER_CLOCK_VALID_EPOCH ||
                remaining < LE_TIMER_MIN_SECONDS) {
                ++ctx->timers.missed;
                continue;
            }
            if (le_timer_add_countdown(&ctx->timers, remaining, label, now_ms,
                                       &id) != LE_TIMER_OK)
                ++ctx->timers.missed;
        }
    }
    fclose(file);
    if (le_timer_active_count(&ctx->timers))
        le_log_info("timerd: restored %d timer(s)",
                    le_timer_active_count(&ctx->timers));
    if (ctx->timers.missed)
        le_log_info("timerd: %u timer(s) came due while the device was down",
                    ctx->timers.missed);
}

/* -------------------------------- ringing ------------------------------- */

static void audio_cue(struct context *ctx)
{
    struct le_adapter *adapter;
    char args[128];

    adapter = le_adapter_connect(ctx->audio_sock, AUDIO_TIMEOUT_MS);
    if (!adapter) {
        le_log_warn("timerd: audio daemon unavailable; timer is silent");
        return;
    }
    snprintf(args, sizeof(args),
             "{\"first_hz\":%d,\"second_hz\":%d,\"ms\":%d}",
             RING_LOW_HZ, RING_HIGH_HZ, RING_CUE_MS);
    (void)le_adapter_call(adapter, "cue", args, NULL, 0);
    le_adapter_close(adapter);
}

static void ring_tick(struct context *ctx, long long now_ms)
{
    if (le_timer_ringing_count(&ctx->timers) <= 0) {
        ctx->next_ring_ms = 0;
        return;
    }
    if (now_ms < ctx->next_ring_ms)
        return;
    audio_cue(ctx);
    ctx->next_ring_ms = now_ms + RING_PERIOD_MS;
}

/* ------------------------------- commands ------------------------------- */

static int timers_json(struct context *ctx, char *out, size_t size)
{
    long long now_ms = monotonic_ms();
    long long now_epoch = wall_epoch();
    size_t used = 0;
    size_t i;
    int first = 1;
    int written;

    written = snprintf(out, size, "{\"timers\":[");
    if (written < 0 || (size_t)written >= size)
        return -1;
    used = (size_t)written;

    for (i = 0; i < LE_TIMER_MAX; ++i) {
        const struct le_timer *timer = &ctx->timers.timers[i];
        char label[LE_TIMER_LABEL_MAX * 2];
        long long remaining;

        if (timer->state == LE_TIMER_STATE_FREE)
            continue;
        if (timer->kind == LE_TIMER_ALARM)
            remaining = timer->due_epoch - now_epoch;
        else
            remaining = (timer->due_monotonic_ms - now_ms) / 1000LL;
        if (remaining < 0)
            remaining = 0;
        json_escape(label, sizeof(label), timer->label);

        written = snprintf(out + used, size - used,
                           "%s{\"id\":%u,\"kind\":\"%s\",\"state\":\"%s\","
                           "\"seconds_remaining\":%lld,\"label\":\"%s\"}",
                           first ? "" : ",", timer->id,
                           timer->kind == LE_TIMER_ALARM ? "alarm"
                                                         : "countdown",
                           timer->state == LE_TIMER_STATE_RINGING
                               ? "ringing" : "pending",
                           remaining, label);
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
        first = 0;
    }
    written = snprintf(out + used, size - used,
                       "],\"ringing\":%d,\"missed\":%u}",
                       le_timer_ringing_count(&ctx->timers),
                       ctx->timers.missed);
    if (written < 0 || (size_t)written >= size - used)
        return -1;
    return 0;
}

static const char *add_error(int result)
{
    switch (result) {
    case LE_TIMER_ERR_FULL:
        return "no free timer slots";
    case LE_TIMER_ERR_RANGE:
        return "time out of range";
    case LE_TIMER_ERR_CLOCK:
        return "clock not synchronised yet";
    default:
        return "cannot add timer";
    }
}

static int dispatch(struct context *ctx, const char *cmd, const char *args,
                    unsigned long id, char *out, size_t size)
{
    char data[LE_ADAPTER_MSG_MAX];
    char label[LE_TIMER_LABEL_MAX];
    int value = 0;

    label[0] = '\0';
    if (args)
        (void)json_get_string(args, "label", label, sizeof(label));

    if (!strcmp(cmd, "status") || !strcmp(cmd, "list")) {
        if (timers_json(ctx, data, sizeof(data)) != 0)
            return le_adapter_respond_err(out, size, id, "too many timers");
        return le_adapter_respond_ok(out, size, id, data);
    }

    if (!strcmp(cmd, "add")) {
        unsigned int created = 0;
        int result;

        if (!args || json_get_int(args, "seconds", &value) < 1)
            return le_adapter_respond_err(out, size, id,
                                          "add requires seconds");
        result = le_timer_add_countdown(&ctx->timers, value, label,
                                        monotonic_ms(), &created);
        if (result != LE_TIMER_OK)
            return le_adapter_respond_err(out, size, id, add_error(result));
        ctx->dirty = 1;
        snprintf(data, sizeof(data), "{\"id\":%u}", created);
        le_log_info("timerd: countdown %u for %d s", created, value);
        return le_adapter_respond_ok(out, size, id, data);
    }

    if (!strcmp(cmd, "add_alarm")) {
        unsigned int created = 0;
        int result;

        if (!args || json_get_int(args, "epoch", &value) < 1)
            return le_adapter_respond_err(out, size, id,
                                          "add_alarm requires epoch");
        result = le_timer_add_alarm(&ctx->timers, value, label, wall_epoch(),
                                    &created);
        if (result != LE_TIMER_OK)
            return le_adapter_respond_err(out, size, id, add_error(result));
        ctx->dirty = 1;
        snprintf(data, sizeof(data), "{\"id\":%u}", created);
        le_log_info("timerd: alarm %u at %d", created, value);
        return le_adapter_respond_ok(out, size, id, data);
    }

    /*
     * Two commands that exist so callers do not have to parse the list and
     * re-derive what the daemon already knows. Voice control needs "cancel
     * everything" and "how long is left", and doing that by scraping the
     * status array in the caller would duplicate the schedule's rules
     * outside the schedule.
     */
    if (!strcmp(cmd, "cancel_all")) {
        size_t i;
        int cancelled = 0;

        for (i = 0; i < LE_TIMER_MAX; ++i) {
            if (ctx->timers.timers[i].state != LE_TIMER_STATE_PENDING)
                continue;
            memset(&ctx->timers.timers[i], 0,
                   sizeof(ctx->timers.timers[i]));
            ++cancelled;
        }
        if (cancelled)
            ctx->dirty = 1;
        snprintf(data, sizeof(data), "{\"cancelled\":%d}", cancelled);
        return le_adapter_respond_ok(out, size, id, data);
    }

    /* The soonest pending timer, and how many there are. */
    if (!strcmp(cmd, "next")) {
        long long now_ms = monotonic_ms();
        long long now_epoch = wall_epoch();
        long long soonest = -1;
        char label[LE_TIMER_LABEL_MAX * 2] = "";
        int count = 0;
        size_t i;

        for (i = 0; i < LE_TIMER_MAX; ++i) {
            const struct le_timer *timer = &ctx->timers.timers[i];
            long long remaining;

            if (timer->state != LE_TIMER_STATE_PENDING)
                continue;
            ++count;
            if (timer->kind == LE_TIMER_ALARM)
                remaining = timer->due_epoch - now_epoch;
            else
                remaining = (timer->due_monotonic_ms - now_ms) / 1000LL;
            if (remaining < 0)
                remaining = 0;
            if (soonest < 0 || remaining < soonest) {
                soonest = remaining;
                json_escape(label, sizeof(label), timer->label);
            }
        }
        snprintf(data, sizeof(data),
                 "{\"count\":%d,\"seconds_remaining\":%lld,"
                 "\"label\":\"%s\"}",
                 count, soonest < 0 ? 0 : soonest, label);
        return le_adapter_respond_ok(out, size, id, data);
    }

    if (!strcmp(cmd, "cancel")) {
        if (!args || json_get_int(args, "id", &value) < 1)
            return le_adapter_respond_err(out, size, id,
                                          "cancel requires id");
        if (le_timer_cancel(&ctx->timers, (unsigned int)value) != LE_TIMER_OK)
            return le_adapter_respond_err(out, size, id, "no such timer");
        ctx->dirty = 1;
        return le_adapter_respond_ok(out, size, id, "{}");
    }

    /* What "Alexa, stop" reaches. Silences every ring and leaves pending
       timers alone. */
    if (!strcmp(cmd, "dismiss")) {
        int stopped;

        if (args && json_get_int(args, "id", &value) == 1)
            stopped = le_timer_dismiss(&ctx->timers, (unsigned int)value);
        else
            stopped = le_timer_dismiss_all(&ctx->timers);
        if (stopped)
            ctx->dirty = 1;
        snprintf(data, sizeof(data), "{\"dismissed\":%d}", stopped);
        return le_adapter_respond_ok(out, size, id, data);
    }

    return le_adapter_respond_err(out, size, id, "unknown command");
}

/* -------------------------------- server -------------------------------- */

static void close_client(struct context *ctx, int index)
{
    if (ctx->clients[index] >= 0)
        close(ctx->clients[index]);
    ctx->clients[index] = -1;
}

static void serve_client(struct context *ctx, int index)
{
    char message[LE_ADAPTER_MSG_MAX];
    char response[LE_ADAPTER_MSG_MAX];
    char cmd[64];
    char *args = NULL;
    unsigned long id = 0;
    ssize_t count;
    int length;

    count = read(ctx->clients[index], message, sizeof(message) - 1);
    if (count <= 0) {
        close_client(ctx, index);
        return;
    }
    message[count] = '\0';
    if (le_adapter_parse_request(message, cmd, sizeof(cmd), &args, &id) != 0) {
        length = le_adapter_respond_err(response, sizeof(response), id,
                                        "malformed request");
    } else {
        length = dispatch(ctx, cmd, args, id, response, sizeof(response));
    }
    if (length > 0 && write(ctx->clients[index], response, (size_t)length) < 0)
        close_client(ctx, index);
}

static void accept_client(struct context *ctx)
{
    int fd = le_adapter_accept(ctx->listen_fd);
    int i;

    if (fd < 0)
        return;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (ctx->clients[i] < 0) {
            ctx->clients[i] = fd;
            return;
        }
    }
    close(fd);
}

int main(int argc, char **argv)
{
    struct context ctx;
    struct sigaction action;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.socket_path = TIMER_SOCK;
    ctx.state_path = STATE_PATH;
    ctx.audio_sock = LE_ADAPTER_AUDIO_SOCK;
    ctx.listen_fd = -1;
    for (i = 0; i < MAX_CLIENTS; ++i)
        ctx.clients[i] = -1;

    le_log_init("timerd", argc, argv);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            ctx.socket_path = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc)
            ctx.state_path = argv[++i];
        else if (!strcmp(argv[i], "--audio-socket") && i + 1 < argc)
            ctx.audio_sock = argv[++i];
        else if (!strcmp(argv[i], "--foreground"))
            continue;
        else if (argv[i][0] == '-' && strncmp(argv[i], "--verbose", 9) &&
                 strncmp(argv[i], "--debug", 7) &&
                 strncmp(argv[i], "--quiet", 7)) {
            fprintf(stderr,
                    "usage: %s [--foreground] [--socket PATH] "
                    "[--state PATH] [--audio-socket PATH]\n", argv[0]);
            return 2;
        }
    }

    le_timer_set_init(&ctx.timers);
    state_load(&ctx);

    ctx.listen_fd = le_adapter_listen(ctx.socket_path);
    if (ctx.listen_fd < 0) {
        le_log_error("timerd: cannot listen on %s: %s", ctx.socket_path,
                     strerror(errno));
        return EXIT_FAILURE;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    le_log_info("timerd listening on %s", ctx.socket_path);

    while (!stop_requested) {
        struct pollfd fds[1 + MAX_CLIENTS];
        int slots[1 + MAX_CLIENTS];
        unsigned int fired[LE_TIMER_MAX];
        nfds_t nfds = 1;
        long long now_ms = monotonic_ms();
        long long timeout;
        int count;
        int result;
        nfds_t j;

        count = le_timer_step(&ctx.timers, now_ms, wall_epoch(), fired,
                              LE_TIMER_MAX);
        if (count > 0) {
            int k;

            for (k = 0; k < count; ++k)
                le_log_info("timerd: timer %u is due", fired[k]);
            /* Ring at once rather than waiting out a period. */
            ctx.next_ring_ms = 0;
            ctx.dirty = 1;
        }
        ring_tick(&ctx, now_ms);
        if (ctx.dirty) {
            state_save(&ctx);
            ctx.dirty = 0;
        }

        fds[0].fd = ctx.listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        slots[0] = -1;
        for (i = 0; i < MAX_CLIENTS; ++i) {
            if (ctx.clients[i] < 0)
                continue;
            fds[nfds].fd = ctx.clients[i];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            slots[nfds] = i;
            ++nfds;
        }

        timeout = le_timer_poll_timeout_ms(&ctx.timers, now_ms, wall_epoch(),
                                           POLL_CAP_MS);
        if (le_timer_ringing_count(&ctx.timers) > 0) {
            long long until_ring = ctx.next_ring_ms - now_ms;

            if (until_ring < 0)
                until_ring = 0;
            if (until_ring < timeout)
                timeout = until_ring;
        }

        result = poll(fds, nfds, (int)timeout);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            le_log_error("timerd: poll failed: %s", strerror(errno));
            break;
        }
        if (fds[0].revents & POLLIN)
            accept_client(&ctx);
        for (j = 1; j < nfds; ++j) {
            if (slots[j] >= 0 && ctx.clients[slots[j]] >= 0 &&
                (fds[j].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
                serve_client(&ctx, slots[j]);
        }
    }

    state_save(&ctx);
    for (i = 0; i < MAX_CLIENTS; ++i)
        close_client(&ctx, i);
    if (ctx.listen_fd >= 0)
        close(ctx.listen_fd);
    unlink(ctx.socket_path);
    le_log_info("timerd stopped");
    return EXIT_SUCCESS;
}
