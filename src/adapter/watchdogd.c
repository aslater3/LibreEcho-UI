/*
 * Service watchdog.
 *
 * Supervises the LibreEcho adapter daemons: probes each one's control socket,
 * and restarts a service that has stopped answering. The restart decisions
 * live in watchdog_policy.c so they can be tested without processes; this file
 * is the part that talks to sockets and runs init scripts.
 *
 * Probing the socket rather than the pid is deliberate. A daemon that is alive
 * but wedged -- blocked on a device, spinning, or holding a lock -- looks
 * perfectly healthy to a pid check while being just as useless to the rest of
 * the system. The socket is what other services actually depend on, so that is
 * what is measured.
 *
 * Deliberately not supervised: waked. It cannot be restarted at runtime --
 * micd offers its microphone stream once, so a second waked gets "microphone
 * stream: Protocol error", and stopping it unmounts the wakeword payload.
 * Restarting it would take the wake word down until the next boot, which is
 * worse than the fault being repaired. It is probed and reported, never
 * restarted.
 */

#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "watchdog_policy.h"
#include "../log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROBE_TIMEOUT_MS 1500
#define DEFAULT_INTERVAL_S 5
#define MAX_SERVICES 16

struct supervised {
    const char *name;
    const char *socket_path;
    const char *init_script;
    int restartable;
    struct le_watchdog_service state;
    int last_healthy;
    int reported_give_up;
    unsigned int total_restarts;
};

static volatile sig_atomic_t running = 1;

static void stop(int signo)
{
    (void)signo;
    running = 0;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/*
 * Healthy means the service answered "status" on its socket. Any other
 * outcome -- refused, timed out, malformed -- is a failure; the policy needs
 * two in a row before it acts, so a single blip costs nothing.
 */
static int probe(const struct supervised *service)
{
    struct le_adapter *adapter;
    char response[1024];
    int rc;

    adapter = le_adapter_connect(service->socket_path, PROBE_TIMEOUT_MS);
    if (!adapter)
        return 0;
    rc = le_adapter_call(adapter, "status", "{}", response, sizeof(response));
    le_adapter_close(adapter);
    return rc == LE_ADAPTER_OK;
}

static int run_init(const char *script, const char *action)
{
    pid_t child = fork();
    int status = 0;

    if (child < 0)
        return -1;
    if (child == 0) {
        execl("/bin/sh", "sh", script, action, (char *)NULL);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void restart(struct supervised *service)
{
    le_log_warn("watchdog: %s is not answering; restarting", service->name);
    /* Stop first and ignore its result: a wedged service often fails to stop
       cleanly, and refusing to start again because of that would leave it
       down permanently -- the opposite of the point. */
    (void)run_init(service->init_script, "stop");
    if (run_init(service->init_script, "start") != 0)
        le_log_error("watchdog: restarting %s failed", service->name);
    ++service->total_restarts;
}

int main(int argc, char **argv)
{
    static struct supervised services[MAX_SERVICES] = {
        {"networkd", "/run/libreecho/networkd.sock",
         "/etc/init.d/libreecho-networkd.init", 1, {0}, 1, 0, 0},
        {"audiod", "/run/libreecho/audio.sock",
         "/etc/init.d/libreecho-audiod.init", 1, {0}, 1, 0, 0},
        {"micd", "/run/libreecho/mic.sock",
         "/etc/init.d/libreecho-micd.init", 1, {0}, 1, 0, 0},
        {"ledd", "/run/libreecho/led.sock",
         "/etc/init.d/libreecho-ledd.init", 1, {0}, 1, 0, 0},
        {"buttond", "/run/libreecho/buttond.sock",
         "/etc/init.d/libreecho-buttond.init", 1, {0}, 1, 0, 0},
        {"btd", "/run/libreecho/bluetooth.sock",
         "/etc/init.d/libreecho-btd.init", 1, {0}, 1, 0, 0},
        {"radiod", "/run/libreecho/radio.sock",
         "/etc/init.d/libreecho-radiod.init", 1, {0}, 1, 0, 0},
        {"agentd", "/run/libreecho/agent.sock",
         "/etc/init.d/libreecho-agentd.init", 1, {0}, 1, 0, 0},
        {"ttsd", "/run/libreecho/tts.sock",
         "/etc/init.d/libreecho-ttsd.init", 1, {0}, 1, 0, 0},
        {"sttd", "/run/libreecho/stt.sock",
         "/etc/init.d/libreecho-sttd.init", 1, {0}, 1, 0, 0},
        {"airplayd", "/run/libreecho/airplay.sock",
         "/etc/init.d/libreecho-airplayd.init", 1, {0}, 1, 0, 0},
        /* Probed and reported, never restarted -- see the note at the top. */
        {"waked", "/run/libreecho/wakeword.sock", NULL, 0, {0}, 1, 0, 0},
    };
    static char argbuf[MAX_SERVICES][512];
    size_t count = 12, i;
    int interval = DEFAULT_INTERVAL_S;
    int passes = 0;   /* 0 = run forever */
    int start_delay = 0;
    int pass = 0;
    size_t custom = 0;
    long long now;

    for (i = 1; i < (size_t)argc; ++i) {
        if (!strcmp(argv[i], "--interval") && i + 1 < (size_t)argc)
            interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--foreground"))
            continue;
        /* Run a fixed number of probe passes and exit. Failure counting is
           per-process state, so a test has to drive several passes inside one
           run; separate invocations would each start from zero and never
           reach the consecutive-failure threshold. */
        else if (!strcmp(argv[i], "--passes") && i + 1 < (size_t)argc)
            passes = atoi(argv[++i]);
        /* Wait before the first pass. The supervised services are still
           starting when this one does, and a service that has not finished
           starting has not failed. */
        else if (!strcmp(argv[i], "--start-delay") && i + 1 < (size_t)argc)
            start_delay = atoi(argv[++i]);
        /* Supervise only what the caller names: NAME:SOCKET:INIT. Without
           this the service table is compiled in and the daemon cannot be
           exercised against anything but a real device. */
        else if (!strcmp(argv[i], "--service") && i + 1 < (size_t)argc) {
            char *name, *sock, *init;

            if (custom >= MAX_SERVICES) {
                fprintf(stderr, "too many --service entries\n");
                return 2;
            }
            snprintf(argbuf[custom], sizeof(argbuf[custom]), "%s", argv[++i]);
            name = argbuf[custom];
            sock = strchr(name, ':');
            if (!sock) { fprintf(stderr, "--service wants NAME:SOCKET:INIT\n"); return 2; }
            *sock++ = '\0';
            init = strchr(sock, ':');
            if (!init) { fprintf(stderr, "--service wants NAME:SOCKET:INIT\n"); return 2; }
            *init++ = '\0';
            memset(&services[custom], 0, sizeof(services[custom]));
            services[custom].name = name;
            services[custom].socket_path = sock;
            services[custom].init_script = init;
            services[custom].restartable = *init ? 1 : 0;
            services[custom].last_healthy = 1;
            ++custom;
        }
        else {
            fprintf(stderr, "usage: %s [--foreground] [--interval SECONDS]\n",
                    argv[0]);
            return 2;
        }
    }
    if (interval < 1)
        interval = DEFAULT_INTERVAL_S;
    if (custom)
        count = custom;

    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);

    now = monotonic_ms();
    for (i = 0; i < count; ++i)
        le_watchdog_service_init(&services[i].state, now);

    le_log_info("watchdog started; supervising %u services every %ds "
                "after a %ds settling delay",
                (unsigned)count, interval, start_delay);
    if (start_delay > 0) {
        sleep((unsigned int)start_delay);
        now = monotonic_ms();
        for (i = 0; i < count; ++i)
            le_watchdog_service_init(&services[i].state, now);
    }

    while (running) {
        now = monotonic_ms();
        for (i = 0; i < count && running; ++i) {
            struct supervised *s = &services[i];
            int healthy = probe(s);
            enum le_watchdog_action action;

            if (healthy != s->last_healthy) {
                if (healthy)
                    le_log_info("watchdog: %s is answering again", s->name);
                s->last_healthy = healthy;
            }
            if (!s->restartable) {
                if (!healthy)
                    le_log_warn("watchdog: %s is not answering "
                                "(not restartable; a reboot is needed)",
                                s->name);
                continue;
            }
            action = le_watchdog_step(&s->state, healthy, now);
            if (action == LE_WATCHDOG_RESTART) {
                restart(s);
                le_watchdog_restarted(&s->state, now);
            } else if (action == LE_WATCHDOG_GIVE_UP && !s->reported_give_up) {
                /* Say it once, then stay quiet rather than logging forever. */
                s->reported_give_up = 1;
                le_log_error("watchdog: giving up on %s after %u restarts; "
                             "it needs attention", s->name,
                             s->total_restarts);
            }
        }
        if (passes && ++pass >= passes)
            break;
        sleep((unsigned int)interval);
    }
    le_log_info("watchdog stopped");
    return 0;
}
