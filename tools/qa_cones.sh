#!/usr/bin/env bash
# Level 8's cone creatures and a client (level 8 checkpoint 1, two games, about a minute). The level opens with its
# script scaring every cone creature (Level.ConeScared), and each deletes itself some two seconds later. The host's
# deletions reach a client as despawns; a client whose own intro ran later than that (a slower load) then called
# mdkSetLuaEvent on a creature already gone and crashed at mdk2hd.exe+0xd055, reading 0x84 (found by the campaign sweep
# on a loaded machine). Here the creature is gone in both games first, then the client runs the level's own call.
#   MUTANT=unsafe   the client keeps the level's own function ([test] conesafe=0): it must crash reading 0x84
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START="8 1"
case "${MUTANT:-}" in ''|unsafe) : ;; *) echo 'Unknown mutant' >&2; exit 2 ;; esac
source "$here/mdk.sh"
"$here/install.sh" >/dev/null || exit 1
d2="$(mdk_wsl "$(mdk_dir 2)")"
[ "${MUTANT:-}" = unsafe ] && printf 'conesafe=0\r\n' >> "$d2/coop/coop.ini"      # the file ends in the [test] section
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-cones"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
has() { grep -qaE "$1" "$(mdk_log 2)"; }
note() { tr -d '\r' < "$d2/coop/note_cone.txt" 2>/dev/null; }
wait_for() { local i; for i in $(seq 1 "$(( $2 * 2 ))"); do eval "$1" && return 0; sleep 0.5; done; return 1; }
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
P1=$(mdk_pid 1); P2=$(mdk_pid 2)
check "both games are in level 8" bash -c "grep -qa '^level 8 checkpoint' '$(mdk_log 1)' && grep -qa '^level 8 checkpoint' '$(mdk_log 2)'"
sleep 12                                                  # the level's own scare has run in both games
mdk_cmd "$P1" 'if (coop_alive(l8r1_cc08)) then omGobDelete(l8r1_cc08) end'
gone() { rm -f "$d2/coop/note_cone.txt"; mdk_cmd "$P2" 'coop_note("cone", format("alive %d", coop_alive(l8r1_cc08) or 0))'; sleep 1; note | grep -q '^alive 0'; }
check "the creature is gone in the client's game"         wait_for gone 20
[ "${MUTANT:-}" = unsafe ] && mdk_crash_expect 1
mdk_cmd "$P2" 'Level.ConeScared(l8r1_cc08, 1.5)'
if [ "${MUTANT:-}" = unsafe ]; then
    check "the client crashed in the level's call"        wait_for "has 'crash: exception 0xc0000005 at mdk2hd.exe\\+0xd0[0-9a-f]{2} .*reading 0x00000084'" 40
    mdk_crash_scan
else
    check "the client skipped the creature"               wait_for "has '^lua: level 8: cone script skipped a creature that is already gone'" 10
    sleep 5
    mdk_alive "$P1" && mdk_alive "$P2"; alive=$?
    check "both games are still running"                  test "$alive" = 0
    check "no Lua error in the client's game"             bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 2)'"
fi
mdk_crash_scan
[ "${MUTANT:-}" = unsafe ] && mdk_crash_collect "$out/dumps"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
echo "process evidence: $out"
mdk_crash_report
[ "$fail" -eq 0 ]
