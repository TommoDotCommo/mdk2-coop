#!/usr/bin/env bash
# The torpedo minigames of levels 2 (checkpoint 13), 5 (11) and 9 (13) with two real games: both players are in the
# minigame and each sees the other's ship as a ghost (screenshot as evidence). One boot per level. Needs a lab lease.
#   tools/qa_minigames.sh      (about 6 minutes)
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
both_go() { wait_log 1 'movie 1: go' 60 && wait_log 2 'movie 1: (go|the host had already said go)' 60; }
run_level() {
    local level=$1 cp=$2
    MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || return 1
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
    check "level $level: no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
    for i in 1 2; do cp "$(mdk_log $i)" "$out/level${level}_game$i.log" 2>/dev/null; done
    mdk_kill
}
run_level 2 13
run_level 5 11
run_level 9 13
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs and screenshots: $out"
[ "$fail" = 0 ]
