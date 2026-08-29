#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#define main backend_linux_test_main
#include "../src/backend_linux.c"
#undef main

#include <assert.h>
#include <stdio.h>

static int test_adapter_rc;

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
        snprintf(response, response_size,
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

    test_adapter_rc = LE_OK;
    assert(timers(NULL, &list) == LE_OK);
    assert(list.available == 1 && list.count == 0);

    puts("Linux timer status transport failures propagate: ok");
    return 0;
}
