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

mdk_kill; sleep 1; rm -rf "$(dirname "$log")/logs"      # the copy keeps earlier runs' session logs: only this test's count
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
# the session logs (bridge/diag.c): one file per run, never replaced, with the crashed run marked and its minute saved
logs="$(dirname "$log")/logs"
crashed_session() { ls "$logs"/*.crash 2>/dev/null | head -1 | sed 's/\.crash$//'; }
sess="$(crashed_session)"
check "two session logs, one per run"              test "$(ls "$logs"/*.log 2>/dev/null | wc -l)" = 2
check "the crashed run is marked"                  test -n "$sess" -a -f "$sess.log"
check "the crashed run's log has the crash"        grep -q " crash: exception 0xc0000005" "$sess.log"
check "the crashed run has no clean exit line"     bash -c "! grep -q 'session end: clean exit' '$sess.log'"
check "the flight recorder was written"            test -s "$(ls "$sess".flight-0-crash.txt 2>/dev/null | head -1)"
check "the flight dump has samples and the log tail" bash -c "grep -q '^S ' '$sess.flight-0-crash.txt' && grep -q '^L .* $tag' '$sess.flight-0-crash.txt'"
check "the hash chains are intact"                 python3 "$here/diag_report.py" --raw "$(dirname "$log")"
cp -r "$logs" "$out/logs" 2>/dev/null
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
