#!/usr/bin/env bash
# The AI target guard (bridge/presence_game.c ai_target_ready), live: the sight check at mdk2hd.exe+0x2a40 reads the
# game object of the AI target and faulted (0x2a95, reading 8) when a gob that was still in the table had none. A test
# hook makes one non-player gob the AI target and takes its game object away while enemies think.
#   MDK2_N=1 tools/qa_ai_target_guard.sh                  guard on: the game must stay up and the guard must answer
#   MUTANT=noguard MDK2_N=1 tools/qa_ai_target_guard.sh   guard off (coop_ai_guard(0)): the game must crash at 0x2a95
# Overhaul build only (the fault site is that build's). About 2 minutes. PLANT=<name part> picks the gob.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
level="${LEVEL:-2}"; cp="${CP:-9}"; plant="${PLANT:-l2_roomlight32}"
MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || exit 1
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-aiguard"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
d="$(mdk_wsl "$(mdk_dir 1)")"; log="$d/coop/bridge.log"
# counter <name>: a value from the game's own state dump (presence_dump), read from the process
counter() { local f="$d/coop/state_ag.txt"; rm -f "$f"; mdk_cmd "$(mdk_pid 1)" 'coop_state("ag")' >/dev/null 2>&1
            for _ in $(seq 1 30); do [ -e "$f" ] && break; sleep 0.2; done; sleep 0.3; tr -d "\r" < "$f" 2>/dev/null | sed -n "s/^$1 //p" | tail -1; }
has() { grep -qE "$1" "$log"; }
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
mdk_kill; sleep 1
mdk_boot
check "game reached level $level checkpoint $cp"    wait_for "has '^level $level checkpoint $cp started'" 90
sleep 5
mdk_cmd "$(mdk_pid 1)" 'God(1)'
# the turrets above the checkpoint shaft of level 2 (l2r9tur*) ask for their target as soon as the hero is in the shaft
[ "$level.$cp" = 2.9 ] && mdk_cmd "$(mdk_pid 1)" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 1342, 899, 240)'
sleep 4
check "enemies ask for a target before the plant" test "$(counter ai_queries)" -gt 0
if [ "${MUTANT:-}" = noguard ]; then
    mdk_crash_expect 1
    mdk_cmd "$(mdk_pid 1)" 'coop_ai_guard(0)'
fi
mdk_cmd "$(mdk_pid 1)" "coop_test_infoless_target(\"$plant\")"
check "the target was planted"                      wait_for "has 'test: planted AI target'" 10
# the native callers of the sight check are not reached by the shaft's turrets: run their sequence for one actor
mdk_cmd "$(mdk_pid 1)" 'coop_test_sight("l2r9tur01")'
if [ "${MUTANT:-}" = noguard ]; then
    check "the game crashed at the sight check"     wait_for "has 'crash: sight target|0x2a95|mdk2hd.exe\\+0x2a95'" 40
    check "the crash log names the sight target"    wait_for "has 'crash: sight target gob=.* info=00000000'" 10
    mdk_crash_scan                                   # the game stays up behind its error dialog until it is closed
    check "the process is gone"                     wait_for "! mdk_alive $(mdk_pid 1)" 20
else
    check "the sight check was not run on it"       wait_for "has 'test: sight check skipped, no AI target'" 10
    check "the guard answered no target"            wait_for 'test "$(counter ai_infoless_answers)" -gt 0' 40
    check "enemies asked for a target (ai_queries)"  test "$(counter ai_queries)" -gt 0
    sleep 20
    check "the game is still running"               mdk_alive "$(mdk_pid 1)"
    mdk_cmd "$(mdk_pid 1)" 'coop_test_infoless_target("")'
    sleep 3
    check "and runs after the object is restored"   mdk_alive "$(mdk_pid 1)"
fi
cp "$log" "$out/bridge.log" 2>/dev/null
mdk_crash_scan
[ "${MUTANT:-}" = noguard ] && mdk_crash_collect "$out/dumps"
mdk_kill
printf '%s\n' "${results[@]}"; echo "passed $pass failed $fail (evidence $out)"
[ "$fail" = 0 ]
