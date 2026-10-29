#!/usr/bin/env bash
# Animation lookups must survive models that have no animation table (level 2 exits, triggers): the game's own
# lookup reads address <id> for them and the Overhaul build answers with "Jinkies! There was an error" and a dump.
# Net sync, snapshots and the emote code ask every gob, so this ran into the crash at the first synced exit.
# The game's per-frame animation update keeps at most 20 live states of a gob on its stack; the 21st corrupts the
# index list and the next state is advanced with a pointer as its index. The bridge trims surplus states first.
# Boots two games into level 2 and runs the bridge's capture/apply over every gob with a controller on both.
#   MDK2_BUILD=overhaul tools/qa_animguard.sh        (the build players run; see tools/mdk.sh)
#   MUTANT=rawlookup  switches the lookup guard off: the game must crash (the crash guard is told to expect it)
#   MUTANT=nofloodtrim  switches the update-overflow guard off: a gob with 30 live animation states must crash the
#                     game itself (not the bridge) inside its animation update, with a state pointer used as index
#                     (the mechanism of the owner's first dump)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="2 7" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-animguard"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
alive() { mdk_alive "$(mdk_pid "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do grep -qaE "$2" "$(mdk_log "$1")" && return 0; sleep 1; done; return 1; }
trap 'mdk_kill' EXIT

mdk_boot
check "both peers loaded level 2"                  bash -c "grep -qaE '^level 2 checkpoint 7 started' '$(mdk_log 1)' '$(mdk_log 2)'"
sleep 3

if [ "${MUTANT:-}" = rawlookup ]; then
    mdk_crash_expect 1
    lua 1 'coop_animguard(0)'
    lua 1 'coop_animscan()'
    wait_for_death() { local i; for i in $(seq 1 30); do mdk_crash_scan; alive 1 || return 0; sleep 1; done; return 1; }
    if wait_for_death; then mdk_crash_collect "$out/dumps"; echo "mutant $MUTANT caught"; exit 0; fi
    echo "mutant $MUTANT NOT caught: the unguarded lookup did not crash"; exit 1
fi

if [ "${MUTANT:-}" = nofloodtrim ]; then
    mdk_crash_expect 1
    lua 1 'coop_animtrim(0)'
    lua 1 'coop_animflood(mdkGetPlayerGob(), 30)'
    for i in $(seq 1 30); do mdk_crash_scan; alive 1 || break; sleep 1; done
    if ! alive 1 && grep -qaE '^crash: exception 0xc0000005 at mdk2hd.exe\+0x[0-9a-f]+' "$(mdk_log 1)"; then
        mdk_crash_collect "$out/dumps"; cp "$(mdk_log 1)" "$out/game1.log"; echo "mutant $MUTANT caught"; exit 0
    fi
    echo "mutant $MUTANT NOT caught: the overflowing animation update did not crash"; exit 1
fi

for i in 1 2; do
    lua "$i" 'coop_animscan()'
    check "game $i scanned its gobs and none had to be skipped by a crash" wait_log "$i" '^animscan: [0-9]+ gobs with an animation controller' 15
    # non-vacuous both ways: some gobs have no animations at all (the crash), and some are really animating
    # (a guard that refuses everything would pass the first alone and silence every synced animation)
    check "game $i has gobs without an animation table (the case under test)" bash -c "grep -aE '^animscan: ' '$(mdk_log "$i")' | tail -1 | grep -qvE ', 0 without'"
    check "game $i still finds the animations of gobs that have them" bash -c "grep -aE '^animscan: ' '$(mdk_log "$i")' | tail -1 | grep -qvE ', 0 playing'"
    lua "$i" 'coop_animdump(mdkGetPlayerGob())'; sleep 1
    check "game $i names the player's current animation" bash -c "grep -aE '^animdump ' '$(mdk_log "$i")' | tail -1 | grep -qE '\\(anim [0-9]+\\)'"
    lua "$i" 'coop_animdump(l2_r6exit03)'
    sleep 1
    check "game $i survives asking the exit for its animations" alive "$i"
done
lua 1 'coop_animflood(mdkGetPlayerGob(), 30)'
sleep 3
check "the trimmed gob is logged"                  wait_log 1 '^anim guard: stopped [0-9]+ surplus animation states' 5
check "the host survived the flood"                alive 1
check "no crash lines in either log"               bash -c "! grep -qa '^crash:' '$(mdk_log 1)' '$(mdk_log 2)'"
for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
