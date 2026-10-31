#!/usr/bin/env bash
# What a player does with an item, or to a door lock, changes the level script's state in every game (bridge/remote.c).
# Two background copies, a host and a client, with the client far from the host (docs/TESTING.md), state read from the
# game processes (Lua values read back through each game's own log, and the state dumps compared by statecheck.py):
#   - level 3, Doc's leak: the host uses the duct tape at the pillar. l3_w_hub00's OnItemUsed ran on the host only: the client's
#     water flag stayed 0 and the cord item the handler spawns (obj1) never existed there, so the client could not play on.
#     Now the use is told to the client, whose handler runs with the host's puppet as the player.
#   - level 2, the entrance door of room 2: the CLIENT shoots the lock l2_r2doorlock dead. The lock's death animation
#     lets the door out of stasis (omGobExitStasis); that must happen in both games, whoever fired the shots.
#   MUTANT=noitemused  tools/qa_itemused.sh   item uses are not sent ([test] no_itemused=1, the old behaviour): level 3 must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 4 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1
# SCENARIO=leak runs the level 3 part alone, SCENARIO=lock the level 2 part (default: both)
scenario="${SCENARIO:-all}"
[ "$scenario" = lock ] && export MDK2_START='2 2' || export MDK2_START='3 6'
source "$here/mdk.sh"
case "${MUTANT:-}" in ''|noitemused) : ;; *) echo 'Unknown itemused mutant' >&2; exit 2 ;; esac
"$here/install.sh" >/dev/null || exit 1
if [ "${MUTANT:-}" = noitemused ]; then
    for i in 1 2; do printf 'no_itemused=1\r\n' >> "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.ini"; done      # the file ends in the [test] section
fi

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-itemused"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
count() { grep -acE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
q() {   # q <game> <lua value expression>: that game's value, read back from its own log
    local tag="Q$RANDOM$RANDOM"
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    wait_log "$1" "^lua: $tag " 10 >/dev/null
    grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'
}
wait_q() { local t; for t in $(seq 1 "$4"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }
# both games' value of Lua expression $1 is $2 (within $3 seconds)
both() { wait_q 1 "$1" "$2" "$3" && wait_q 2 "$1" "$2" "$3"; }
capture() {
    local tag="$1" i path t
    for i in $(mdk_seq); do
        path="$(mdk_wsl "$(mdk_dir "$i")")/coop/state_${tag}_${i}.txt"
        rm -f "$path"
        lua "$i" "coop_state(\"${tag}_${i}\")" || return 1
        for t in $(seq 1 150); do [ -f "$path" ] && break; sleep .1; done
        cp "$path" "$out/${tag}_${i}.txt" || return 1
    done
}
# the state dump of game $2 taken under tag $1 holds an item named $3
worlds_have() { grep -qaE "^item .* ${3}[[:space:]]*\$" "$out/${1}_${2}.txt"; }
worlds() { capture "$1" && python3 "$here/statecheck.py" "$out/${1}_1.txt" "$out/${1}_2.txt" --after "$out/${1}_1.txt"; }
far() { mdk_assert_far 2 "$1"; }
# start a level again on both games (the host loads it, the client follows)
reload() {
    local n1 n2 t
    n1=$(count 1 "^level $1 checkpoint $2 started"); n2=$(count 2 "^level $1 checkpoint $2 started")
    lua 1 "mdkNewGame($1, $2)"
    for t in $(seq 1 90); do
        [ "$(count 1 "^level $1 checkpoint $2 started")" -gt "$n1" ] && [ "$(count 2 "^level $1 checkpoint $2 started")" -gt "$n2" ] && break; sleep 1; done
    sleep 10
}
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
check "each game has the other's puppet" bash -c "grep -qa 'puppet spawned for player 2' '$(mdk_log 1)' && grep -qa 'puppet spawned for player 1' '$(mdk_log 2)'"
first=3; [ "$scenario" = lock ] && first=2
check "both games are in level $first"   bash -c "grep -q '^level $first checkpoint' '$(mdk_log 1)' && grep -q '^level $first checkpoint' '$(mdk_log 2)'"
if [[ "${results[-1]}" == FAIL* ]]; then echo "the games did not start together: nothing to test"; exit 1; fi
lua 1 'God(1)'; lua 2 'God(1)'

# ---- level 3: Doc's leak ----
if [ "$scenario" != lock ]; then
wait_log 1 'item use: l3_w_hub00.OnItemUsed' 40 && wait_log 2 'item use: l3_w_hub00.OnItemUsed' 40
check "both games wrapped l3_w_hub00.OnItemUsed"   bash -c "grep -qa 'item use: l3_w_hub00.OnItemUsed' '$(mdk_log 1)' && grep -qa 'item use: l3_w_hub00.OnItemUsed' '$(mdk_log 2)'"
# the client far away, on the bridge room (330+ units); the host at the pillar
lua 2 'local x, y, z = coop_room_center("l3_bridge") mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z + 1)'
lua 1 'mdkGobSetPosition(mdkGetPlayerGob(), "l3_pillarwaypnt")'
sleep 4
check "the client is outside every splash radius of the host (process state, before)" far "itemused before"
check "before: the leak is open in both games (water 0)"  both "l3_w_hub00.water or 0" 0 5
capture before_leak
check "before: no spawned cord item (obj1) in either game"  bash -c "! grep -qaE '^item .* obj1[[:space:]]*\$' '$out/before_leak_1.txt' '$out/before_leak_2.txt'"
# the host uses the duct tape on the leak: the engine runs the room's OnItemUsed with these two item types
lua 1 'l3_w_hub00.OnItemUsed(l3_w_hub00, OBJ_DUCTTAPE, OBJ_NONE)'
check "the host's leak plugged (water 1)"                 wait_q 1 "l3_w_hub00.water" 1 20
check "the client's leak plugged too (water 1)"           wait_q 2 "l3_w_hub00.water" 1 30
sleep 5
capture leak
check "the cord item (obj1) was spawned on the host"      worlds_have leak 1 obj1
check "the cord item (obj1) exists on the client too"     worlds_have leak 2 obj1
check "the worlds match: same items on both (statecheck)" worlds leak
check "the client is outside every splash radius of the host (process state, after)" far "itemused after"

fi
[ "$scenario" = leak ] && { printf '%s\n' "${results[@]}"; echo "$pass passed, $fail failed"; echo "process evidence: $out"; [ "$fail" -eq 0 ]; exit; }

# ---- level 2: the client shoots the entrance door's lock ----
[ "$scenario" = lock ] || reload 2 2
check "both games are in level 2" bash -c "[ \$(grep -ac '^level 2 checkpoint 2 started' '$(mdk_log 1)') -ge 1 ] && [ \$(grep -ac '^level 2 checkpoint 2 started' '$(mdk_log 2)') -ge 1 ]"
lua 1 'God(1)'; lua 2 'God(1)'
check "before: the door is in stasis in both games (the lock holds it)" both "coop_in_stasis(l2_r2entrdoor)" 1 20
# the room's two teleporters are not played: its exit wall is opened the way their OnDie does (in both games)
lua 1 'omAnimPlay(l2_r2exitwall, ANIM_OPEN, ANIMFLAG_NOTRANS)'; lua 2 'omAnimPlay(l2_r2exitwall, ANIM_OPEN, ANIMFLAG_NOTRANS)'
lua 2 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 52, 198, 0)'
lua 1 'local x, y, z = coop_room_center("l2_r3exit") mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z + 1)'
sleep 4
check "the client is outside every splash radius of the host (process state, before)" far "lock before"
lua 2 'mdkDealDamage(mdkGetPlayerGob(), l2_r2doorlock, 400, 1, 8)'
check "the lock is dead on the client (it fired the shots)" wait_q 2 "l2_r2doorlock and coop_alive(l2_r2doorlock)" nil 20
check "the lock is dead on the host"                        wait_q 1 "l2_r2doorlock and coop_alive(l2_r2doorlock)" nil 20
check "the door left stasis on the client"                  wait_q 2 "coop_in_stasis(l2_r2entrdoor)" 0 20
check "the door left stasis on the host"                    wait_q 1 "coop_in_stasis(l2_r2entrdoor)" 0 20
check "the client is outside every splash radius of the host (process state, after)" far "lock after"
# the host goes through the door and on (the door opens and closes in both games), the client comes later
lua 1 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 49, 216, 0)'
sleep 6
lua 1 'local x, y, z = coop_room_center("l2_r3exit") mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z + 1)'
sleep "${LOCK_WAIT:-30}"
echo "probe: door open/stasis/locked on the client: $(q 2 'coop_door_open(l2_r2entrdoor)') $(q 2 'coop_in_stasis(l2_r2entrdoor)') $(q 2 'coop_door_locked(l2_r2entrdoor)')"
echo "probe: door open/stasis/locked on the host:   $(q 1 'coop_door_open(l2_r2entrdoor)') $(q 1 'coop_in_stasis(l2_r2entrdoor)') $(q 1 'coop_door_locked(l2_r2entrdoor)')"
# the client was away for a while (the door's room is not near anyone), then comes to the door
lua 2 'local x, y, z = coop_room_center("l2_r1") mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z + 1)'
sleep "${LOCK_AWAY:-40}"
echo "probe: away: door open/stasis/locked on the client: $(q 2 'coop_door_open(l2_r2entrdoor)') $(q 2 'coop_in_stasis(l2_r2entrdoor)') $(q 2 'coop_door_locked(l2_r2entrdoor)')"
lua 2 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 49, 216, 0)'
check "the door opens for the client's own hero (it was away from the door's room for a while)" wait_q 2 "coop_door_open(l2_r2entrdoor)" 1 20
echo "probe: back: door open/stasis/locked on the client: $(q 2 'coop_door_open(l2_r2entrdoor)') $(q 2 'coop_in_stasis(l2_r2entrdoor)') $(q 2 'coop_door_locked(l2_r2entrdoor)')"

printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"; echo "process evidence: $out"
[ "$fail" -eq 0 ]
