#ifndef LIBREECHO_NETWORK_HEALTH_H
#define LIBREECHO_NETWORK_HEALTH_H

struct le_network_health_config {
    long long probe_interval_ms;
    int failure_threshold;
    long long reassociate_grace_ms;
    long long interface_down_ms;
    long long interface_grace_ms;
};

enum le_gateway_probe_result {
    LE_GATEWAY_PROBE_UNAVAILABLE = -1,
    LE_GATEWAY_UNREACHABLE = 0,
    LE_GATEWAY_REACHABLE = 1
};

enum le_network_health_action {
    LE_NETWORK_HEALTH_NONE = 0,
    LE_NETWORK_HEALTH_PROBE,
    LE_NETWORK_HEALTH_REASSOCIATE,
    LE_NETWORK_HEALTH_INTERFACE_DOWN,
    LE_NETWORK_HEALTH_INTERFACE_UP,
    LE_NETWORK_HEALTH_REQUEST_REBOOT
};

enum le_network_health_phase {
    LE_NETWORK_HEALTH_IDLE = 0,
    LE_NETWORK_HEALTH_REASSOCIATING,
    LE_NETWORK_HEALTH_INTERFACE_DOWN_WAIT,
    LE_NETWORK_HEALTH_INTERFACE_RECOVERING,
    LE_NETWORK_HEALTH_REBOOT_REQUESTED
};

struct le_network_health {
    struct le_network_health_config config;
    enum le_network_health_phase phase;
    long long next_probe_ms;
    long long transition_deadline_ms;
    int associated;
    int awaiting_probe;
    int gateway_reachable;
    int consecutive_failures;
    int healthy_seen;
};

void le_network_health_default_config(struct le_network_health_config *config);
void le_network_health_init(struct le_network_health *health,
                            const struct le_network_health_config *config,
                            long long now_ms);
enum le_network_health_action le_network_health_tick(
    struct le_network_health *health, long long now_ms, int associated,
    int gateway_route_available);
enum le_network_health_action le_network_health_record_probe(
    struct le_network_health *health, long long now_ms,
    enum le_gateway_probe_result result);
const char *le_network_health_connectivity(
    const struct le_network_health *health);
const char *le_network_health_recovery_stage(
    const struct le_network_health *health);
int le_network_health_gateway_reachable(
    const struct le_network_health *health);
int le_network_health_consecutive_failures(
    const struct le_network_health *health);
int le_network_health_reboot_requested(
    const struct le_network_health *health);

#endif
