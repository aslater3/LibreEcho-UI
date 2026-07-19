#!/bin/sh
set -eu
pid=$1
rss=$(ps -o rss= -p "$pid" | tr -d ' ')
[ -n "$rss" ]
printf 'memory: idle RSS %s KiB\n' "$rss"
[ "$rss" -lt 15360 ]
