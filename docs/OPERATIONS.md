# LibreEcho-UI Operations Guide

This guide covers building, configuring, running, and troubleshooting the LibreEcho web interface. Device flashing and deployment belong to the separate LibreEcho image/build project.

## Quick Start

### Build (Host)

```sh
# Native build (x86-64)
make clean && make all

# Cross-compile for ARM32
make clean && make CROSS_COMPILE=arm-linux-musleabihf- release

# Verify binaries
file build/libreecho-*
```

### Start Services

```sh
# On target device (via ADB or SSH)
/etc/init.d/libreecho-logd start
/etc/init.d/libreecho-networkd start
/etc/init.d/libreecho-audiod start
/etc/init.d/libreecho-ledd start
/etc/init.d/libreecho-web start
```

The full voice loop additionally requires the services to remain warm in this
order:

```sh
/etc/init.d/libreecho-waked start
/etc/init.d/libreecho-sttd start
/etc/init.d/libreecho-ttsd start
/etc/init.d/libreecho-agentd start
```

### Access

```
http://<device-ip>:8080        # LAN (requires --allow-insecure-lan)
http://127.0.0.1:8080          # Local only (default)
```

## Configuration

### ChatGPT subscription voice setup

1. Open **Integrations → Voice assistant**.
2. Select **Connect ChatGPT**.
3. Open the displayed verification URL on a trusted browser and enter the
   device code.
4. Return to LibreEcho and wait for the status to become **Connected**.
5. Enable **wake-to-reply voice loop** and save.

This path uses the user's ChatGPT subscription through device OAuth. LibreEcho
does not accept an OpenAI API key and does not silently switch to metered API
billing. Microphone audio and speech recognition remain local; only the final
transcript and configured instruction prompt are sent to the selected response
provider.

Persistent non-secret settings are stored in
`/data/libreecho/config/agent.json`. OAuth credentials are stored separately
in `/data/libreecho/secrets/openai-codex.json` with mode `0600`.

Assistant status and latency telemetry are available from:

```sh
curl http://127.0.0.1:8080/api/v1/assistant
```

The measured target is at most 3000 ms from estimated speech end to the first
PCM sent to the announcement bus. Check `last_stt_processing_ms`,
`last_first_text_ms`, `last_first_announce_dispatch_ms`, and
`last_speech_end_to_first_pcm_ms` to locate a regression.

### Central Config

All services read from `/etc/libreecho/config.json`:

```json
{
  "version": 1,
  "system": {
    "hostname": "libreecho",
    "log_level": "info",
    "log_retention_days": 7
  },
  "audio": {
    "volume": 50,
    "microphone_gain": 65,
    "microphone_muted": false
  },
  "led": {
    "brightness": 70,
    "boot_color": [72, 216, 118]
  },
  "network": {
    "wifi_enabled": true,
    "hostname": "libreecho",
    "ssh_enabled": false
  },
  "wake_word": {
    "enabled": true,
    "sensitivity": 68
  },
  "privacy": {
    "local_only": true,
    "telemetry": false
  }
}
```

### Per-Service Config

Each service reads its section at startup. To change:

1. Edit `/etc/libreecho/config.json` (or use web UI)
2. Reload: `killall -HUP libreecho-networkd libreecho-audiod libreecho-ledd libreecho-web`

### Config History

Every write saves the previous version:

```
/etc/libreecho/history/
├── config-20260720-143022.json
├── config-20260720-142815.json
└── ... (last 10 kept)
```

Restore a previous version:
```sh
cp /etc/libreecho/history/config-20260720-143022.json /etc/libreecho/config.json
killall -HUP libreecho-web
```

## Logging

### View Logs

```sh
# Real-time (all services)
tail -f /var/log/libreecho/system.log

# Filter by service
grep '"service":"networkd"' /var/log/libreecho/system.log

# Filter by level
grep '"level":"error"' /var/log/libreecho/system.log

# Via web API
curl http://127.0.0.1:8080/api/v1/logs
```

### Log Levels

| Level | Flag | Output |
|-------|------|--------|
| DEBUG | `--verbose` | All messages |
| INFO | (default) | Info + warnings + errors |
| WARNING | `--quiet` | Warnings + errors only |
| ERROR | (always) | Errors only |

### Log Rotation

Automatic when file exceeds 512KB:

```
/var/log/libreecho/
├── system.log       # Current
├── system.log.1     # Previous
└── system.log.2     # Oldest (deleted on next rotation)
```

## Backup & Restore

### Create Backup

```sh
# On target device
tools/libreecho-backup.sh create /tmp/backup.tar.gz

# Contents:
# - /etc/libreecho/ (all config)
# - /var/log/libreecho/ (recent logs)
# - Web daemon state
```

### Restore Backup

```sh
# Stops all services, restores, restarts
tools/libreecho-backup.sh restore /tmp/backup.tar.gz

# Restore only config
tar -xzf /tmp/backup.tar.gz -C /tmp/restore
cp /tmp/restore/config/* /etc/libreecho/
killall -HUP libreecho-web
```

### List Backup Contents

```sh
tools/libreecho-backup.sh list /tmp/backup.tar.gz
```

## Service Management

### Start/Stop/Restart

```sh
# Individual services
/etc/init.d/libreecho-web restart
/etc/init.d/libreecho-networkd stop
/etc/init.d/libreecho-audiod start

# All at once
/etc/init.d/libreecho-logd stop
/etc/init.d/libreecho-networkd stop
/etc/init.d/libreecho-audiod stop
/etc/init.d/libreecho-ledd stop
/etc/init.d/libreecho-web stop
```

### Check Status

```sh
# Init scripts
/etc/init.d/libreecho-web status

# Process list
ps | grep libreecho

# Socket check
ls -la /run/libreecho/*.sock
```

### Startup Order

**Required:**
1. `libreecho-logd` (first — others connect to it)
2. `libreecho-networkd`
3. `libreecho-audiod`
4. `libreecho-ledd`
5. `libreecho-web` (last — connects to all above)

**Graceful degradation:** Web daemon works even if companion daemons are missing (returns 501 for unavailable hardware).

## Troubleshooting

### Web UI Not Loading

```sh
# Check web daemon is running
ps | grep libreecho-web

# Check port
netstat -tlnp | grep 8080

# Check logs
tail /var/log/libreecho/system.log | grep '"service":"libreecho-web"'

# Test API directly
curl http://127.0.0.1:8080/api/v1/status
```

### Hardware Not Responding

```sh
# Check daemon is running
ps | grep libreecho-networkd
ps | grep libreecho-audiod
ps | grep libreecho-ledd

# Check socket exists
ls -la /run/libreecho/*.sock

# Check daemon logs
grep '"service":"networkd"' /var/log/libreecho/system.log | tail -20

# Test adapter directly (manual)
echo '{"v":1,"id":1,"cmd":"status","args":{}}' | nc -U /run/libreecho/audio.sock
```

### Config Not Applying

```sh
# Check config syntax
python3 -m json.tool /etc/libreecho/config.json

# Check daemon loaded it
grep "config: loaded" /var/log/libreecho/system.log

# Force reload
killall -HUP libreecho-web
grep "config: reloaded" /var/log/libreecho/system.log
```

### Logs Not Appearing

```sh
# Check logd is running
ps | grep libreecho-logd

# Check socket exists
ls -la /run/libreecho/log.sock

# Check log directory
ls -la /var/log/libreecho/

# Test logd directly
echo '{"ts":1234567890,"level":"info","service":"test","msg":"hello"}' | \
    socat - UNIX-SENDTO:/run/libreecho/log.sock
tail /var/log/libreecho/system.log
```

### Permission Denied

```sh
# Config files must be 0600, owned by root
ls -la /etc/libreecho/config.json
chmod 600 /etc/libreecho/config.json
chown root:root /etc/libreecho/config.json

# Log directory must be writable
ls -la /var/log/libreecho/
chmod 755 /var/log/libreecho/
```

## Security

### LAN Access

Default is loopback only. To enable LAN:

```sh
# Create auth token
openssl rand -hex 32 > /etc/libreecho/api.token
chmod 600 /etc/libreecho/api.token

# Start with auth
libreecho-web --backend linux \
    --listen 0.0.0.0:8080 \
    --auth-token-file /etc/libreecho/api.token \
    --allowed-origin http://libreecho.local:8080
```

**Never use `--allow-insecure-lan` in production.**

### File Permissions

| Path | Mode | Owner | Purpose |
|------|------|-------|---------|
| `/etc/libreecho/config.json` | 0600 | root | Central config |
| `/etc/libreecho/api.token` | 0600 | root | Auth token |
| `/var/log/libreecho/` | 0755 | root | Log directory |
| `/run/libreecho/*.sock` | 0660 | root | Adapter sockets |

## Development

### Mock Backend

Test without hardware:

```sh
./build/libreecho-web --backend mock --listen 127.0.0.1:8080
```

### Debug Logging

```sh
# All daemons
./build/libreecho-networkd --foreground --verbose --socket /tmp/test.sock

# Web daemon
./build/libreecho-web --backend linux --verbose --listen 127.0.0.1:8080
```

### Test Suite

```sh
make test
```

## Upgrade

```sh
# Backup first
tools/libreecho-backup.sh create /tmp/pre-upgrade-backup.tar.gz

# Stop services
/etc/init.d/libreecho-web stop
# ... (all services)

# Start services
/etc/init.d/libreecho-logd start
# ... (all services)

# Verify
curl http://127.0.0.1:8080/api/v1/status
```
