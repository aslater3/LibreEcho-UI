#include "adapter/adapter.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    struct le_adapter *adapter;
    char response[LE_ADAPTER_MSG_MAX];
    int result;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s SOCKET COMMAND [ARGS_JSON]\n",
                argv[0]);
        return 2;
    }
    adapter = le_adapter_connect(argv[1], 3000);
    if (!adapter) {
        fprintf(stderr, "unable to connect to %s\n", argv[1]);
        return 1;
    }
    result = le_adapter_call(
        adapter, argv[2], argc == 4 ? argv[3] : NULL,
        response, sizeof(response));
    le_adapter_close(adapter);
    if (result != LE_ADAPTER_OK) {
        fprintf(stderr, "adapter result=%d response=%s\n",
                result, response);
        return 1;
    }
    puts(response);
    return 0;
}
