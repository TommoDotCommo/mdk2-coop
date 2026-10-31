#!/usr/bin/env bash
# The bot's line-of-sight probes against a recycled address (mdkAILineOfSight faulted at mdk2hd.exe+0x2a95, reading 8):
# the pilot keeps an invisible probe gob in a Lua global; a level load frees it and the engine may reuse the address
# for another gob with no game object (a level's sign or light), which the global still "finds" in the gob table.
# The test makes coop.pilot_probe point at such a gob (a live level gob whose game object is taken away) and asks the
# pilot for a line of sight.
#   MDK2_N=1 tools/qa_stale_probe.sh                  probe identity checks on: a new probe is made, the game stays up
#   MUTANT=unchecked MDK2_N=1 tools/qa_stale_probe.sh checks off (coop.test_unchecked): the game must crash at 0x2a95
# Overhaul build only (the fault site is that build's). About 2 minutes. PLANT=<name part> picks the gob.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
level="${LEVEL:-2}"; cp="${CP:-9}"; plant="${PLANT:-l2_roomlight32}"
MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || exit 1
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-staleprobe"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
d="$(mdk_wsl "$(mdk_dir 1)")"; log="$d/coop/bridge.log"
has() { grep -qE "$1" "$log"; }
note() { tr -d "\r" < "$d/coop/note_stale.txt" 2>/dev/null; }
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
mdk_kill; sleep 1
mdk_boot
check "game reached level $level checkpoint $cp"    wait_for "has '^level $level checkpoint $cp started'" 90
sleep 5
rm -f "$d/coop/note_stale.txt"
# the pilot makes its probe, a real one
mdk_cmd "$(mdk_pid 1)" 'coop.PilotSees(mdkGetPlayerGob(), 1342, 899, 240) coop_note("stale", format("probe %d", coop_gob_ok(coop.pilot_probe, "coop_pilot_probe") or 0))'
check "the pilot's probe is named and has a game object" wait_for "note | grep -q '^probe 1'" 10
[ "${MUTANT:-}" = unchecked ] && { mdk_crash_expect 1; mdk_cmd "$(mdk_pid 1)" 'coop.test_unchecked = 1'; }
rm -f "$d/coop/note_stale.txt"
# the stale reference: a live level gob without a game object where the probe was
mdk_cmd "$(mdk_pid 1)" "coop.stale = coop_test_gob(\"$plant\", 1) coop.pilot_probe = coop.stale coop.PilotSees(mdkGetPlayerGob(), 1342, 899, 240) coop_note(\"stale\", format(\"asked %d %d\", coop.pilot_probe ~= coop.stale, coop_gob_ok(coop.pilot_probe, \"coop_pilot_probe\") or 0))"
if [ "${MUTANT:-}" = unchecked ]; then
    check "the game crashed at the sight check"     wait_for "has 'crash: sight target|0x2a95|mdk2hd.exe\\+0x2a95'" 40
    mdk_crash_scan
    check "the process is gone"                     wait_for "! mdk_alive $(mdk_pid 1)" 20
else
    check "a new probe replaced the stale one"      wait_for "note | grep -q '^asked 1 1'" 15
    check "the stale gob has its game object back"  mdk_cmd "$(mdk_pid 1)" 'coop_test_gob("'"$plant"'", 0)'
    sleep 15
    check "the game is still running"               mdk_alive "$(mdk_pid 1)"
fi
cp "$log" "$out/bridge.log" 2>/dev/null
mdk_crash_scan
[ "${MUTANT:-}" = unchecked ] && mdk_crash_collect "$out/dumps"
mdk_kill
printf '%s\n' "${results[@]}"; echo "passed $pass failed $fail (evidence $out)"
[ "$fail" = 0 ]
