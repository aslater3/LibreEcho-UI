#include "network_health.h"

#include <string.h>

#ifdef LE_NETWORKD_TESTING
#define DEFAULT_PROBE_INTERVAL_MS 20LL
#define DEFAULT_FAILURE_THRESHOLD 3
#define DEFAULT_REASSOCIATE_GRACE_MS 40LL
#define DEFAULT_INTERFACE_DOWN_MS 10LL
#define DEFAULT_INTERFACE_GRACE_MS 40LL
#else
#define DEFAULT_PROBE_INTERVAL_MS 15000LL
#define DEFAULT_FAILURE_THRESHOLD 3
#define DEFAULT_REASSOCIATE_GRACE_MS 30000LL
#define DEFAULT_INTERFACE_DOWN_MS 1000LL
#define DEFAULT_INTERFACE_GRACE_MS 45000LL
#endif

void le_network_health_default_config(struct le_network_health_config *config)
{
    if (!config)
        return;
    config->probe_interval_ms = DEFAULT_PROBE_INTERVAL_MS;
    config->failure_threshold = DEFAULT_FAILURE_THRESHOLD;
    config->reassociate_grace_ms = DEFAULT_REASSOCIATE_GRACE_MS;
    config->interface_down_ms = DEFAULT_INTERFACE_DOWN_MS;
    config->interface_grace_ms = DEFAULT_INTERFACE_GRACE_MS;
}

void le_network_health_init(struct le_network_health *health,
                            const struct le_network_health_config *config,
                            long long now_ms)
{
    struct le_network_health_config defaults;

    if (!health)
        return;
    le_network_health_default_config(&defaults);
    memset(health, 0, sizeof(*health));
    health->config = config ? *config : defaults;
    if (health->config.probe_interval_ms <= 0)
        health->config.probe_interval_ms = defaults.probe_interval_ms;
    if (health->config.failure_threshold <= 0)
        health->config.failure_threshold = defaults.failure_threshold;
    if (health->config.reassociate_grace_ms <= 0)
        health->config.reassociate_grace_ms = defaults.reassociate_grace_ms;
    if (health->config.interface_down_ms <= 0)
        health->config.interface_down_ms = defaults.interface_down_ms;
    if (health->config.interface_grace_ms <= 0)
        health->config.interface_grace_ms = defaults.interface_grace_ms;
    health->phase = LE_NETWORK_HEALTH_IDLE;
    health->next_probe_ms = now_ms;
    health->gateway_reachable = -1;
}

static void mark_disconnected(struct le_network_health *health,
                              long long now_ms)
{
    health->phase = LE_NETWORK_HEALTH_IDLE;
    health->next_probe_ms = now_ms + health->config.probe_interval_ms;
    health->transition_deadline_ms = 0;
    health->associated = 0;
    health->awaiting_probe = 0;
    health->gateway_reachable = -1;
    health->consecutive_failures = 0;
    health->healthy_seen = 0;
}

enum le_network_health_action le_network_health_tick(
    struct le_network_health *health, long long now_ms, int associated,
    int gateway_route_available)
{
    if (!health)
        return LE_NETWORK_HEALTH_NONE;
    if (health->phase == LE_NETWORK_HEALTH_REBOOT_PENDING ||
        health->phase == LE_NETWORK_HEALTH_REBOOT_REQUESTED)
        return LE_NETWORK_HEALTH_NONE;
    if (health->phase == LE_NETWORK_HEALTH_INTERFACE_DOWN_WAIT) {
        if (now_ms < health->transition_deadline_ms)
            return LE_NETWORK_HEALTH_NONE;
        health->phase = LE_NETWORK_HEALTH_INTERFACE_RECOVERING;
        health->next_probe_ms = now_ms + health->config.interface_grace_ms;
        return LE_NETWORK_HEALTH_INTERFACE_UP;
    }
    if (health->phase != LE_NETWORK_HEALTH_REASSOCIATING &&
        health->phase != LE_NETWORK_HEALTH_INTERFACE_RECOVERING &&
        !associated) {
        mark_disconnected(health, now_ms);
        return LE_NETWORK_HEALTH_NONE;
    }
    if (associated && !health->associated) {
        health->associated = 1;
        health->next_probe_ms = now_ms;
    }
    if (health->awaiting_probe || now_ms < health->next_probe_ms)
        return LE_NETWORK_HEALTH_NONE;
    if (!gateway_route_available)
        return le_network_health_record_probe(
            health, now_ms, LE_GATEWAY_UNREACHABLE);
    health->awaiting_probe = 1;
    return LE_NETWORK_HEALTH_PROBE;
}

enum le_network_health_action le_network_health_record_probe(
    struct le_network_health *health, long long now_ms,
    enum le_gateway_probe_result result)
{
    if (!health)
        return LE_NETWORK_HEALTH_NONE;
    health->awaiting_probe = 0;
    if (result == LE_GATEWAY_PROBE_UNAVAILABLE) {
        health->gateway_reachable = -1;
        health->consecutive_failures = 0;
        health->next_probe_ms = now_ms + health->config.probe_interval_ms;
        return LE_NETWORK_HEALTH_NONE;
    }
    if (result == LE_GATEWAY_REACHABLE) {
        health->gateway_reachable = 1;
        health->consecutive_failures = 0;
        health->healthy_seen = 1;
        health->phase = LE_NETWORK_HEALTH_IDLE;
        health->transition_deadline_ms = 0;
        health->next_probe_ms = now_ms + health->config.probe_interval_ms;
        return LE_NETWORK_HEALTH_NONE;
    }

    health->gateway_reachable = 0;
    if (health->consecutive_failures < health->config.failure_threshold)
        ++health->consecutive_failures;
    health->next_probe_ms = now_ms + health->config.probe_interval_ms;
    if (health->phase == LE_NETWORK_HEALTH_IDLE && health->healthy_seen &&
        health->consecutive_failures >= health->config.failure_threshold) {
        health->phase = LE_NETWORK_HEALTH_REASSOCIATING;
        health->next_probe_ms = now_ms + health->config.reassociate_grace_ms;
        return LE_NETWORK_HEALTH_REASSOCIATE;
    }
    if (health->phase == LE_NETWORK_HEALTH_REASSOCIATING) {
        health->phase = LE_NETWORK_HEALTH_INTERFACE_DOWN_WAIT;
        health->transition_deadline_ms =
            now_ms + health->config.interface_down_ms;
        return LE_NETWORK_HEALTH_INTERFACE_DOWN;
    }
    if (health->phase == LE_NETWORK_HEALTH_INTERFACE_RECOVERING) {
        health->phase = LE_NETWORK_HEALTH_REBOOT_PENDING;
        health->next_probe_ms = 0;
        return LE_NETWORK_HEALTH_REQUEST_REBOOT;
    }
    return LE_NETWORK_HEALTH_NONE;
}

const char *le_network_health_connectivity(
    const struct le_network_health *health)
{
    if (!health || !health->associated)
        return "disconnected";
    if (health->phase != LE_NETWORK_HEALTH_IDLE)
        return "recovering";
    if (health->gateway_reachable > 0)
        return "healthy";
    if (health->gateway_reachable == 0 || health->consecutive_failures > 0)
        return "degraded";
    return "unknown";
}

const char *le_network_health_recovery_stage(
    const struct le_network_health *health)
{
    if (!health)
        return "none";
    if (health->phase == LE_NETWORK_HEALTH_REASSOCIATING)
        return "reassociate";
    if (health->phase == LE_NETWORK_HEALTH_INTERFACE_DOWN_WAIT ||
        health->phase == LE_NETWORK_HEALTH_INTERFACE_RECOVERING)
        return "interface-reset";
    if (health->phase == LE_NETWORK_HEALTH_REBOOT_PENDING)
        return "reboot-pending";
    if (health->phase == LE_NETWORK_HEALTH_REBOOT_REQUESTED)
        return "reboot-requested";
    if (health->phase == LE_NETWORK_HEALTH_EXHAUSTED)
        return "exhausted";
    return "none";
}

int le_network_health_gateway_reachable(
    const struct le_network_health *health)
{
    return health ? health->gateway_reachable : -1;
}

int le_network_health_consecutive_failures(
    const struct le_network_health *health)
{
    return health ? health->consecutive_failures : 0;
}

int le_network_health_reboot_requested(
    const struct le_network_health *health)
{
    return health && health->phase == LE_NETWORK_HEALTH_REBOOT_REQUESTED;
}

void le_network_health_finish_reboot_request(
    struct le_network_health *health, int submitted)
{
    if (!health || health->phase != LE_NETWORK_HEALTH_REBOOT_PENDING)
        return;
    health->phase = submitted ? LE_NETWORK_HEALTH_REBOOT_REQUESTED :
                                LE_NETWORK_HEALTH_EXHAUSTED;
}
