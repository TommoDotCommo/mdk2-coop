#!/usr/bin/env bash
# The torpedo minigames of levels 2 (checkpoint 13), 5 (11) and 9 (13) with two real games: both players are in the
# minigame and each sees the other's ship as a ghost (screenshot as evidence), then both win it and both games
# live on into the rest of the movie. One boot per level. Needs isolated game copies.
#   tools/qa_minigames.sh      (about 7 minutes)
#   MUTANT=noguard tools/qa_minigames.sh   the bridge's emitter guard off (frame.c): passes only if the client's
#                              crash at the end of level 2's minigame is caught
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
out="$here/../local/qa-minigames/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
pass=0; fail=0; results=()
trap 'mdk_kill' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
check() {
    local name=$1; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has() { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i=$1 pattern=$2 deadline=$((SECONDS + $3)); while [ "$SECONDS" -le "$deadline" ]; do has "$i" "$pattern" && return 0; sleep 0.5; done; return 1; }
# in the minigame once Save.inminigame says so; skip votes carry the movie up to it
reach() {
    local deadline=$((SECONDS + 120)) i
    while [ "$SECONDS" -le "$deadline" ]; do
        for i in 1 2; do mdk_cmd "$(mdk_pid $i)" 'if Save.inminigame then coop_log("T mini".."game") end'; done
        has 1 'lua: T minigame' && has 2 'lua: T minigame' && return 0
        mdk_press "$(mdk_pid 1)" 14 1 100 200; mdk_press "$(mdk_pid 2)" 14 1 100 200
        sleep 4
    done
    return 1
}
# win the minigame through the game's own end-of-course hook, once it runs undecided (its start resets the result)
win() {
    local i=$1 level=$2 code deadline=$((SECONDS + 30))
    case $level in
        2) code='if (torpedogame.ready and torpgob.result == 0 and not torpgob.dead) then tpgame_timer.OnCustomKey(tpgame_timer, 0, CUSTKEY_USER1); coop_log("T won") end' ;;
        5) code='if (podgob.result == 0 and not podgob.dead) then pd_gametimer.OnAnimLoop(pd_gametimer, ANIM_ACTION00); coop_log("T won") end' ;;
        9) code='if (jdgob.result == 0 and not jdgob.dead) then jdgame_timer.OnAnimLoop(jdgame_timer, ANIM_ACTION00); coop_log("T won") end' ;;
    esac
    while [ "$SECONDS" -le "$deadline" ]; do
        mdk_cmd "$(mdk_pid "$i")" "if (Save and Save.inminigame) then $code end"
        has "$i" '^lua: T won' && return 0
        sleep 1
    done
    return 1
}
both_live_on() { sleep 15; mdk_alive "$(mdk_pid 1)" && mdk_alive "$(mdk_pid 2)"; }
both_go() { wait_log 1 'movie 1: go' 60 && wait_log 2 'movie 1: (go|the host had already said go)' 60; }
run_level() {
    local level=$1 cp=$2
    MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || return 1
    if [ "${MUTANT:-}" = noguard ]; then      # off from the start: the bad count appears while the level loads
        for i in 1 2; do sed -i 's/^\[test\]\r$/[test]\r\nemitter_guard=0\r/' "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.ini"; done
    fi
    mdk_boot
    check "level $level: both games play the movie" both_go
    check "level $level: both reach the minigame" reach
    check "level $level: host sees the client's ship" wait_log 1 "minigame: player 2 plays beside you \\(level $level\\)" 15
    check "level $level: client sees the host's ship" wait_log 2 "minigame: player 1 plays beside you \\(level $level\\)" 15
    # the client steers its ship aside, so its ghost leaves the host's ship (evidence: the screenshot)
    case $level in
        2) nudge='torpgob.z = 8' ;;
        5) nudge='podgob.x = 688; podgob.z = 7' ;;
        9) nudge='jdgob.z = 8; jdgob.x = 0' ;;
    esac
    mdk_cmd "$(mdk_pid 2)" "$nudge"; sleep 1
    mdk_shot "$(mdk_pid 1)" "$out/level${level}_host.png" || true
    # the end of the minigame: the client crashed here at level 2 (an emitter of the movie camera with a negative
    # count; bridge frame.c resets it)
    check "level $level: host wins the minigame" win 1 "$level"
    check "level $level: client wins the minigame" win 2 "$level"
    check "level $level: both games survive the end of the minigame" both_live_on
    check "level $level: no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
    for i in 1 2; do cp "$(mdk_log $i)" "$out/level${level}_game$i.log" 2>/dev/null; done
    mdk_kill
}
run_level 2 13
run_level 5 11
run_level 9 13
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs and screenshots: $out"
if [ "${MUTANT:-}" = noguard ]; then
    printf '%s\n' "${results[@]}" | grep -qF "FAIL  level 2: both games survive the end of the minigame" &&
        { echo "mutant noguard caught"; exit 0; }
    echo "mutant noguard NOT caught"; exit 1
fi
[ "$fail" = 0 ]
