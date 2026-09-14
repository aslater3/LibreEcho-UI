#define _POSIX_C_SOURCE 200809L
#include "mdns_lease.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/le-mdns-lease-XXXXXX", old[64], data[1024];
    struct le_mdns_lease lease;
    int fd, input;
    ssize_t size;
    assert(mkdtemp(dir));
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    le_mdns_lease_init(&lease, fd);
    assert(le_mdns_lease_register(&lease, 7, 0) < 0);
    assert(le_mdns_lease_register(&lease, 7, 65536) < 0);
    assert(le_mdns_lease_register(&lease, 7, 21000) == 0);
    input = openat(fd, lease.filename, O_RDONLY);
    assert(input >= 0);
    size = read(input, data, sizeof(data) - 1);
    assert(size > 0); data[size] = 0;
    assert(strstr(data, "<port>21000</port>"));
    assert(strstr(data, "_wyoming._tcp"));
    close(input);
    assert(le_mdns_lease_register(&lease, 8, 10700) < 0);
    assert(le_mdns_lease_withdraw(&lease, 8) < 0);
    memcpy(old, lease.filename, sizeof(old));
    assert(le_mdns_lease_register(&lease, 7, 22000) == 0);
    assert(strcmp(old, lease.filename));
    assert(faccessat(fd, old, F_OK, 0) < 0);
    assert(le_mdns_lease_withdraw(&lease, 7) == 0);
    assert(lease.owner_fd == -1);
    lease.generation = ULONG_MAX;
    assert(le_mdns_lease_register(&lease, 7, 10700) < 0);
    lease.generation = 0;
    assert(symlinkat("/no/such/target", fd, "wyoming-1.tmp") == 0);
    assert(le_mdns_lease_register(&lease, 7, 10700) < 0);
    assert(unlinkat(fd, "wyoming-1.tmp", 0) == 0);
    close(fd);
    assert(rmdir(dir) == 0);
    puts("mDNS lease: port, ownership, replacement, withdrawal, limits: ok");
    return 0;
}
