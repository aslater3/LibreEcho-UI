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
 * micd and waked are restarted together, as a group. waked is micd's only
 * microphone-stream consumer and it exits when that stream ends, so
 * restarting micd on its own would take the wake word down until the next
 * reboot -- the watchdog silently breaking the thing people most want
 * working. Restarting waked on its own does not work either: micd offers the
 * stream once, so a second waked gets "microphone stream: Protocol error".
 * Restarting both, micd first, is the one order that leaves a working wake
 * word: a fresh micd offers a fresh stream and a fresh waked takes it.
 *
 * Every service is handled as a group; most are groups of one.
 */

#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "watchdog_policy.h"
#include "../json.h"
#include "../log.h"
#include "../service_env.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROBE_TIMEOUT_MS 1500
#define DEFAULT_INTERVAL_S 5
/* Probe intervals of grace before a re-armed voice service can be
   restarted: 30 s at the default interval. */
#define REARM_GRACE_INTERVALS 6
#define MAX_SERVICES 24

/*
 * Most daemons answer on a control socket, which is the better signal: it is
 * what their callers depend on, so it catches a wedged daemon as well as a
 * dead one. Some -- buttond, timed, logd, esphomed -- serve no socket at all;
 * they read input events, poll the clock or talk outward. sttd, ttsd and
 * agentd do serve sockets, but their status path can be occupied by expected
 * voice work (recognition, synthesis or a provider response). Those three
 * deliberately use their pidfiles to avoid interrupting an active turn. It is
 * a weaker check (it cannot tell a wedged process from a working one) but it
 * still catches the common failure, which is the process being gone.
 */
enum probe_kind {
    PROBE_SOCKET,
    PROBE_PIDFILE
};

enum voice_owner {
    OWNER_ANY = 0,     /* not tied to a voice mode */
    OWNER_LOCAL,       /* wanted unless Home Assistant owns voice */
    OWNER_HA           /* wanted only while Home Assistant owns voice */
};

/* What the saved configuration says about voice ownership. */
enum voice_mode {
    MODE_UNKNOWN = 0,  /* unreadable: supervise everything, as before */
    MODE_LOCAL,
    MODE_HA
};

/* What is supervised: constant, and the thing the contract test reads. */
struct service_desc {
    const char *name;
    enum probe_kind kind;
    const char *probe_path;
    const char *init_script;
    int restartable;
    /* Services sharing a group name are stopped and started together, in
       table order. NULL means this service stands alone. */
    const char *group;
    /* Which voice mode owns this service. Local voice (sttd, ttsd, agentd)
       and the Home Assistant satellite (esphomed) hold the microphone path
       exclusively, and the web API stops one set and starts the other on a
       mode switch. Restarting a service the saved mode does not want undoes
       that switch, so such a service is not supervised until its mode is
       selected again. */
    enum voice_owner owner;
};

/* How it is going: mutable, one per descriptor. */
struct supervised {
    const struct service_desc *desc;
    struct le_watchdog_service state;
    /* Index of the first member of this service's group; its own index when
       it stands alone. All restart decisions are made on the leader. */
    size_t leader;
    int healthy;
    int last_healthy;
    int seen_healthy;
    /* Has ever answered. Survives voice-mode deselection, so supervision is
       re-armed when the mode is selected again rather than waiting for a
       daemon that may already have died to answer first. */
    int installed;
    /* Skipped because its voice mode was not selected. Selecting the mode
       while this watchdog runs is the owner asking for the service, so it
       is armed then even if it was never seen answering (booted in the
       other mode). */
    int deselected;
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

#include "../esphome_health.h"
static int probe(const struct service_desc *service)
{
    if(!strcmp(service->name,"esphomed"))
        return le_esphome_health_default(service->probe_path,"ready");
    if (service->kind == PROBE_PIDFILE)
        return probe_pidfile(service->probe_path);
    return probe_socket(service->probe_path);
}

static int run_init(const char *script, const char *action)
{
    const char *argv[4];

    /* Recovery runs another service's init script while this daemon carries
       its own ARGS (--foreground --interval ...). Without the boundary that
       argv would be resolved as the recovered service's command line, so a
       watchdog restart would take the service down instead of bringing it
       back. See src/service_env.c. */
    argv[0] = "sh";
    argv[1] = script;
    argv[2] = action;
    argv[3] = NULL;
    /* A stop request cancels the recovery rather than waiting it out: the
       caller stopping the watchdog is quiescing the services it supervises
       (the factory reset stops it first for exactly that reason), so a
       recovery this daemon launches must not outlive it -- the shell would be
       reparented and start a service the caller has already confirmed
       stopped. The request is read before the fork as well as during it, so a
       recovery that arrives after the signal is not started at all, and one
       that is already running is terminated with the work it has begun rather
       than waited out. The init script collects the recoveries it can still
       see; this covers the ones it cannot -- a fork after its snapshot -- and
       is what keeps this daemon's shutdown from ending before the work it
       started. */
    return le_service_command_cancellable("/bin/sh", argv, &running);
}

#define CONFIG_MAX 65536

/*
 * Read the voice owner from the configuration the web API persists. The rule
 * is the API's own load rule: Home Assistant owns voice when integration bit
 * 1 is set or the pipeline mode is "home-assistant". Anything unreadable is
 * MODE_UNKNOWN, never "nothing wanted": a missing or half-written file must
 * not switch supervision off.
 */
static enum voice_mode read_voice_mode(const char *path)
{
    static char text[CONFIG_MAX];
    unsigned int integrations = 0;
    char mode[32];
    int have_bits, have_mode;
    size_t n;
    FILE *file;

    if (!path || !path[0])
        return MODE_UNKNOWN;
    file = fopen(path, "r");
    if (!file)
        return MODE_UNKNOWN;
    n = fread(text, 1, sizeof(text) - 1, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        return MODE_UNKNOWN;
    }
    fclose(file);
    text[n] = '\0';
    if (!json_valid_object(text, n))
        return MODE_UNKNOWN;
    have_bits = json_get_uint(text, "integrations", &integrations) == 1;
    have_mode = json_get_string_top_level(text, "voice_pipeline_mode", mode,
                                          sizeof(mode)) > 0;
    if (!have_bits && !have_mode)
        return MODE_UNKNOWN;
    if ((have_bits && (integrations & 1u)) ||
        (have_mode && !strcmp(mode, "home-assistant")))
        return MODE_HA;
    return MODE_LOCAL;
}

static int wanted(const struct service_desc *desc, enum voice_mode mode)
{
    if (mode == MODE_UNKNOWN || desc->owner == OWNER_ANY)
        return 1;
    return desc->owner == (mode == MODE_HA ? OWNER_HA : OWNER_LOCAL);
}

static int in_group(const struct supervised *services, size_t index,
                    size_t leader)
{
    return services[index].leader == leader &&
           services[index].desc->init_script != NULL;
}

/*
 * A group is unhealthy when any member that has ever answered stops
 * answering. A member never seen answering is not counted -- it is disabled
 * or not installed, and its absence is not a fault.
 */
static int group_healthy(const struct supervised *services, size_t count,
                         size_t leader)
{
    size_t i;

    for (i = 0; i < count; ++i)
        if (services[i].leader == leader && services[i].seen_healthy &&
            !services[i].healthy)
            return 0;
    return 1;
}

static void restart_group(struct supervised *services, size_t count,
                          size_t leader, const char *config_path)
{
    size_t i;
    enum voice_mode mode;

    /* A stop request arrived before this group was reached: the caller is
       quiescing these services, and starting one now would undo that. */
    if (!running)
        return;

    /* Stop in reverse order, so a consumer is down before its producer, and
       ignore the result: a wedged service often fails to stop cleanly, and
       refusing to start again because of that would leave it down
       permanently -- the opposite of the point. */
    for (i = count; i-- > 0;)
        if (in_group(services, i, leader))
            (void)run_init(services[i].desc->init_script, "stop");

    /* The stop request can also have arrived while that ran. */
    if (!running)
        return;

    /* So can a voice-mode switch: the pass read the mode before deciding,
       and starting a member the new owner does not want would leave both
       exclusive voice owners running. */
    mode = read_voice_mode(config_path);

    for (i = 0; i < count; ++i) {
        if (!in_group(services, i, leader))
            continue;
        if (!wanted(services[i].desc, mode)) {
            le_log_info("watchdog: not restarting %s; its voice mode is no "
                        "longer selected", services[i].desc->name);
            continue;
        }
        le_log_warn("watchdog: restarting %s", services[i].desc->name);
        if (run_init(services[i].desc->init_script, "start") != 0)
            le_log_error("watchdog: restarting %s failed",
                         services[i].desc->name);
        ++services[i].total_restarts;
    }
}

int main(int argc, char **argv)
{
    static const struct service_desc descriptors[] = {
        /* Socket paths are the defaults the init scripts pass; a source
           contract test keeps this table and init/ from drifting apart. */
        {"networkd", PROBE_SOCKET, "/run/libreecho/network.sock",
         "/etc/init.d/libreecho-networkd.init", 1, NULL, OWNER_ANY},
        {"audiod", PROBE_SOCKET, "/run/libreecho/audio.sock",
         "/etc/init.d/libreecho-audiod.init", 1, NULL, OWNER_ANY},
        /* micd and waked are one unit -- see the note at the top. */
        {"micd", PROBE_SOCKET, "/run/libreecho/mic.sock",
         "/etc/init.d/libreecho-micd.init", 1, "capture", OWNER_ANY},
        {"ledd", PROBE_SOCKET, "/run/libreecho/led.sock",
         "/etc/init.d/libreecho-ledd.init", 1, NULL, OWNER_ANY},
        {"btd", PROBE_SOCKET, "/run/libreecho/bluetooth.sock",
         "/etc/init.d/libreecho-btd.init", 1, NULL, OWNER_ANY},
        {"radiod", PROBE_SOCKET, "/run/libreecho/radio.sock",
         "/etc/init.d/libreecho-radiod.init", 1, NULL, OWNER_ANY},
        /* Lived stays available while disarmed so the UI can select GPT-Live. */
        {"lived", PROBE_SOCKET, "/run/libreecho/live.sock",
         "/etc/init.d/libreecho-lived.init", 1, NULL, OWNER_ANY},
        /* Status can wait behind an in-flight provider response. */
        {"agentd", PROBE_PIDFILE, "/var/run/libreecho-agentd.pid",
         "/etc/init.d/libreecho-agentd.init", 1, NULL, OWNER_LOCAL},
        /* In-process synthesis can occupy the status path for one utterance. */
        {"ttsd", PROBE_PIDFILE, "/var/run/libreecho-ttsd.pid",
         "/etc/init.d/libreecho-ttsd.init", 1, NULL, OWNER_LOCAL},
        /* Streaming recognition owns the request loop until the turn ends. */
        {"sttd", PROBE_PIDFILE, "/var/run/libreecho-sttd.pid",
         "/etc/init.d/libreecho-sttd.init", 1, NULL, OWNER_LOCAL},
        {"mdnsd", PROBE_SOCKET, "/run/libreecho/mdns.sock",
         "/etc/init.d/libreecho-mdnsd.init", 1, NULL, OWNER_ANY},
        {"airplayd", PROBE_SOCKET, "/run/libreecho/airplay.sock",
         "/etc/init.d/libreecho-airplayd.init", 1, NULL, OWNER_ANY},
        /* Timers must recover with their durable schedule after a daemon exit. */
        {"timerd", PROBE_SOCKET, "/run/libreecho/timer.sock",
         "/etc/init.d/libreecho-timerd.init", 1, NULL, OWNER_ANY},
        /* No control socket; the pidfile is the only signal. */
        {"buttond", PROBE_PIDFILE, "/var/run/libreecho-buttond.pid",
         "/etc/init.d/libreecho-buttond.init", 1, NULL, OWNER_ANY},
        {"timed", PROBE_PIDFILE, "/var/run/libreecho-timed.pid",
         "/etc/init.d/libreecho-timed.init", 1, NULL, OWNER_ANY},
        {"logd", PROBE_PIDFILE, "/var/run/libreecho-logd.pid",
         "/etc/init.d/libreecho-logd.init", 1, NULL, OWNER_ANY},
        {"esphomed", PROBE_PIDFILE, "/var/run/libreecho-esphomed.pid",
         "/etc/init.d/libreecho-esphomed.init", 1, NULL, OWNER_HA},
        {"waked", PROBE_SOCKET, "/run/libreecho/wakeword.sock",
         "/etc/init.d/libreecho-waked.init", 1, "capture", OWNER_ANY},
    };
    static struct supervised services[MAX_SERVICES];
    static char argbuf[MAX_SERVICES][512];
    static struct service_desc custom_descs[MAX_SERVICES];
    size_t count = sizeof(descriptors) / sizeof(descriptors[0]), i;
    int interval = DEFAULT_INTERVAL_S;
    int passes = 0;   /* 0 = run forever */
    int start_delay = 0;
    int pass = 0;
    const char *config_path = getenv("LE_CONFIG_PATH");
    enum voice_mode mode = MODE_UNKNOWN, last_mode = MODE_UNKNOWN;
    size_t custom = 0;
    long long now;

    if (count > MAX_SERVICES) {
        fprintf(stderr, "service table exceeds watchdog capacity\n");
        return 2;
    }

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
        /* The configuration the voice mode is read from. */
        else if (!strcmp(argv[i], "--config") && i + 1 < (size_t)argc)
            config_path = argv[++i];
        /* Supervise only what the caller names: NAME:SOCKET:INIT. Without
           this the service table is compiled in and the daemon cannot be
           exercised against anything but a real device. */
        else if (!strcmp(argv[i], "--service") && i + 1 < (size_t)argc) {
            char *name, *sock, *init, *group;

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
            if (!init) { fprintf(stderr, "--service wants NAME:SOCKET:INIT[:GROUP]\n"); return 2; }
            *init++ = '\0';
            group = strchr(init, ':');
            if (group) {
                char *owner;

                *group++ = '\0';
                owner = strchr(group, ':');
                if (owner) {
                    *owner++ = '\0';
                    if (!strcmp(owner, "local"))
                        custom_descs[custom].owner = OWNER_LOCAL;
                    else if (!strcmp(owner, "home-assistant"))
                        custom_descs[custom].owner = OWNER_HA;
                    else if (*owner) {
                        fprintf(stderr, "--service owner must be local or "
                                "home-assistant\n");
                        return 2;
                    }
                }
                if (!*group)
                    group = NULL;
            }
            custom_descs[custom].name = name;
            custom_descs[custom].kind = PROBE_SOCKET;
            custom_descs[custom].probe_path = sock;
            custom_descs[custom].init_script = init;
            custom_descs[custom].restartable = *init ? 1 : 0;
            custom_descs[custom].group = group;
            ++custom;
        }
        else {
            fprintf(stderr, "usage: %s [--foreground] [--interval SECONDS] "
                    "[--config PATH]\n", argv[0]);
            return 2;
        }
    }
    if (interval < 1)
        interval = DEFAULT_INTERVAL_S;
    if (!config_path || !config_path[0])
        config_path = "/data/libreecho/config/web-config.json";
    if (custom)
        count = custom;
    for (i = 0; i < count; ++i) {
        size_t j;

        services[i].desc = custom ? &custom_descs[i] : &descriptors[i];
        services[i].leader = i;
        for (j = 0; j < i; ++j) {
            const char *mine = services[i].desc->group;
            const char *theirs = services[j].desc->group;

            if (mine && theirs && !strcmp(mine, theirs)) {
                services[i].leader = services[j].leader;
                break;
            }
        }
    }

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
        mode = read_voice_mode(config_path);
        if (mode != last_mode) {
            le_log_info("watchdog: voice is owned by %s",
                        mode == MODE_HA ? "Home Assistant" :
                        mode == MODE_LOCAL ? "the local pipeline" :
                        "an unknown mode (supervising all)");
            last_mode = mode;
        }

        /* Probe everything first, then decide. A group has to be judged on
           one sweep, or a member restarted mid-pass looks like a second
           fault. */
        for (i = 0; i < count && running; ++i) {
            struct supervised *s = &services[i];

            /* A service the saved voice mode does not want was stopped by
               the API on purpose. Drop its supervision latch and failure
               history, so it is neither restarted now nor treated as a crash
               when its mode is selected again and the API starts it. */
            if (wanted(s->desc, mode) && (s->installed || s->deselected) &&
                !s->seen_healthy) {
                s->seen_healthy = 1;
                s->deselected = 0;
                s->last_healthy = 1;
                s->reported_give_up = 0;
                le_watchdog_service_init(&s->state, now);
                /* The API is starting it now; give a model-loading daemon
                   time to answer before a restart can stop it again. */
                s->state.next_attempt_ms =
                    now + (long long)interval * REARM_GRACE_INTERVALS * 1000;
                le_log_info("watchdog: supervising %s again; its voice mode "
                            "is selected", s->desc->name);
            }
            if (!wanted(s->desc, mode)) {
                if (s->seen_healthy)
                    le_log_info("watchdog: not supervising %s; its voice "
                                "mode is not selected", s->desc->name);
                s->seen_healthy = 0;
                s->deselected = 1;
                s->healthy = 0;
                s->last_healthy = 1;
                s->reported_give_up = 0;
                le_watchdog_service_init(&s->state, now);
                continue;
            }

            s->healthy = probe(s->desc);

            /* Supervision latches on the first healthy probe. A daemon that
               has never answered is either disabled or not installed, and
               starting something the owner turned off is not recovery. */
            if (s->healthy)
                s->installed = 1;
            if (s->healthy && !s->seen_healthy) {
                s->seen_healthy = 1;
                le_log_info("watchdog: supervising %s", s->desc->name);
            }
            if (s->healthy != s->last_healthy) {
                if (s->healthy && s->seen_healthy)
                    le_log_info("watchdog: %s is answering again",
                                s->desc->name);
                else if (!s->healthy && s->seen_healthy &&
                         !s->desc->restartable)
                    le_log_warn("watchdog: %s is not answering "
                                "(not restartable; a reboot is needed)",
                                s->desc->name);
                s->last_healthy = s->healthy;
            }
        }

        for (i = 0; i < count && running; ++i) {
            struct supervised *s = &services[i];
            enum le_watchdog_action action;
            int healthy;

            /* Members are acted on through their group's leader. */
            if (s->leader != i || !s->seen_healthy)
                continue;

            healthy = group_healthy(services, count, i);
            /* Reported on the health transition above, so a service that
               cannot be restarted does not refill the log every pass. */
            if (!s->desc->restartable)
                continue;
            action = le_watchdog_step(&s->state, healthy, now);
            if (action == LE_WATCHDOG_RESTART) {
                restart_group(services, count, i, config_path);
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
