#!/usr/bin/env bash
# Crash evidence in one background game: a crash writes its exception and place to bridge.log, and starting
# the game again keeps that log as bridge.previous.log instead of wiping it (players restart before reporting).
# coop_crash() crashes on purpose and works only in background test mode.
#   MDK2_N=1 tools/qa_crashlog.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-1}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-crashlog"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
log="$(mdk_log 1)"
previous="$(dirname "$log")/bridge.previous.log"
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
started() { grep -q "^level [1-9][0-9]* checkpoint [0-9]* started" "$log" 2>/dev/null; }
dead() { ! mdk_alive "$(mdk_pid 1)"; }

mdk_boot
check "game reached a level"                       wait_for started 60
tag="crashlog-marker-$$-$RANDOM"
mdk_cmd "$(mdk_pid 1)" "coop_log(\"$tag\")"; sleep 1
mdk_cmd "$(mdk_pid 1)" 'coop_crash()'
check "the game ended after the crash"             wait_for dead 30
cp "$log" "$out/crashed.log" 2>/dev/null
check "the crash is in the log"                    grep -qiE "^crash: exception 0xc0000005 at dinput8\.dll\+0x[0-9a-f]+ .*writing 0x0*10\)" "$log"

echo "$(mdk_start 1)" > "$MDK2_PIDS"
check "the game started again"                     wait_for "grep -q 'bridge attached' '$log' 2>/dev/null && ! grep -q '$tag' '$log'" 30
check "the crashed run's log is kept"              grep -q "$tag" "$previous"
check "the kept log has the crash"                 grep -q "^crash: exception 0xc0000005" "$previous"
check "the new log is a fresh one"                 bash -c "! grep -q '^crash:' '$log'"
cp "$log" "$out/restarted.log"; cp "$previous" "$out/previous.log" 2>/dev/null
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
