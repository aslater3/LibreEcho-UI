#!/bin/sh
set -eu

grep -q '#define LE_MAX_TLS_RELAYS 4' src/http_server.c
grep -q 'tls_relay_pids' src/http_server.c
grep -q 'LE_TLS_HANDSHAKE_TIMEOUT_MS' src/tls.c
grep -q 'poll(&waitfd' src/tls.c
grep -q 'tls_stub.c' Makefile
grep -q 'https_active' src/api.h src/api.c
grep -q 'known_user' src/auth.c
grep -q 'le_auth_save_sessions' src/api.c
grep -q 'init/libreecho-radiod.init' Makefile
grep -q 'usb_open_dir' src/api.c
grep -q 'openat' src/api.c
grep -q 'O_NOFOLLOW' src/api.c
grep -q 'fstatat' src/api.c
grep -q 'open_usb_file' src/adapter/radiod.c
grep -q 'O_NOFOLLOW' src/adapter/radiod.c
grep -q 'install_stop_handlers' src/adapter/radiod.c
grep -q 'close(http_listener)' src/http_server.c
grep -q 'relay_host' src/http_server.c
grep -Fq "return !*host||*host==':'" src/api.c
sh -n init/libreecho-radiod.init
printf '%s\n' 'PR 137 review contracts: ok'
