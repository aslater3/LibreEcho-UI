#include "backend_internal.h"

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

    assert(backend.ops->timer_add(&backend, 1, "tick", &id) == LE_OK);
    sleep(2);
    assert(backend.ops->tick(&backend) == LE_OK);
    assert(backend.ops->timers(&backend, &list) == LE_OK);
    assert(list.count == 1);
    assert(list.ringing == 1);
    assert(list.items[0].state[0] == 'r');

    backend.ops->destroy(&backend);

    memset(&backend, 0, sizeof(backend));
    assert(le_mock_create(&backend, NULL, NULL, 1) == LE_OK);
    for (int i = 0; i < 16; ++i)
        assert(backend.ops->timer_add(&backend, 60, "capacity", &id) == LE_OK);
    assert(backend.ops->timer_add(&backend, 60, "capacity", &id) == LE_BUSY);
    backend.ops->destroy(&backend);

    puts("mock timer backend: ok");
    return 0;
}
