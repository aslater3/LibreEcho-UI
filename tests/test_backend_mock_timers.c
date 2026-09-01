#include "backend_internal.h"
#include "adapter/timer_schedule.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    struct le_backend backend;
    struct le_timer_list list;
    unsigned int id = 0;
    int dismissed = 0;

    memset(&backend, 0, sizeof(backend));
    assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
    assert(backend.ops->timer_add(&backend, 3, "tea", &id) == LE_OK);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1);
    assert(list.items[0].state[0] == 'p');
    assert(list.items[0].seconds_remaining <= 3);

    sleep(1);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1);
    assert(list.items[0].state[0] == 'p');
    assert(list.items[0].seconds_remaining < 3);
    assert(list.items[0].seconds_remaining > 0);

    sleep(3);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1);
    assert(list.ringing == 1);
    assert(list.items[0].state[0] == 'r');
    assert(list.items[0].seconds_remaining == 0);
    assert(backend.ops->timer_dismiss(&backend, &dismissed) == LE_OK);
    assert(dismissed == 1);

    memset(&backend, 0, sizeof(backend));
    assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
    le_mock_test_set_time(1000);
    assert(backend.ops->timer_add(&backend, 1, "expiry", &id) == LE_OK);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1 && list.items[0].state[0] == 'p');
    le_mock_test_set_time(1002);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1 && list.ringing == 1);
    le_mock_test_set_time(1002 + LE_TIMER_RING_SECONDS - 1);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1 && list.ringing == 1);
    le_mock_test_set_time(1002 + LE_TIMER_RING_SECONDS);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 0 && list.ringing == 0);
    le_mock_test_set_time(2000);
    assert(backend.ops->timer_add(&backend, 1, "missed", &id) == LE_OK);
    le_mock_test_set_time(2000 + 1 + LE_TIMER_MISS_GRACE_SECONDS + 1);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 0 && list.ringing == 0 && list.missed == 1);
    le_mock_test_set_time(3000);
    assert(backend.ops->timer_add(&backend, 1, "direct", &id) == LE_OK);
    le_mock_test_set_time(3001);
    assert(backend.ops->timer_cancel(&backend, id) == LE_INVALID);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1 && list.ringing == 1);
    backend.ops->destroy(&backend);
    le_mock_test_use_real_time();

    memset(&backend, 0, sizeof(backend));
    assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
    assert(backend.ops->timer_add(&backend, 1, "tick", &id) == LE_OK);
    sleep(2);
    assert(backend.ops->tick(&backend) == LE_OK);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1);
    assert(list.ringing == 1);
    assert(list.items[0].state[0] == 'r');
    assert(backend.ops->timer_cancel(&backend, id) == LE_INVALID);

    backend.ops->destroy(&backend);

    memset(&backend, 0, sizeof(backend));
    assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
    for (int i = 0; i < 16; ++i)
        assert(backend.ops->timer_add(&backend, 60, "capacity", &id) == LE_OK);
    assert(backend.ops->timer_add(&backend, 60, "capacity", &id) == LE_BUSY);
    backend.ops->destroy(&backend);

    /* A mock association must publish the requested SSID after its bounded
     * asynchronous completion, not retain the previous network identity. */
    {
        struct le_wifi_credentials wifi;
        struct le_network_state network;

        memset(&backend, 0, sizeof(backend));
        memset(&wifi, 0, sizeof(wifi));
        strcpy(wifi.ssid, "LibreNet-IoT");
        strcpy(wifi.security, "wpa2");
        assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
        assert(backend.ops->connect(&backend, &wifi) == LE_OK);
        sleep(3);
        assert(backend.ops->tick(&backend) == LE_OK);
        assert(backend.ops->network(&backend, &network) == LE_OK);
        assert(!strcmp(network.state, "connected"));
        assert(!strcmp(network.ssid, "LibreNet-IoT"));
        backend.ops->destroy(&backend);
    }

    puts("mock timer backend: ok");
    return 0;
}
