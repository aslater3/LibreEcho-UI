# Adding New Hardware to LibreEcho-UI

This guide shows how to add a new hardware subsystem to the LibreEcho web interface.

## When to Use This Guide

Use this when you need to:
- Add support for a new hardware device (e.g., camera, microphone array, button)
- Extend an existing daemon with new capabilities
- Integrate a new sensor or actuator

## Overview

Every hardware subsystem follows the same pattern:
1. **Companion daemon** owns the hardware
2. **Adapter protocol** exposes it to the web daemon
3. **Config section** persists settings
4. **Web UI panel** displays controls

## Step 1: Define the Hardware Domain

Choose a name and socket path:

```c
// src/adapter/adapter.h
#define LE_ADAPTER_MYD_SOCK "/run/libreecho/myd.sock"
```

## Step 2: Write the Companion Daemon

Create `src/adapter/myd.c`:

```c
/*
 * LibreEcho myd daemon — my hardware domain.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS 4

static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

/* ── Hardware Interface ─────────────────────────────────────────────── */

struct myd_hw {
    int fd;
    int available;
    /* Add hardware-specific fields */
};

static int hw_open(struct myd_hw *hw)
{
    // Open device, probe, initialize
    // Example: hw->fd = open("/dev/mydevice", O_RDWR);
    hw->available = (hw->fd >= 0);
    if (!hw->available)
        le_log_warn("myd: hardware not available");
    return hw->available ? 0 : -1;
}

static int hw_set_value(struct myd_hw *hw, int value)
{
    if (!hw->available)
        return -1;
    // Write to hardware
    // Example: return ioctl(hw->fd, MYD_SET_VALUE, &value);
    le_log_debug("myd: set value %d", value);
    return 0;
}

static int hw_get_value(struct myd_hw *hw)
{
    if (!hw->available)
        return -1;
    // Read from hardware
    return 0;
}

static void hw_close(struct myd_hw *hw)
{
    if (hw->fd >= 0)
        close(hw->fd);
    hw->fd = -1;
}

/* ── State ──────────────────────────────────────────────────────────── */

struct myd_state {
    int value;
    int enabled;
    /* Add persistent state fields */
};

static void load_state(struct myd_state *st)
{
    // Load from /etc/libreecho/myd-state.json
    // Use config_manager or json.h
    st->value = 0;
    st->enabled = 1;
}

static void save_state(const struct myd_state *st)
{
    // Save to /etc/libreecho/myd-state.json
    // Use config_manager or atomic file write
}

/* ── Command Handlers ───────────────────────────────────────────────── */

static int cmd_status(struct myd_hw *hw, struct myd_state *st,
                      char *response, size_t size)
{
    int current = hw_get_value(hw);
    int n = snprintf(response, size,
        "{\"available\":%s,\"value\":%d,\"enabled\":%s}",
        hw->available ? "true" : "false",
        current,
        st->enabled ? "true" : "false");
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

static int cmd_set_value(struct myd_hw *hw, struct myd_state *st,
                         const char *args, char *response, size_t size)
{
    int value;
    if (json_get_int(args, "value", &value) <= 0)
        return -1;
    if (hw_set_value(hw, value) < 0)
        return -1;
    st->value = value;
    save_state(st);
    return snprintf(response, size, "{}") >= 0 ? 0 : -1;
}

/* ── Main Loop ──────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *sock_path = LE_ADAPTER_MYD_SOCK;
    int foreground = 0;
    int listen_fd;
    struct myd_hw hw;
    struct myd_state st;
    struct pollfd pfds[1 + MAX_CLIENTS];
    int clients[MAX_CLIENTS];
    int i;

    le_log_init("myd", argc, argv);

    /* Parse args */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--foreground")) foreground = 1;
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock_path = argv[++i];
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "--debug") ||
                 !strcmp(argv[i], "--quiet")) {}
        else {
            fprintf(stderr, "usage: %s [--socket PATH] [--foreground] [--verbose] [--debug] [--quiet]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    memset(&hw, 0, sizeof(hw));
    hw.fd = -1;
    for (i = 0; i < MAX_CLIENTS; i++)
        clients[i] = -1;

    if (hw_open(&hw) < 0)
        le_log_warn("myd: starting without hardware");

    load_state(&st);

    if (!foreground && daemon(1, 0) < 0) {
        le_log_perror(LE_LOG_ERROR, "myd: daemon");
        return EXIT_FAILURE;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    listen_fd = le_adapter_listen(sock_path);
    if (listen_fd < 0) {
        hw_close(&hw);
        return EXIT_FAILURE;
    }

    le_log_info("myd: listening on %s", sock_path);

    while (!stop_requested) {
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        for (i = 0; i < MAX_CLIENTS; i++) {
            pfds[i + 1].fd = clients[i];
            pfds[i + 1].events = clients[i] >= 0 ? POLLIN : 0;
        }

        if (poll(pfds, 1 + MAX_CLIENTS, 1000) < 0) {
            if (errno == EINTR) continue;
            le_log_perror(LE_LOG_ERROR, "myd: poll");
            break;
        }

        /* Accept new clients */
        if (pfds[0].revents & POLLIN) {
            int fd = le_adapter_accept(listen_fd);
            if (fd >= 0) {
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i] < 0) {
                        clients[i] = fd;
                        le_log_debug("myd: client %d connected", i);
                        break;
                    }
                }
                if (i == MAX_CLIENTS) {
                    le_log_warn("myd: max clients reached, dropping");
                    close(fd);
                }
            }
        }

        /* Handle client requests */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0 && pfds[i + 1].revents & POLLIN) {
                char msg[4096];
                ssize_t n = read(clients[i], msg, sizeof(msg) - 1);
                if (n <= 0) {
                    le_log_debug("myd: client %d disconnected", i);
                    close(clients[i]);
                    clients[i] = -1;
                    continue;
                }
                msg[n] = '\0';

                char cmd[64], args[4096];
                unsigned long id;
                if (le_adapter_parse_request(msg, cmd, sizeof(cmd), (char**)&args, &id) < 0) {
                    le_adapter_respond_err(msg, sizeof(msg), 0, "malformed request");
                    write(clients[i], msg, strlen(msg));
                    continue;
                }

                le_log_debug("myd: cmd=\"%s\" id=%lu", cmd, id);

                char response[4096];
                int rc;
                if (!strcmp(cmd, "status")) {
                    rc = cmd_status(&hw, &st, response, sizeof(response));
                } else if (!strcmp(cmd, "set_value")) {
                    rc = cmd_set_value(&hw, &st, args, response, sizeof(response));
                } else {
                    rc = -1;
                    snprintf(response, sizeof(response), "{\"error\":\"unknown command\"}");
                }

                if (rc < 0)
                    le_adapter_respond_err(response, sizeof(response), id, "command failed");
                else
                    le_adapter_respond_ok(response, sizeof(response), id, response);

                write(clients[i], response, strlen(response));
            }
        }
    }

    le_log_info("myd: shutting down");
    hw_close(&hw);
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] >= 0) close(clients[i]);
    close(listen_fd);
    unlink(sock_path);
    return EXIT_SUCCESS;
}
```

## Step 3: Add to Makefile

```makefile
# In Makefile
ADAPTER_TARGETS += $(BUILD)/libreecho-myd
MYD_SOURCES = src/adapter/myd.c src/adapter/adapter_server.c src/log.c
MYD_OBJECTS = $(MYD_SOURCES:src/%.c=$(BUILD)/%.o)

$(BUILD)/libreecho-myd: $(MYD_OBJECTS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(MYD_OBJECTS) $(LDFLAGS) -o $@
```

## Step 4: Add Backend Support

Edit `src/backend_linux.c`:

```c
// Add to the ops vtable (match order in backend_internal.h)
static int myd(struct le_backend *b, struct le_myd_state *o)
{
    char response[4096];
    int rc = adapter_command(LE_ADAPTER_MYD_SOCK, "status", NULL, response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    // Parse response into o
    json_get_int(response, "value", &o->value);
    return LE_OK;
}

static int myd_set(struct le_backend *b, int value)
{
    char args[128];
    snprintf(args, sizeof(args), "{\"value\":%d}", value);
    return adapter_command(LE_ADAPTER_MYD_SOCK, "set_value", args, NULL, 0);
}
```

## Step 5: Add API Endpoint

Edit `src/api.c`:

```c
if (!strcmp(p, "/api/v1/myd")) {
    if (!strcmp(q->method, "GET")) {
        struct le_myd_state m;
        int rc = le_get_myd_state(c->backend, &m);
        if (rc) { err(r, 501, rc, "Myd not available"); return; }
        out(r, 200, "{\"ok\":true,\"data\":{\"value\":%d,\"enabled\":%s},\"error\":null}",
            m.value, m.enabled ? "true" : "false");
        return;
    }
    if (!strcmp(q->method, "PUT")) {
        int v;
        if (json_get_int(q->body, "value", &v) > 0) {
            rc = le_set_myd_value(c->backend, v);
            if (rc) { err(r, 500, rc, "Set failed"); return; }
            // Re-read and return current state
            // ...
        }
    }
}
```

## Step 6: Add Config Section

Edit `config/config.json`:

```json
{
  "myd": {
    "enabled": true,
    "default_value": 0,
    "poll_interval_ms": 1000
  }
}
```

## Step 7: Add Init Script

Create `init/libreecho-myd.init`:

```sh
#!/bin/sh
### BEGIN INIT INFO
# Provides:          libreecho-myd
# Required-Start:    $local_fs libreecho-logd
# Required-Stop:     $local_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
### END INIT INFO

DAEMON=/usr/local/sbin/libreecho-myd
PIDFILE=/var/run/libreecho-myd.pid

# ... (copy pattern from libreecho-audiod.init)
```

## Step 8: Add Tests

Create `tests/test_myd.sh`:

```sh
#!/bin/sh
# Test myd adapter protocol
set -eu

SOCKET=/tmp/myd_test.sock
DAEMON=./build/libreecho-myd

# Start daemon in stub mode
$DAEMON --foreground --socket $SOCKET &
PID=$!
sleep 1

# Test status
RESULT=$(echo '{"v":1,"id":1,"cmd":"status","args":{}}' | nc -U $SOCKET | head -1)
echo "Status: $RESULT"

# Test set_value
RESULT=$(echo '{"v":1,"id":2,"cmd":"set_value","args":{"value":42}}' | nc -U $SOCKET | head -1)
echo "Set: $RESULT"

# Cleanup
kill $PID 2>/dev/null || true
rm -f $SOCKET
```

## Step 9: Add UI Panel

Edit `web/js/app.js` and `web/index.html` to add a control panel for the new hardware.

## Step 10: Update Documentation

- Add to `docs/ARCHITECTURE.md` under Companion Daemons
- Add to `docs/HARDWARE.md` (create if needed)
- Update `README.md` with new daemon

## Checklist

- [ ] Daemon compiles: `make build/libreecho-myd`
- [ ] Daemon starts without hardware (stub mode)
- [ ] Adapter protocol responds to `status` command
- [ ] Web daemon can query daemon via backend_linux.c
- [ ] API endpoint returns 200 on GET
- [ ] API endpoint accepts PUT and persists change
- [ ] Config section loads on daemon startup
- [ ] Init script starts/stops daemon correctly
- [ ] Logs appear in `/var/log/libreecho/system.log`
- [ ] `make test` passes
- [ ] Cross-compiles for ARM32

## Common Patterns

### Hardware Not Available
Always allow the daemon to start without hardware. Return `LE_NOT_SUPPORTED` from backend_linux.c when the daemon socket is missing.

### Stub Mode
Add a `--stub` flag for testing without hardware. Store state in memory only.

### Error Recovery
If hardware disappears mid-operation, log the error and return `LE_IO` — don't crash.

### Rate Limiting
For sensors that update frequently, add a minimum poll interval in the daemon. Don't let the web UI hammer the hardware.

### Atomic Config
Use `config_write_atomic()` or `le_config_write()` for config changes. Never write directly to `/etc/libreecho/config.json` from a companion daemon — only the web daemon writes config.
