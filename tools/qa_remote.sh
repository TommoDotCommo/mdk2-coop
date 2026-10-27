#!/usr/bin/env bash
# What one player does reaches the other players' scripts (bridge/remote.c), with a host and a client on level 1:
#   - a pickup: the client takes level 1's mortars; the host's copy goes, and so does the item's script state, because
#     the item's OnPickedUp ran there too (it used to run only where the item was taken)
#   - a room: when anyone enters level 1's l1_con2a02, its OnEnterRoom (which locks the door l1_con2adr01 behind them)
#     runs on every game, once; the client entering and the host entering are both tried
#   - an area that closes: whoever is outside it, or on the far side of the door that locks, is brought to the one
#     who triggered it, with a line saying so; a player on the triggerer's side stays put; a player in a minigame or
#     a cutscene is moved when it is over
#   - tutorial grenades: everyone receives the reward before their inventory handler removes four
#   MUTANT=noremote tools/qa_remote.sh    the bridge queues nothing for other players' events: must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 1" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-remote"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
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
to() { lua "$1" "mdkGobSetPosition(mdkGetPlayerGob(), \"$2\")"; }     # that game's own player to a named place
inroom() { lua "$1" "local x, y, z = coop_room_center(\"$2\") if (x) then mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z) else coop_log(\"no room $2\") end"; }
# the distance, in game $1, between its own player and player $2's puppet
dist() {
    lua "$1" "function qa_dist(n) local a = mdkGetPlayerGob() local b = coop_gob(\"@p\" .. n) if (not a or not b) then return -1 end local x = a.position.x - b.position.x local y = a.position.y - b.position.y local z = a.position.z - b.position.z return floor(sqrt(x*x + y*y + z*z)) end"
    q "$1" "qa_dist($2)"
}
# start level 1 again on both games and let the puppets settle (room tracking takes a first sighting as the start)
reload() {
    local n1 n2
    n1=$(count 1 '^level 1 checkpoint 1 started'); n2=$(count 2 '^level 1 checkpoint 1 started')
    lua 1 'mdkNewGame(1, 1)'
    local t; for t in $(seq 1 60); do
        [ "$(count 1 '^level 1 checkpoint 1 started')" -gt "$n1" ] && [ "$(count 2 '^level 1 checkpoint 1 started')" -gt "$n2" ] && break; sleep 1; done
    sleep 9
}
trap 'mdk_kill' EXIT

mdk_boot
[ "${MUTANT:-}" = noremote ] && for i in 1 2; do lua "$i" 'coop_test_noremote(1)'; done
# the room tracking takes a puppet's first sighting as where it starts, so let each game see the other settle first
check "each game has the other's puppet"                bash -c "grep -qa 'puppet spawned for player 2' '$(mdk_log 1)' && grep -qa 'puppet spawned for player 1' '$(mdk_log 2)'"
sleep 6
check "both games are in level 1"                       bash -c "grep -q '^level 1 checkpoint' '$(mdk_log 1)' && grep -q '^level 1 checkpoint' '$(mdk_log 2)'"
if [[ "${results[-1]}" == FAIL* ]]; then printf '%s\n' "${results[@]}"; echo "the games did not start together: nothing to test"; exit 1; fi

# 1. a pickup: the client walks into room 7, which makes the mortars item, and takes it
inroom 2 l1_r7
check "the host ran room 7's OnEnterRoom for the client" wait_log 1 '^(lua: )?script event L1: OnEnterRoom l1_r7 \(remote, player 2\)' 15
check "so the host has the mortars item too"            wait_q 1 'type(l1_r7.mortars)' userdata 10
to 2 l1_r7mortar01
check "the client picked the mortars up"                wait_log 2 '^pickup mortars' 20
check "the host ran the mortars' OnPickedUp"            wait_log 1 '^(lua: )?script event L1: OnPickedUp mortars \(remote, player 1\)' 15
check "the host's script state followed (l1_r7.mortars cleared)" wait_q 1 'type(l1_r7.mortars)' nil 15
check "the host removed its copy of the item"           wait_log 1 'peer picked up mortars: removing it here after its script' 5
check "no Lua errors yet"                               bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"

# 2. a room the client enters; the host is far away, in level 1's last area, and is brought to the client
inroom 1 l1_r8; inroom 2 l1_r8; sleep 3
d0=$(dist 1 2)
check "both in the far room before ($d0 apart at most 15)" test "$d0" -le 15 -a "$d0" -ge 0
check "the door behind the room is open on both games before" bash -c "[ \"$(q 1 'coop_door_locked(l1_con2adr01)')\" = 0 ] && [ \"$(q 2 'coop_door_locked(l1_con2adr01)')\" = 0 ]"
inroom 2 l1_con2a02
check "the client's own game ran the room's handler"    wait_log 2 '^script event L1: OnEnterRoom l1_con2a02[[:space:]]*$' 10
check "the host ran it too, for player 2"               wait_log 1 '^(lua: )?script event L1: OnEnterRoom l1_con2a02 \(remote, player 2\)' 15
check "the door locked on the client"                   wait_q 2 'coop_door_locked(l1_con2adr01)' 1 10
check "the door locked on the host"                     wait_q 1 'coop_door_locked(l1_con2adr01)' 1 10
check "once on each game, not twice"                    bash -c "[ $(count 1 'script event L1: OnEnterRoom l1_con2a02') = 1 ] && [ $(count 2 'script event L1: OnEnterRoom l1_con2a02') = 1 ]"
check "the host was outside and was brought to the client" wait_log 1 'closed an area behind player 2 and we are outside it' 10
sleep 2
d1=$(dist 1 2)
check "the host stands next to the client now ($d1)"    test "$d1" -le 6 -a "$d1" -ge 0

# 3. the other way: a fresh level, the host enters the room and the client is far away
lua 1 'mdkNewGame(1, 1)'
check "the host started level 1 again"                  wait_log 1 '^level 1 checkpoint 1 started' 40
check "the client followed"                             wait_log 2 '^level 1 checkpoint 1 started' 40
sleep 8
inroom 2 l1_r8; sleep 3
inroom 1 l1_con2a02
check "the client ran the room's handler for player 1"  wait_log 2 '^(lua: )?script event L1: OnEnterRoom l1_con2a02 \(remote, player 1\)' 15
check "the door locked on the client"                   wait_q 2 'coop_door_locked(l1_con2adr01)' 1 10
check "the client was outside and joined the host"      wait_log 2 'closed an area behind player 1 and we are outside it' 10
sleep 2
d2=$(dist 2 1)
check "the client stands next to the host now ($d2)"    test "$d2" -le 6 -a "$d2" -ge 0
check "the host did not move itself (it entered by choice)" bash -c "! grep -qa 'closed an area behind player 2' <(tail -n 40 '$(mdk_log 1)')"
# 4. the client is inside the zone but on the far side of the door that locks (room 01 of the corridor, behind the door)
reload
inroom 2 l1_con2a01; sleep 3
inroom 1 l1_con2a02
check "the client was cut off by the locked door and joined the host" wait_log 2 'closed an area behind player 1 and we are outside it \(on the far side of a locked door\)' 15
sleep 2
d3=$(dist 2 1)
check "the client stands next to the host now ($d3)"    test "$d3" -le 6 -a "$d3" -ge 0
check "the client was told"                             wait_log 2 '^chat: \(game\): The way back closed: you joined' 5

# 5. the door test itself, with made-up places (a player slides about in a corridor, so the real thing cannot be put
# on the triggerer's side on purpose): a door between two players is found, a door behind one of them or off to the
# side is not
lua 2 'qa_door = { position = { x = 0, y = 0, z = 0 } } function qa_at(x, y, z) return { position = { x = x, y = y, z = z } } end'
check "a door between two players counts"               test "$(q 2 'coop.DoorBetween(qa_door, qa_at(0, 0, -20), qa_at(0, 0, 30))')" = 1
check "a door behind both of them does not"             test "$(q 2 'coop.DoorBetween(qa_door, qa_at(0, 0, 10), qa_at(0, 0, 30))')" = nil
check "a door behind the other player does not"         test "$(q 2 'coop.DoorBetween(qa_door, qa_at(0, 0, -30), qa_at(0, 0, -10))')" = nil
check "a door well off to the side does not"            test "$(q 2 'coop.DoorBetween(qa_door, qa_at(40, 0, -20), qa_at(40, 0, 30))')" = nil
check "a door a little off the line still counts"       test "$(q 2 'coop.DoorBetween(qa_door, qa_at(4, 0, -20), qa_at(4, 0, 30))')" = 1

# 6. the client is busy (a minigame): it is moved when it is over, not before
reload
inroom 2 l1_r8; sleep 3
lua 2 'Save.inminigame = 1'
inroom 1 l1_con2a02
check "the client ran the room's handler"               wait_log 2 '^(lua: )?script event L1: OnEnterRoom l1_con2a02 \(remote, player 1\)' 15
sleep 5
d5=$(dist 2 1)
check "a client in a minigame is left alone ($d5 apart)" test "$d5" -ge 40
lua 2 'Save.inminigame = nil'
check "once it is over, the client joins the host"      wait_log 2 'closed an area behind player 1 and we are outside it' 15
sleep 2
d6=$(dist 2 1)
check "and stands next to the host ($d6)"               test "$d6" -le 6 -a "$d6" -ge 0

# 7. Shared tutorial grenades: both players receive five, then their own handler
# removes four. This must happen after the reward is granted, once per player.
reload
for i in 1 2; do
    lua "$i" 'qa_tutorial_before=coop_item_count(mdkGetPlayerGob(),OBJ_GRENADEITEM) qa_tutorial_runs=0 grenades=mdkCreateObjectLua("grenades",OBJ_GRENADEITEM,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) mdkGobSetPosition(grenades,"l1_r1grengen") grenades.OnPickedUp=function(gob,id) mdkKurtRemoveItem(mdkGetPlayerGob(),id,4) qa_tutorial_runs=qa_tutorial_runs+1 end'
done
check "both games have the grenades"                    bash -c "[ \"$(q 1 'coop_alive(grenades)')\" = 1 ] && [ \"$(q 2 'coop_alive(grenades)')\" = 1 ]"
to 2 l1_r1grengen
check "the client took its grenades"                    wait_log 2 '^pickup grenades ' 15
for i in 1 2; do
    check "player $i received one tutorial grenade after the script" wait_q "$i" 'coop_item_count(mdkGetPlayerGob(),OBJ_GRENADEITEM)-qa_tutorial_before' 1 15
    check "player $i ran its tutorial handler once" wait_q "$i" 'qa_tutorial_runs' 1 10
done
check "no Lua errors in these parts"                    bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"

check "both games still running"                        bash -c "for p in $(mdk_pids | tr '\n' ' '); do powershell.exe -NoProfile -Command \"if (-not (Get-Process -Id \$p -ErrorAction SilentlyContinue)) { exit 1 }\" || exit 1; done"
check "no Lua errors anywhere"                          bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
