#define _POSIX_C_SOURCE 200809L
#include "adapter/playback_status_client.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static int replace(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ssize_t length = (ssize_t)strlen(text);
    int ok = fd >= 0 && write(fd, text, (size_t)length) == length;
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}

int main(void)
{
    char path[128];

    snprintf(path, sizeof(path), "/tmp/le-playback-status-%d", (int)getpid());
    CHECK(replace(path, "{\"buses\":{\"system\":false},\"drain\":{\"system\":{\"drained\":false}}}") == 0);
    CHECK(le_playback_status_bus_drained(path, "system") == 0);
    CHECK(replace(path, "{\"buses\":{\"system\":true},\"drain\":{\"system\":{\"drained\":true}}}") == 0);
    CHECK(le_playback_status_bus_drained(path, "system") == 1);
    CHECK(replace(path, "{\"buses\":{\"announcement\":false}}") == 0);
    CHECK(le_playback_status_bus_drained(path, "announcement") == 1);
    CHECK(le_playback_status_bus_drained(path, "media") == -1);
    unlink(path);
    puts("playback status client: ok");
    return 0;
}
