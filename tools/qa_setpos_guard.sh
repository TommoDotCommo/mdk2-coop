#!/usr/bin/env bash
# The game's position setter on a gob that does not exist (mdkGobSetPositionXYZ(mdkGetPlayerGob(), ...) while no level is
# loaded, the title screen or a load in progress, stored to address 0x18: access violation at mdk2hd.exe+0xdda8).
# The bridge ignores a position set on a null gob. A running level answers mdkGetPlayerGob() fine, so the test passes
# nil, which reaches the same native call.
#   MDK2_N=1 tools/qa_setpos_guard.sh                  guard on: the call is ignored (counter 1), the game stays up
#   MUTANT=unguarded MDK2_N=1 tools/qa_setpos_guard.sh guard off (coop_setposguard(0)): the game must crash at 0xdda8
# Overhaul build (the fault site was seen there; the setter is the same code in both builds). About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
level="${LEVEL:-2}"; cp="${CP:-4}"
MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || exit 1
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-setposguard"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
d="$(mdk_wsl "$(mdk_dir 1)")"; log="$d/coop/bridge.log"
has() { grep -qE "$1" "$log"; }
note() { tr -d "\r" < "$d/coop/note_setpos.txt" 2>/dev/null; }
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
mdk_kill; sleep 1
mdk_boot
check "game reached level $level checkpoint $cp"    wait_for "has '^level $level checkpoint $cp started'" 90
check "the position setter guard is installed"      has 'position setter guard installed'
sleep 3
rm -f "$d/coop/note_setpos.txt"
# a real placement works (the guard passes real gobs through)
mdk_cmd "$(mdk_pid 1)" 'local p=mdkGetPlayerGob() local x,y,z=p.position.x,p.position.y,p.position.z mdkGobSetPositionXYZ(p, x + 1, y, z) coop_note("setpos", format("moved %.1f", p.position.x - x))'
check "a placement on a real gob still moves it"    wait_for "note | grep -q '^moved 1.0'" 10
if [ "${MUTANT:-}" = unguarded ]; then
    mdk_crash_expect 1
    mdk_cmd "$(mdk_pid 1)" 'coop_setposguard(0)'
fi
mdk_cmd "$(mdk_pid 1)" 'mdkGobSetPositionXYZ(nil, 49, 216, 0) coop_note("setpos", format("survived %d", coop_setpos_blocked()))'
if [ "${MUTANT:-}" = unguarded ]; then
    check "the game crashed in the position setter" wait_for "has 'crash: exception 0xc0000005 at mdk2hd.exe\\+0xdda8'" 40
    mdk_crash_scan
    check "the process is gone"                     wait_for "! mdk_alive $(mdk_pid 1)" 20
else
    check "the null placement was ignored and counted" wait_for "note | grep -q '^survived 1'" 15
    sleep 10
    check "the game is still running"               mdk_alive "$(mdk_pid 1)"
fi
cp "$log" "$out/bridge.log" 2>/dev/null
mdk_crash_scan
[ "${MUTANT:-}" = unguarded ] && mdk_crash_collect "$out/dumps"
mdk_kill
printf '%s\n' "${results[@]}"; echo "$pass passed, $fail failed (evidence $out)"
[ "$fail" = 0 ]
