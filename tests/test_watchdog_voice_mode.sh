#!/bin/sh
# The watchdog must follow the saved voice mode, not fight it.
#
# Local voice (sttd/ttsd/agentd) and the Home Assistant satellite (esphomed)
# own the microphone path exclusively; the web API stops one set and starts
# the other when the owner switches mode. A watchdog that latched on "seen
# healthy once" then restarted whichever set the API had just stopped: on a
# real device it revived local STT/TTS/agent ~70 s into HA mode and brought
# esphomed (and port 6053) back after HA was turned off. This drives two fake
# services owned by opposite modes through a mode switch in one watchdog run.
set -eu
dir=$(mktemp -d)
cleanup() {
    for f in "$dir"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null || true; done
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

cat > "$dir/fake.py" <<'PY'
import os, socket, sys
path = sys.argv[1]
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(4)
open(sys.argv[2], "w").write(str(os.getpid()))
while True:
    c, _ = s.accept()
    try:
        c.recv(4096)
        c.sendall(b'{"v":1,"id":1,"ok":true,"data":{},"error":null}\n')
    except Exception:
        pass
    c.close()
PY

# One init script per fake service; "start" is recorded so restarts count.
for name in local ha; do
    cat > "$dir/$name.init" <<EOF
#!/bin/sh
case "\$1" in
  start) echo start >> "$dir/$name.starts"
         python3 "$dir/fake.py" "$dir/$name.sock" "$dir/$name.pid" &
         sleep 1 ;;
  stop)  [ -f "$dir/flip-on-stop" ] && cp "$dir/flip-on-stop" "$dir/web-config.json"
         [ -f "$dir/$name.pid" ] && kill "\$(cat "$dir/$name.pid")" 2>/dev/null || true
         rm -f "$dir/$name.sock" "$dir/$name.pid" ;;
esac
exit 0
EOF
    chmod +x "$dir/$name.init"
    : > "$dir/$name.starts"
done
starts() { wc -l < "$dir/$1.starts" | tr -d ' '; }

WD=./build/libreecho-watchdogd
[ -x "$WD" ] || { echo "watchdogd not built"; exit 1; }
config="$dir/web-config.json"
local_cfg='{"integrations":20,"voice_pipeline_mode":"local"}'
ha_cfg='{"integrations":21,"voice_pipeline_mode":"home-assistant"}'
services="--service local:$dir/local.sock:$dir/local.init::local --service ha:$dir/ha.sock:$dir/ha.init::home-assistant"

# --- local -> Home Assistant: local services stay down ---------------------
printf '%s\n' "$local_cfg" > "$config"
sh "$dir/local.init" start
(
    sleep 3
    # What the API does: persist HA, stop local, start the satellite.
    printf '%s\n' "$ha_cfg" > "$config"
    sh "$dir/local.init" stop
    sh "$dir/ha.init" start
) &
"$WD" --passes 12 --interval 1 --config "$config" $services >"$dir/wd1.log" 2>&1
wait
if [ "$(starts local)" != "1" ]; then
    echo "FAIL: watchdog restarted a local voice service in Home Assistant mode"
    cat "$dir/wd1.log"; exit 1
fi
echo "  local voice left stopped in Home Assistant mode: ok"

# --- the owning mode's service is still supervised -------------------------
# Kill the satellite while HA is selected: that is a crash, and it must come
# back. Mode-awareness must not switch supervision off altogether.
(
    sleep 3
    kill "$(cat "$dir/ha.pid")"; rm -f "$dir/ha.sock"
) &
"$WD" --passes 12 --interval 1 --config "$config" $services >"$dir/wd2.log" 2>&1
wait
if [ "$(starts ha)" -lt "2" ]; then
    echo "FAIL: watchdog did not restart the satellite while HA owns voice"
    cat "$dir/wd2.log"; exit 1
fi
echo "  crashed satellite restarted while HA is selected: ok"

# --- Home Assistant -> local: the satellite stays down ---------------------
ha_before=$(starts ha)
(
    sleep 3
    printf '%s\n' "$local_cfg" > "$config"
    sh "$dir/ha.init" stop
    sh "$dir/local.init" start
) &
"$WD" --passes 12 --interval 1 --config "$config" $services >"$dir/wd3.log" 2>&1
wait
if [ "$(starts ha)" != "$ha_before" ] || [ -S "$dir/ha.sock" ]; then
    echo "FAIL: watchdog brought the ESPHome satellite back after HA was turned off"
    cat "$dir/wd3.log"; exit 1
fi
echo "  satellite left stopped after leaving Home Assistant: ok"

# --- an unreadable config never disables supervision -----------------------
# A missing or half-written file must not be read as "nothing is wanted".
rm -f "$config"
local_before=$(starts local)
(
    sleep 3
    kill "$(cat "$dir/local.pid")"; rm -f "$dir/local.sock"
) &
"$WD" --passes 12 --interval 1 --config "$config" $services >"$dir/wd4.log" 2>&1
wait
if [ "$(starts local)" -le "$local_before" ]; then
    echo "FAIL: an unreadable config switched supervision off"
    cat "$dir/wd4.log"; exit 1
fi
echo "  unreadable config keeps supervising running services: ok"

# --- a mode switch that lands during a restart is honoured -----------------
# The watchdog reads the mode once per pass. If the API persists HA after that
# read while a local restart is already due, starting the local service would
# leave both exclusive voice owners running. The fake local init's "stop"
# persists HA mode itself, so the switch lands exactly inside the restart.
printf '%s\n' "$local_cfg" > "$config"
[ -S "$dir/local.sock" ] || sh "$dir/local.init" start
[ -f "$dir/ha.pid" ] && sh "$dir/ha.init" stop
printf '%s\n' "$ha_cfg" > "$dir/flip-on-stop"
local_before=$(starts local)
(
    sleep 3
    kill "$(cat "$dir/local.pid")"; rm -f "$dir/local.sock"
) &
"$WD" --passes 12 --interval 1 --config "$config" $services >"$dir/wd5.log" 2>&1
wait
rm -f "$dir/flip-on-stop"
if [ "$(starts local)" != "$local_before" ] || [ -S "$dir/local.sock" ]; then
    echo "FAIL: watchdog started a local voice service after HA took ownership mid-restart"
    cat "$dir/wd5.log"; exit 1
fi
echo "  mode switch during a restart is honoured: ok"

# --- a service whose mode is reselected is supervised again ----------------
# Leaving local mode must not make the watchdog forget local voice is
# installed. When local is selected again and the daemon dies before the
# watchdog's next probe, it must still be restarted.
printf '%s\n' "$local_cfg" > "$config"
[ -S "$dir/local.sock" ] || sh "$dir/local.init" start
[ -f "$dir/ha.pid" ] && sh "$dir/ha.init" stop
local_before=$(starts local)
(
    sleep 3
    printf '%s\n' "$ha_cfg" > "$config"
    sh "$dir/local.init" stop
    sleep 3
    # Local is selected again; its daemon exited before being probed.
    printf '%s\n' "$local_cfg" > "$config"
) &
"$WD" --passes 20 --interval 1 --rearm-grace 2 --config "$config" $services >"$dir/wd6.log" 2>&1
wait
if [ "$(starts local)" -le "$local_before" ]; then
    echo "FAIL: watchdog forgot a reselected voice service and never restarted it"
    cat "$dir/wd6.log"; exit 1
fi
echo "  reselected voice service is supervised again: ok"

# --- a service deselected when the watchdog started is armed on selection --
# Booting in HA mode means local voice is never probed. When the owner then
# selects local and the daemon exits before the first probe, the watchdog
# must still restart it rather than wait for an answer that never comes.
printf '%s\n' "$ha_cfg" > "$config"
[ -f "$dir/local.pid" ] && sh "$dir/local.init" stop
[ -S "$dir/ha.sock" ] || sh "$dir/ha.init" start
local_before=$(starts local)
(
    sleep 3
    printf '%s\n' "$local_cfg" > "$config"
    sh "$dir/ha.init" stop
) &
"$WD" --passes 20 --interval 1 --rearm-grace 2 --config "$config" $services >"$dir/wd7.log" 2>&1
wait
if [ "$(starts local)" -le "$local_before" ]; then
    echo "FAIL: watchdog never armed a voice service first selected after it started"
    cat "$dir/wd7.log"; exit 1
fi
if [ -f "$dir/ha.pid" ]; then
    echo "FAIL: watchdog restarted the deselected satellite"
    cat "$dir/wd7.log"; exit 1
fi
echo "  service selected after watchdog start is armed: ok"

# --- a re-armed service is not restarted while its init is still starting -
# agentd's init can wait 90 s for its dependencies before it answers. The
# default grace must cover that: restarting inside it would stop the runtime
# under the API's start and race it. Here the API-started daemon takes 12 s
# to answer, longer than the old 6-interval grace at --interval 1.
printf '%s\n' "$ha_cfg" > "$config"
[ -f "$dir/local.pid" ] && sh "$dir/local.init" stop
[ -S "$dir/ha.sock" ] || sh "$dir/ha.init" start
local_before=$(starts local)
(
    sleep 3
    printf '%s\n' "$local_cfg" > "$config"
    sh "$dir/ha.init" stop
    # What a slow API start looks like: no socket until the daemon is up.
    sleep 12
    python3 "$dir/fake.py" "$dir/local.sock" "$dir/local.pid" &
) &
"$WD" --passes 22 --interval 1 --config "$config" $services >"$dir/wd8.log" 2>&1
wait
if [ "$(starts local)" -ne "$local_before" ]; then
    echo "FAIL: watchdog restarted a re-armed service during its startup window"
    cat "$dir/wd8.log"; exit 1
fi
echo "  re-armed service is not restarted while starting: ok"
echo "watchdog voice-mode ownership: PASS"
