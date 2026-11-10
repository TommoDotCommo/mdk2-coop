#!/usr/bin/env bash
# The engine's per-frame scene pass walks the scene's gob handle vector twice; the second walk used the handle lookup's
# answer without a null check, so a handle left behind by a destroyed gob (a shot-up scenery object) crashed the host at
# mdk2hd.exe+0x570bd, reading address 0x64 (a level 4 run, the sniper hitting scenery). The bridge patches the second
# walk to skip such a handle the way the first one does. The test plants a handle no gob has at the end of the vector.
#   MDK2_N=1 tools/qa_scene_guard.sh                    guard on: the handle is skipped (counter > 0), the game stays up
#   MUTANT=unguarded MDK2_N=1 tools/qa_scene_guard.sh   guard taken out (coop_sceneguard(0)): the game must crash at 0x570bd
# Either build (the fault was seen in the Overhaul build at +0x570bd; the Steam build has the same code at +0x570dd;
# the bridge checks the code bytes first). About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
level="${LEVEL:-2}"; cp="${CP:-4}"
MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || exit 1
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-sceneguard"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
d="$(mdk_wsl "$(mdk_dir 1)")"; log="$d/coop/bridge.log"
has() { grep -qE "$1" "$log"; }
note() { tr -d "\r" < "$d/coop/note_scene.txt" 2>/dev/null; }
wait_for() { local i; for i in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
mdk_kill; sleep 1
mdk_boot
check "game reached level $level checkpoint $cp"    wait_for "has '^level $level checkpoint $cp started'" 90
check "the scene pass guard is installed"           has 'scene guard installed'
sleep 3
rm -f "$d/coop/note_scene.txt"
[ "${MUTANT:-}" = unguarded ] && { mdk_crash_expect 1; mdk_cmd "$(mdk_pid 1)" 'coop_sceneguard(0)'; }
mdk_cmd "$(mdk_pid 1)" 'coop_note("scene", format("planted %d", coop_test_stale_scene(1)))'
if [ "${MUTANT:-}" = unguarded ]; then
    check "the game crashed in the scene pass"      wait_for "has 'crash: exception 0xc0000005 at mdk2hd.exe\\+0x570[bd]d'" 40
    mdk_crash_scan
    check "the process is gone"                     wait_for "! mdk_alive $(mdk_pid 1)" 20
else
    check "a stale handle was planted"              wait_for "note | grep -q '^planted 1'" 10
    sleep 8
    check "the game is still running"               mdk_alive "$(mdk_pid 1)"
    mdk_cmd "$(mdk_pid 1)" 'coop_note("scene", format("skipped %d", coop_scene_skipped()))'
    check "the guard stepped over the handle"       wait_for "note | grep -qE '^skipped [1-9]'" 10
    check "the stale handle was reported with its number" wait_for "has 'scene guard: stale handle 7ffe0005 .*deleted by the bridge: no'" 10
    mdk_cmd "$(mdk_pid 1)" 'coop_test_stale_scene(0)'
    sleep 3
    check "the game runs on with the handle removed" mdk_alive "$(mdk_pid 1)"
fi
cp "$log" "$out/bridge.log" 2>/dev/null
mdk_crash_scan
[ "${MUTANT:-}" = unguarded ] && mdk_crash_collect "$out/dumps"
mdk_kill
printf '%s\n' "${results[@]}"; echo "$pass passed, $fail failed (evidence $out)"
[ "$fail" = 0 ]
