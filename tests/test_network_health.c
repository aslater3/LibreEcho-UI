#include "adapter/network_health.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static struct le_network_health_config fast_config(void)
{
    struct le_network_health_config config;

    config.probe_interval_ms = 1000;
    config.failure_threshold = 3;
    config.reassociate_grace_ms = 2000;
    config.interface_down_ms = 500;
    config.interface_grace_ms = 3000;
    return config;
}

static int test_healthy_gateway_is_probed_periodically(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();

    le_network_health_init(&health, &config, 0);
    CHECK(le_network_health_tick(&health, 0, 1, 1) == LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 0, LE_GATEWAY_REACHABLE) ==
          LE_NETWORK_HEALTH_NONE);
    CHECK(!strcmp(le_network_health_connectivity(&health), "healthy"));
    CHECK(le_network_health_gateway_reachable(&health) == 1);
    CHECK(le_network_health_tick(&health, 999, 1, 1) == LE_NETWORK_HEALTH_NONE);
    CHECK(le_network_health_tick(&health, 1000, 1, 1) == LE_NETWORK_HEALTH_PROBE);
    return 0;
}

static int test_sustained_failure_reassociates_then_recovers_without_reboot(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();
    long long now = 0;
    int failure;

    le_network_health_init(&health, &config, now);
    CHECK(le_network_health_tick(&health, now, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, now,
              LE_GATEWAY_REACHABLE) == LE_NETWORK_HEALTH_NONE);
    now += config.probe_interval_ms;
    for (failure = 1; failure <= 3; ++failure) {
        enum le_network_health_action expected = failure == 3 ?
            LE_NETWORK_HEALTH_REASSOCIATE : LE_NETWORK_HEALTH_NONE;
        CHECK(le_network_health_tick(&health, now, 1, 1) ==
              LE_NETWORK_HEALTH_PROBE);
        CHECK(le_network_health_record_probe(
                  &health, now, LE_GATEWAY_UNREACHABLE) == expected);
        now += failure == 3 ? config.reassociate_grace_ms :
                             config.probe_interval_ms;
    }
    CHECK(!strcmp(le_network_health_connectivity(&health), "recovering"));
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "reassociate"));
    CHECK(le_network_health_tick(&health, now, 1, 1) == LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, now, LE_GATEWAY_REACHABLE) ==
          LE_NETWORK_HEALTH_NONE);
    CHECK(!strcmp(le_network_health_connectivity(&health), "healthy"));
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "none"));
    CHECK(le_network_health_consecutive_failures(&health) == 0);
    CHECK(le_network_health_reboot_requested(&health) == 0);
    return 0;
}

static int test_persistent_failure_has_bounded_ordered_escalation(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();
    long long now = 0;
    int failure;

    le_network_health_init(&health, &config, now);
    CHECK(le_network_health_tick(&health, now, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, now,
              LE_GATEWAY_REACHABLE) == LE_NETWORK_HEALTH_NONE);
    now += config.probe_interval_ms;
    for (failure = 1; failure <= 3; ++failure) {
        CHECK(le_network_health_tick(&health, now, 1, 1) ==
              LE_NETWORK_HEALTH_PROBE);
        CHECK(le_network_health_record_probe(&health, now,
                  LE_GATEWAY_UNREACHABLE) ==
              (failure == 3 ? LE_NETWORK_HEALTH_REASSOCIATE :
                              LE_NETWORK_HEALTH_NONE));
        now += failure == 3 ? config.reassociate_grace_ms :
                             config.probe_interval_ms;
    }

    CHECK(le_network_health_tick(&health, now, 0, 0) ==
          LE_NETWORK_HEALTH_INTERFACE_DOWN);
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "interface-reset"));

    CHECK(le_network_health_tick(&health, now + config.interface_down_ms - 1,
                                  0, 0) == LE_NETWORK_HEALTH_NONE);
    now += config.interface_down_ms;
    CHECK(le_network_health_tick(&health, now, 0, 0) ==
          LE_NETWORK_HEALTH_INTERFACE_UP);

    now += config.interface_grace_ms;
    CHECK(le_network_health_tick(&health, now, 0, 0) ==
          LE_NETWORK_HEALTH_REQUEST_REBOOT);
    CHECK(!strcmp(le_network_health_recovery_stage(&health),
                  "reboot-pending"));
    CHECK(le_network_health_reboot_requested(&health) == 0);
    le_network_health_finish_reboot_request(&health, 1);
    CHECK(!strcmp(le_network_health_recovery_stage(&health),
                  "reboot-requested"));
    CHECK(le_network_health_reboot_requested(&health) == 1);
    CHECK(le_network_health_tick(&health, now + 60000, 1, 1) ==
          LE_NETWORK_HEALTH_NONE);
    CHECK(le_network_health_reboot_requested(&health) == 1);
    return 0;
}

static int test_missing_gateway_route_uses_bounded_recovery(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();
    enum le_network_health_action action = LE_NETWORK_HEALTH_NONE;
    long long now = 0;
    int failure;

    le_network_health_init(&health, &config, now);
    CHECK(le_network_health_tick(&health, now, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, now,
              LE_GATEWAY_REACHABLE) == LE_NETWORK_HEALTH_NONE);

    for (failure = 1; failure <= config.failure_threshold; ++failure) {
        now += config.probe_interval_ms;
        action = le_network_health_tick(&health, now, 1, 0);
        CHECK(action == (failure == config.failure_threshold ?
              LE_NETWORK_HEALTH_REASSOCIATE : LE_NETWORK_HEALTH_NONE));
    }
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "reassociate"));
    return 0;
}

static int test_unproven_gateway_never_triggers_recovery(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();
    long long now = 0;
    int failure;

    le_network_health_init(&health, &config, now);
    for (failure = 0; failure < 6; ++failure) {
        CHECK(le_network_health_tick(&health, now, 1, 1) ==
              LE_NETWORK_HEALTH_PROBE);
        CHECK(le_network_health_record_probe(&health, now,
                  LE_GATEWAY_UNREACHABLE) == LE_NETWORK_HEALTH_NONE);
        now += config.probe_interval_ms;
    }
    CHECK(!strcmp(le_network_health_connectivity(&health), "degraded"));
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "none"));
    CHECK(le_network_health_reboot_requested(&health) == 0);
    return 0;
}

static int test_unproven_failures_saturate_at_threshold(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();
    long long now = 0;
    int failure;

    le_network_health_init(&health, &config, now);
    for (failure = 0; failure < 100; ++failure) {
        CHECK(le_network_health_tick(&health, now, 1, 1) ==
              LE_NETWORK_HEALTH_PROBE);
        CHECK(le_network_health_record_probe(&health, now,
                  LE_GATEWAY_UNREACHABLE) == LE_NETWORK_HEALTH_NONE);
        now += config.probe_interval_ms;
    }
    CHECK(le_network_health_consecutive_failures(&health) ==
          config.failure_threshold);
    return 0;
}

static int test_exhausted_state_can_observe_later_recovery(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();

    le_network_health_init(&health, &config, 0);
    health.associated = 1;
    health.phase = LE_NETWORK_HEALTH_REBOOT_PENDING;
    le_network_health_finish_reboot_request(&health, 0);
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "exhausted"));
    CHECK(le_network_health_tick(&health, 0, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 0,
              LE_GATEWAY_REACHABLE) == LE_NETWORK_HEALTH_NONE);
    CHECK(!strcmp(le_network_health_connectivity(&health), "healthy"));
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "none"));
    return 0;
}

static int test_disconnect_and_probe_error_never_trigger_recovery(void)
{
    struct le_network_health health;
    struct le_network_health_config config = fast_config();

    le_network_health_init(&health, &config, 0);
    CHECK(le_network_health_tick(&health, 0, 1, 1) == LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 0, LE_GATEWAY_PROBE_UNAVAILABLE) ==
          LE_NETWORK_HEALTH_NONE);
    CHECK(!strcmp(le_network_health_connectivity(&health), "unknown"));
    CHECK(le_network_health_consecutive_failures(&health) == 0);
    CHECK(le_network_health_tick(&health, 1000, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 1000,
              LE_GATEWAY_REACHABLE) == LE_NETWORK_HEALTH_NONE);
    CHECK(le_network_health_tick(&health, 2000, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 2000,
              LE_GATEWAY_UNREACHABLE) == LE_NETWORK_HEALTH_NONE);
    CHECK(le_network_health_consecutive_failures(&health) == 1);
    CHECK(le_network_health_tick(&health, 3000, 1, 1) ==
          LE_NETWORK_HEALTH_PROBE);
    CHECK(le_network_health_record_probe(&health, 3000,
              LE_GATEWAY_PROBE_UNAVAILABLE) == LE_NETWORK_HEALTH_NONE);
    CHECK(le_network_health_consecutive_failures(&health) == 0);
    CHECK(le_network_health_tick(&health, 4000, 0, 0) ==
          LE_NETWORK_HEALTH_NONE);
    CHECK(!strcmp(le_network_health_connectivity(&health), "disconnected"));
    CHECK(!strcmp(le_network_health_recovery_stage(&health), "none"));
    CHECK(le_network_health_reboot_requested(&health) == 0);
    return 0;
}

int main(void)
{
    if (test_healthy_gateway_is_probed_periodically() ||
        test_sustained_failure_reassociates_then_recovers_without_reboot() ||
        test_persistent_failure_has_bounded_ordered_escalation() ||
        test_missing_gateway_route_uses_bounded_recovery() ||
        test_unproven_gateway_never_triggers_recovery() ||
        test_unproven_failures_saturate_at_threshold() ||
        test_exhausted_state_can_observe_later_recovery() ||
        test_disconnect_and_probe_error_never_trigger_recovery())
        return 1;
    puts("network health policy: ok");
    return 0;
}
