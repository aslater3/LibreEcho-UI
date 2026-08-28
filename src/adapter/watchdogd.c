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
 * A service is supervised only after it has been seen answering at least
 * once. Some daemons are optional or disabled by configuration, and starting
 * something the owner turned off is not recovery. Latching on the first
 * healthy probe also means a daemon that takes longer than the settling delay
 * to come up is picked up when it arrives rather than written off.
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
#define MAX_SERVICES 24

/*
 * Most daemons answer on a control socket, which is the better signal: it is
 * what their callers depend on, so it catches a wedged daemon as well as a
 * dead one. Some -- buttond, timed, logd, wyomingd -- serve no socket at all;
 * they read input events, poll the clock or talk outward. For those the
 * pidfile is all there is. It is a weaker check (it cannot tell a wedged
 * process from a working one) but it still catches the common failure, which
 * is the process being gone.
 */
enum probe_kind {
    PROBE_SOCKET,
    PROBE_PIDFILE
};

/* What is supervised: constant, and the thing the contract test reads. */
struct service_desc {
    const char *name;
    enum probe_kind kind;
    const char *probe_path;
    const char *init_script;
    int restartable;
};

/* How it is going: mutable, one per descriptor. */
struct supervised {
    const struct service_desc *desc;
    struct le_watchdog_service state;
    int last_healthy;
    int seen_healthy;
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
static int probe_socket(const char *path)
{
    struct le_adapter *adapter;
    char response[1024];
    int rc;

    adapter = le_adapter_connect(path, PROBE_TIMEOUT_MS);
    if (!adapter)
        return 0;
    rc = le_adapter_call(adapter, "status", "{}", response, sizeof(response));
    le_adapter_close(adapter);
    return rc == LE_ADAPTER_OK;
}

static int probe_pidfile(const char *path)
{
    char line[64];
    FILE *file = fopen(path, "r");
    long pid;
    char *end;

    if (!file)
        return 0;
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    pid = strtol(line, &end, 10);
    if (end == line || pid <= 0)
        return 0;
    return kill((pid_t)pid, 0) == 0;
}

static int probe(const struct service_desc *service)
{
    if (service->kind == PROBE_PIDFILE)
        return probe_pidfile(service->probe_path);
    return probe_socket(service->probe_path);
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
    le_log_warn("watchdog: %s is not answering; restarting",
                service->desc->name);
    /* Stop first and ignore its result: a wedged service often fails to stop
       cleanly, and refusing to start again because of that would leave it
       down permanently -- the opposite of the point. */
    (void)run_init(service->desc->init_script, "stop");
    if (run_init(service->desc->init_script, "start") != 0)
        le_log_error("watchdog: restarting %s failed", service->desc->name);
    ++service->total_restarts;
}

int main(int argc, char **argv)
{
    static const struct service_desc descriptors[MAX_SERVICES] = {
        /* Socket paths are the defaults the init scripts pass; a source
           contract test keeps this table and init/ from drifting apart. */
        {"networkd", PROBE_SOCKET, "/run/libreecho/network.sock",
         "/etc/init.d/libreecho-networkd.init", 1},
        {"audiod", PROBE_SOCKET, "/run/libreecho/audio.sock",
         "/etc/init.d/libreecho-audiod.init", 1},
        {"micd", PROBE_SOCKET, "/run/libreecho/mic.sock",
         "/etc/init.d/libreecho-micd.init", 1},
        {"ledd", PROBE_SOCKET, "/run/libreecho/led.sock",
         "/etc/init.d/libreecho-ledd.init", 1},
        {"btd", PROBE_SOCKET, "/run/libreecho/bluetooth.sock",
         "/etc/init.d/libreecho-btd.init", 1},
        {"radiod", PROBE_SOCKET, "/run/libreecho/radio.sock",
         "/etc/init.d/libreecho-radiod.init", 1},
        {"agentd", PROBE_SOCKET, "/run/libreecho/agent.sock",
         "/etc/init.d/libreecho-agentd.init", 1},
        {"ttsd", PROBE_SOCKET, "/run/libreecho/tts.sock",
         "/etc/init.d/libreecho-ttsd.init", 1},
        {"sttd", PROBE_SOCKET, "/run/libreecho/stt.sock",
         "/etc/init.d/libreecho-sttd.init", 1},
        {"airplayd", PROBE_SOCKET, "/run/libreecho/airplay.sock",
         "/etc/init.d/libreecho-airplayd.init", 1},
        /* No control socket; the pidfile is the only signal. */
        {"buttond", PROBE_PIDFILE, "/var/run/libreecho-buttond.pid",
         "/etc/init.d/libreecho-buttond.init", 1},
        {"timed", PROBE_PIDFILE, "/var/run/libreecho-timed.pid",
         "/etc/init.d/libreecho-timed.init", 1},
        {"logd", PROBE_PIDFILE, "/var/run/libreecho-logd.pid",
         "/etc/init.d/libreecho-logd.init", 1},
        {"wyomingd", PROBE_PIDFILE, "/var/run/libreecho-wyomingd.pid",
         "/etc/init.d/libreecho-wyomingd.init", 1},
        /* Probed and reported, never restarted -- see the note at the top. */
        {"waked", PROBE_SOCKET, "/run/libreecho/wakeword.sock", NULL, 0},
    };
    static struct supervised services[MAX_SERVICES];
    static char argbuf[MAX_SERVICES][512];
    static struct service_desc custom_descs[MAX_SERVICES];
    size_t count = 15, i;
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
            custom_descs[custom].name = name;
            custom_descs[custom].kind = PROBE_SOCKET;
            custom_descs[custom].probe_path = sock;
            custom_descs[custom].init_script = init;
            custom_descs[custom].restartable = *init ? 1 : 0;
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
    for (i = 0; i < count; ++i)
        services[i].desc = custom ? &custom_descs[i] : &descriptors[i];

    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);

    now = monotonic_ms();
    for (i = 0; i < count; ++i) {
        le_watchdog_service_init(&services[i].state, now);
        services[i].last_healthy = 1;
    }

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
            int healthy = probe(s->desc);
            enum le_watchdog_action action;

            /* Supervision latches on the first healthy probe. A daemon that
               has never answered is either disabled or not installed, and
               starting something the owner turned off is not recovery. */
            if (healthy && !s->seen_healthy) {
                s->seen_healthy = 1;
                le_log_info("watchdog: supervising %s", s->desc->name);
            }
            if (healthy != s->last_healthy) {
                if (healthy && s->seen_healthy)
                    le_log_info("watchdog: %s is answering again", s->desc->name);
                else if (!healthy && s->seen_healthy && !s->desc->restartable)
                    le_log_warn("watchdog: %s is not answering "
                                "(not restartable; a reboot is needed)",
                                s->desc->name);
                s->last_healthy = healthy;
            }
            if (!s->seen_healthy)
                continue;
            if (!s->desc->restartable)
                continue;
            action = le_watchdog_step(&s->state, healthy, now);
            if (action == LE_WATCHDOG_RESTART) {
                restart(s);
                le_watchdog_restarted(&s->state, now);
            } else if (action == LE_WATCHDOG_GIVE_UP && !s->reported_give_up) {
                /* Say it once, then stay quiet rather than logging forever. */
                s->reported_give_up = 1;
                le_log_error("watchdog: giving up on %s after %u restarts; "
                             "it needs attention", s->desc->name,
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
