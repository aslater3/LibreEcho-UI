#ifndef LIBREECHO_NETWORK_RECOVERY_H
#define LIBREECHO_NETWORK_RECOVERY_H

/*
 * Secure recovery access-point core (issue #96).
 *
 * This unit owns the *portable* recovery lifecycle: boot-marker validation,
 * capability probing, bounded softAP child startup/teardown, per-device WPA2
 * provisioning secret handling, submission rate limiting and status
 * reporting.  It performs no HTTP work and never authenticates a user itself;
 * the captive UI and its account/CSRF checks stay in the web daemon.
 *
 * Process control is expressed through struct le_recovery_backend so the
 * lifecycle can be exercised on a host with isolated command oracles while the
 * shipped daemon uses the real fork/exec/kill/wait backend.  Children are
 * identified by pid plus a /proc start-time token; teardown signals exactly
 * those pids and never matches on a process name.
 */

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define LE_RECOVERY_MARKER_MAX   256
#define LE_RECOVERY_PSK_MAX      128
#define LE_RECOVERY_SSID_MAX     64
#define LE_RECOVERY_REASON_MAX   160
#define LE_RECOVERY_CHILD_MAX    4
#define LE_RECOVERY_MARKER_TAG   "libreecho-recovery-v1"

/* Auto-fallback window: default 120 s, bounded so a config value cannot open
 * an unbounded unattended AP. */
#define LE_RECOVERY_TIMEOUT_DEFAULT_MS 120000LL
#define LE_RECOVERY_TIMEOUT_MIN_MS      30000LL
#define LE_RECOVERY_TIMEOUT_MAX_MS     600000LL

/* Bounded startup/teardown for the AP children. */
#define LE_RECOVERY_START_DEFAULT_MS    8000LL
#define LE_RECOVERY_STOP_DEFAULT_MS     5000LL

/* Portal network the softAP serves.  The address must be applied to the
 * interface by the platform net-up helper before dnsmasq can bind and lease,
 * and removed again by net-down on teardown. */
#define LE_RECOVERY_AP_ADDRESS_DEFAULT  "192.168.4.1"
#define LE_RECOVERY_AP_PREFIX_DEFAULT   "24"

#define LE_RECOVERY_RATE_WINDOW_DEFAULT_MS 60000
#define LE_RECOVERY_RATE_MAX_DEFAULT       5

#define LE_RECOVERY_LED_OWNER "recovery-ap"

enum le_recovery_mode {
    LE_RECOVERY_MODE_CLIENT = 0,   /* normal client Wi-Fi */
    LE_RECOVERY_MODE_ARMED,        /* trigger seen, AP not started yet */
    LE_RECOVERY_MODE_STARTING,     /* children spawned, waiting for readiness */
    LE_RECOVERY_MODE_ACTIVE,       /* AP verified up and serving */
    LE_RECOVERY_MODE_HANDOVER,     /* AP released; STA association in flight */
    LE_RECOVERY_MODE_UNAVAILABLE,  /* capability/child failure, fail closed */
    LE_RECOVERY_MODE_STOPPING,
    LE_RECOVERY_MODE_STOPPED
};

enum le_recovery_trigger {
    LE_RECOVERY_TRIGGER_NONE = 0,
    LE_RECOVERY_TRIGGER_PHYSICAL,  /* root-owned tmpfs boot marker */
    LE_RECOVERY_TRIGGER_AUTO       /* opt-in watchdog fallback */
};

struct le_recovery_process {
    pid_t pid;
    unsigned long long start_token;
    int active;
    char name[16];
};

/*
 * Process/capability backend.  Tests supply isolated command oracles; the
 * shipped daemon uses le_recovery_backend_real().
 */
typedef pid_t (*le_recovery_spawn_fn)(void *ctx, char *const argv[],
                                      char *err, size_t err_size);
typedef int (*le_recovery_signal_fn)(void *ctx, pid_t pid, int sig);
typedef int (*le_recovery_wait_fn)(void *ctx, pid_t pid, int *status);
typedef int (*le_recovery_probe_fn)(void *ctx, char *const argv[]);
typedef int (*le_recovery_available_fn)(void *ctx, const char *path);
typedef unsigned long long (*le_recovery_identity_fn)(void *ctx, pid_t pid);

struct le_recovery_backend {
    le_recovery_spawn_fn spawn;
    le_recovery_signal_fn signal_child;
    le_recovery_wait_fn wait_child;
    le_recovery_probe_fn probe;
    le_recovery_available_fn available;
    le_recovery_identity_fn identity;
    void *ctx;
};

struct le_recovery_config {
    char marker_path[PATH_MAX];
    char psk_path[PATH_MAX];
    char run_dir[PATH_MAX];
    char interface[64];
    char serial_path[PATH_MAX];
    char hostapd_bin[PATH_MAX];
    char dhcp_bin[PATH_MAX];
    char dns_bin[PATH_MAX];
    char dhcp_conf[PATH_MAX];
    char hostapd_conf[PATH_MAX];
    char ap_probe_cmd[PATH_MAX];   /* AP-capable driver probe; empty = unverified */
    char ready_probe_cmd[PATH_MAX];/* optional "AP actually serving" probe */
    char net_up_cmd[PATH_MAX];     /* platform helper: release radio + set portal IP */
    char net_down_cmd[PATH_MAX];   /* platform helper: remove portal IP + restore */
    char ap_address[32];           /* portal IPv4, e.g. 192.168.4.1 */
    char led_socket[PATH_MAX];
    long long auto_timeout_ms;
    long long start_timeout_ms;
    long long stop_timeout_ms;
    int enabled;                   /* whole-feature switch; default on */
    int auto_enabled;              /* opt-in; default off */
    int rate_window_ms;
    int rate_max;
    int require_tmpfs;             /* production: 1 */
    int require_root_owner;        /* production: 1 */
};

struct le_recovery {
    struct le_recovery_config config;
    struct le_recovery_backend backend;
    enum le_recovery_mode mode;
    enum le_recovery_trigger trigger;
    int available;                 /* capability probe result */
    char unavailable_reason[LE_RECOVERY_REASON_MAX];
    char last_error[LE_RECOVERY_REASON_MAX];
    struct le_recovery_process children[LE_RECOVERY_CHILD_MAX];
    int child_count;
    int net_configured;            /* net-up succeeded; net-down still owed */
    int led_active;
    int secret_available;
    char ssid[LE_RECOVERY_SSID_MAX];
    long long armed_at_ms;
    long long start_deadline_ms;
    long long active_since_ms;
    long long last_activity_ms;
    long long auto_deadline_ms;
    long long auto_started_ms;
    long long last_now_ms;
    int auto_counting;
    long long rate_window_start_ms;
    int rate_count;
};

/* Fill a config with production defaults for the given interface. */
void le_recovery_config_default(struct le_recovery_config *config,
                                const char *interface);
/* Clamp auto_timeout_ms into the bounded range. */
void le_recovery_config_bound(struct le_recovery_config *config);
/* Real fork/exec/kill/wait backend. */
struct le_recovery_backend le_recovery_backend_real(void);

void le_recovery_init(struct le_recovery *recovery,
                      const struct le_recovery_config *config,
                      const struct le_recovery_backend *backend,
                      long long now_ms);

/*
 * Validate the boot marker.  Returns 1 when a valid marker is present, 0 when
 * no marker exists (normal boot), -1 when a marker exists but is rejected.
 * reason receives a bounded explanation when available.
 */
int le_recovery_marker_check(const struct le_recovery_config *config,
                             char *reason, size_t reason_size);

/*
 * Ensure the per-device provisioning secret exists.  Reads an existing
 * 0600 regular file (never following a symlink) or generates a fresh
 * 192-bit-class secret and stores it atomically at mode 0600.  Returns 0 on
 * success.  The secret is never logged by this unit.
 */
int le_recovery_psk_ensure(const struct le_recovery_config *config,
                           char *out, size_t out_size,
                           char *reason, size_t reason_size);

/*
 * Probe packaged capabilities and AP-capable driver support.  Returns 1 when
 * the AP can start, 0 when it cannot (reason filled), -1 on error.
 */
int le_recovery_probe_capabilities(struct le_recovery *recovery,
                                   char *reason, size_t reason_size);

/* Entry points used by networkd. */
int le_recovery_arm(struct le_recovery *recovery,
                    enum le_recovery_trigger trigger, long long now_ms);
int le_recovery_tick(struct le_recovery *recovery, long long now_ms,
                     int associated);
void le_recovery_stop(struct le_recovery *recovery, long long now_ms,
                      const char *reason);
int le_recovery_rate_limit(struct le_recovery *recovery, long long now_ms);

/*
 * Single-interface handover.  hostapd cannot keep the AP while wpa_supplicant
 * tries to associate on one radio, so the AP must release the interface first.
 * handover_begin() tears the AP children down and runs net-down (portal IPv4
 * removed, radio released) while keeping the boot marker; it returns 1 when an
 * AP was actually released.  handover_result() then either ends recovery on a
 * successful association (marker cleared) or re-arms the AP (marker kept) for
 * another attempt.
 */
int le_recovery_handover_begin(struct le_recovery *recovery, long long now_ms);
void le_recovery_handover_result(struct le_recovery *recovery, long long now_ms,
                                 int associated);

/* Status/secret serialization.  secret_json emits the provisioning secret
 * only for an owner request made while the device is NOT serving the captive
 * AP (client-connected prepare/reveal).  Callers must still gate it on owner
 * authentication + CSRF; an unauthenticated captive client must never receive
 * it.  secret_prepare ensures the secret exists without returning its value. */
int le_recovery_status_json(const struct le_recovery *recovery,
                            char *out, size_t out_size);
int le_recovery_secret_prepare(struct le_recovery *recovery,
                               char *reason, size_t reason_size);
int le_recovery_secret_json(const struct le_recovery *recovery,
                            char *out, size_t out_size);
const char *le_recovery_mode_name(enum le_recovery_mode mode);
const char *le_recovery_trigger_name(enum le_recovery_trigger trigger);

#endif /* LIBREECHO_NETWORK_RECOVERY_H */
