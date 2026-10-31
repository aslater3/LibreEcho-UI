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
    {
        /* LibreEcho control centre (#413): wire round trip, validation, render. */
        struct le_mdns_libreecho_metadata web = { "kitchen-echo", "0.14.6", "Café & <Hall>" }, back;
        struct le_mdns_lease web_lease;
        char message[LE_MDNS_LIBREECHO_PACKET_MAX];
        unsigned int port = 0;
        int n = le_mdns_libreecho_encode(message, sizeof(message), 8080, &web);
        assert(n > 0 && message[n - 1] == '\n');
        assert(!strncmp(message, "LIBREECHO/1 8080 name=6b69", 26));
        assert(le_mdns_libreecho_decode(message, (size_t)n, &port, &back) == 0 && port == 8080);
        assert(!strcmp(back.name, "kitchen-echo") && !strcmp(back.version, "0.14.6") &&
               !strcmp(back.room, web.room));
        assert(le_mdns_libreecho_decode(message, (size_t)n - 1, &port, &back) < 0);
        assert(le_mdns_libreecho_encode(message, sizeof(message), 0, &web) < 0);
        web.room[0] = 0;  /* room is optional */
        n = le_mdns_libreecho_encode(message, sizeof(message), 80, &web);
        assert(n > 0 && le_mdns_libreecho_decode(message, (size_t)n, &port, &back) == 0 && !back.room[0]);
        strcpy(web.version, "0.14\"6"); assert(le_mdns_libreecho_encode(message, sizeof(message), 80, &web) < 0);
        strcpy(web.version, "0.14.6"); web.name[0] = 0;
        assert(le_mdns_libreecho_encode(message, sizeof(message), 80, &web) < 0);
        strcpy(web.name, "a\nb"); assert(le_mdns_libreecho_encode(message, sizeof(message), 80, &web) < 0);
        assert(le_mdns_libreecho_decode("LIBREECHO/1 80 name=6b version=31 room= extra=1\n", 47, &port, &back) < 0);
        assert(le_mdns_libreecho_decode("LIBREECHO/1 80 version=31 name=6b room=\n", 40, &port, &back) < 0);
        assert(le_mdns_libreecho_decode("LIBREECHO/1 80 name=6b version=31 room=\n", 40, &port, &back) == 0);
        strcpy(web.name, "kitchen-echo"); strcpy(web.room, "Café & <Hall>");
        le_mdns_lease_init(&web_lease, fd);
        assert(le_mdns_lease_register_libreecho(&web_lease, 9, 8080, &web) == 0);
        assert(!strncmp(web_lease.filename, "libreecho-", 10));
        input = openat(fd, web_lease.filename, O_RDONLY);
        assert(input >= 0);
        size = read(input, data, sizeof(data) - 1);
        assert(size > 0); data[size] = 0; close(input);
        assert(strstr(data, "<type>_libreecho._tcp</type><port>8080</port>"));
        assert(strstr(data, "<name replace-wildcards=\"no\">kitchen-echo</name>"));
        assert(strstr(data, "<txt-record>version=0.14.6</txt-record>"));
        assert(strstr(data, "<txt-record>room=Café &amp; &lt;Hall&gt;</txt-record>"));
        assert(strstr(data, "<txt-record>path=/</txt-record>"));
        assert(le_mdns_lease_register_libreecho(&web_lease, 10, 8080, &web) < 0);
        assert(le_mdns_lease_withdraw(&web_lease, 9) == 0);
    }
    close(fd);
    assert(rmdir(dir) == 0);
    puts("mDNS ESPHome lease: ports, XML/UTF-8, metadata, ownership, replacement, withdrawal, limits: ok");
    puts("mDNS LibreEcho lease: wire round trip, validation, render, single owner: ok");
    return 0;
}
