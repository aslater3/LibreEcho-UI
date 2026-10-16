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
    char dir[512], old[64], data[4096];
    const char *tmp = getenv("TMPDIR");
    struct le_mdns_esphome_metadata metadata = {
        "2026.9.0", "02:00:00:00:00:01", "LibreEcho", "radar-puffin", "wifi",
        "Café & <Echo> \"one\" 'two'", LE_MDNS_ESPHOME_NOISE, "LibreEcho", "0.14.0"
    };
    struct le_mdns_lease lease;
    int fd, input;
    ssize_t size;
    assert(snprintf(dir, sizeof(dir), "%s/le-mdns-lease-XXXXXX", tmp ? tmp : ".") < (int)sizeof(dir));
    assert(mkdtemp(dir));
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    le_mdns_lease_init(&lease, fd);
    assert(le_mdns_lease_register_esphome(&lease, 7, 0, &metadata) < 0);
    assert(le_mdns_lease_register_esphome(&lease, 7, 65536, &metadata) < 0);
    assert(le_mdns_lease_register_esphome(&lease, 7, 6053, NULL) < 0);
    assert(le_mdns_lease_register_esphome(&lease, 7, 6053, &metadata) == 0);
    input = openat(fd, lease.filename, O_RDONLY);
    assert(input >= 0);
    size = read(input, data, sizeof(data) - 1);
    assert(size > 0); data[size] = 0;
    assert(strstr(data, "<port>6053</port>"));
    assert(strstr(data, "_esphomelib._tcp"));
    assert(!strstr(data, "_wyoming._tcp"));
    assert(strstr(data, "Café &amp; &lt;Echo&gt; &quot;one&quot; &apos;two&apos;"));
    assert(strstr(data, "mac=02:00:00:00:00:01"));
    close(input);
    assert(le_mdns_lease_register_esphome(&lease, 8, 6053, &metadata) < 0);
    assert(le_mdns_lease_withdraw(&lease, 8) < 0);
    memcpy(old, lease.filename, sizeof(old));
    assert(le_mdns_lease_register_esphome(&lease, 7, 65535, &metadata) == 0);
    assert(strcmp(old, lease.filename));
    assert(faccessat(fd, old, F_OK, 0) < 0);
    assert(le_mdns_lease_withdraw(&lease, 7) == 0);
    assert(lease.owner_fd == -1);
    lease.generation = ULONG_MAX;
    assert(le_mdns_lease_register_esphome(&lease, 7, 6053, &metadata) < 0);
    lease.generation = 0;
    assert(symlinkat("/no/such/target", fd, "esphome-1.tmp") == 0);
    assert(le_mdns_lease_register_esphome(&lease, 7, 6053, &metadata) < 0);
    assert(unlinkat(fd, "esphome-1.tmp", 0) == 0);
    memset(metadata.friendly_name, 'x', sizeof(metadata.friendly_name));
    assert(le_mdns_lease_register_esphome(&lease, 7, 6053, &metadata) < 0);
    close(fd);
    assert(rmdir(dir) == 0);
    puts("mDNS ESPHome lease: ports, XML/UTF-8, metadata, ownership, replacement, withdrawal, limits: ok");
    return 0;
}
