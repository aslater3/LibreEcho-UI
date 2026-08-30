#define _POSIX_C_SOURCE 200809L

#include "adapter/adapter.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t size)
{
    size_t used = 0;
    while (used < size) {
        ssize_t n = write(fd, data + used, size - used);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int run_server(int listener)
{
    static const char response[] =
        "{\"v\":1,\"event\":\"network.health\",\"data\":{\"state\":\"connected\"}}\n"
        "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"networks\":[]}}\n";
    char request[512];
    int client = accept(listener, NULL, NULL);
    ssize_t n;

    if (client < 0)
        return 1;
    do {
        n = read(client, request, sizeof(request));
    } while (n < 0 && errno == EINTR);
    if (n <= 0 || write_all(client, response, sizeof(response) - 1) < 0) {
        close(client);
        return 2;
    }
    close(client);
    return 0;
}

int main(void)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address;
    struct le_adapter *adapter = NULL;
    char output[256];
    int listener = -1;
    int result = 1;
    int status = 0;
    pid_t child = -1;

    if (snprintf(path, sizeof(path), "/tmp/libreecho-adapter-event-%ld.sock",
                 (long)getpid()) >= (int)sizeof(path))
        return 1;
    unlink(path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0)
        goto done;

    child = fork();
    if (child < 0)
        goto done;
    if (child == 0)
        _exit(run_server(listener));

    adapter = le_adapter_connect(path, 1000);
    if (!adapter) {
        fprintf(stderr, "adapter event test: connect failed\n");
        goto done;
    }
    if (le_adapter_call(adapter, "scan", NULL, output, sizeof(output)) !=
        LE_ADAPTER_OK) {
        fprintf(stderr,
                "adapter event test: unsolicited event replaced matching reply\n");
        goto done;
    }
    if (strcmp(output, "{\"networks\":[]}")) {
        fprintf(stderr, "adapter event test: unexpected data: %s\n", output);
        goto done;
    }
    result = 0;

done:
    le_adapter_close(adapter);
    if (child > 0) {
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            result = 1;
    }
    if (listener >= 0)
        close(listener);
    unlink(path);
    if (!result)
        puts("adapter client ignores unsolicited events: PASS");
    return result;
}
