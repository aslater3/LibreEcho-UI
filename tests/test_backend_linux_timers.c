#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#define main backend_linux_test_main
#include "../src/backend_linux.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int test_adapter_rc;
static const char *test_response;

int le_backend_linux_test_adapter_command(const char *socket_path,
                                          const char *command,
                                          const char *args,
                                          char *response,
                                          size_t response_size)
{
    (void)socket_path;
    (void)command;
    (void)args;
    if (response && response_size)
        snprintf(response, response_size, "%s", test_response ? test_response :
                 "{\"available\":true,\"ringing\":0,\"missed\":0,\"timers\":[]}");
    return test_adapter_rc;
}

int main(void)
{
    struct le_timer_list list;

    test_adapter_rc = LE_NOT_SUPPORTED;
    assert(timers(NULL, &list) == LE_OK);
    assert(list.available == 0 && list.count == 0);

    test_adapter_rc = LE_IO;
    assert(timers(NULL, &list) == LE_IO);
    {
        unsigned id = 0;
        int stopped = 0;

        assert(timer_add(NULL, 60, "tea", &id) == LE_IO);
        assert(timer_cancel(NULL, 1) == LE_IO);
        assert(timer_dismiss(NULL, &stopped) == LE_IO);
    }

    test_adapter_rc = LE_OK;
    test_response = "{\"available\":true,\"ringing\":0,\"missed\":0,\"timers\":\"bad\"}";
    assert(timers(NULL, &list) == LE_IO);
    test_response = "{\"available\":true,\"ringing\":\"0\",\"missed\":0,\"timers\":[]}";
    assert(timers(NULL, &list) == LE_IO);
    test_response = "{\"available\":true,\"ringing\":0,\"missed\":0,\"timers\":[{\"id\":1}]}";
    assert(timers(NULL, &list) == LE_IO);
    test_response = "{\"available\":true,\"ringing\":1,\"missed\":2,\"timers\":[{\"id\":1,\"kind\":\"countdown\",\"state\":\"ringing\",\"seconds_remaining\":0,\"label\":\"tea\"}]}";
    assert(timers(NULL, &list) == LE_OK);
    assert(list.available == 1 && list.count == 1 && list.ringing == 1 &&
           list.missed == 2 && !strcmp(list.items[0].label, "tea"));
    test_response = "{\"id\":7}";
    {
        unsigned id = 0;
        assert(timer_add(NULL, 60, "tea", &id) == LE_OK && id == 7);
    }
    test_response = "{\"id\":\"bad\"}";
    assert(timer_add(NULL, 60, "tea", NULL) == LE_IO);

    puts("Linux timer status transport failures propagate: ok");
    return 0;
}
