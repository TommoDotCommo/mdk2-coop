#!/usr/bin/env bash
# Character wheel in one background game: hold J (DIK 36), steer with the mouse, let go to switch hero;
# "Level's own" (and the next level load, with [game] char_follow on) puts the game's hero back.
# Needs a lab lease.
#   MDK2_N=1 tools/qa_char.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-1}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-char"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
lua()  { mdk_cmd "$(mdk_pid 1)" "$1"; }
log()  { mdk_log 1; }
# the type of the player gob (-1 if none)
ptype() { lua 'local p = mdkGetPlayerGob() if p then coop_log(format("PTYPE %d", mdkGetGobType(p))) else coop_log("PTYPE -1") end'
    sleep 1; grep -E "lua: PTYPE " "$(log)" | tail -1 | tr -d '\r' | awk '{print $NF}'; }
# hold J, move the mouse (dx, dy), let go
pick() { lua "coop_press(36, 2500)"; sleep 0.6; lua "coop_mouse($1, $2)"; sleep 2.6; }

mdk_boot
for i in $(seq 1 90); do grep -q "^level 1 checkpoint 2 started" "$(log)" 2>/dev/null && break; sleep 2; done
sleep 6
kurt=$(lua 'coop_log(format("OBJ %d %d %d", OBJ_KURT, OBJ_MAX, OBJ_DOC))' >/dev/null; sleep 1; grep -E "lua: OBJ " "$(log)" | tail -1 | tr -d '\r')
K=$(echo "$kurt" | awk '{print $3}'); M=$(echo "$kurt" | awk '{print $4}'); D=$(echo "$kurt" | awk '{print $5}')
check "starts as the level's hero (Kurt $K)"            test "$(ptype)" = "$K"

# slots clockwise from the top: Kurt, Max (right), Doc (down), Level's own (left)
pick 80 0
check "wheel right switches to Max ($M)"                test "$(ptype)" = "$M"
pick 0 80
check "wheel down switches to Doc ($D)"                 test "$(ptype)" = "$D"
pick 0 -80
check "wheel up switches to Kurt ($K)"                  test "$(ptype)" = "$K"
pick 80 0
pick -80 0
check "wheel left is the level's own hero ($K)"         test "$(ptype)" = "$K"

# the choice survives a level load only with char_follow off
lua 'coop_set("game", "char_follow", 0)'
pick 80 0
lua 'mdkNewGame(1, 3)'; sleep 25
check "char_follow off: the choice is kept after a load ($M)"  test "$(ptype)" = "$M"
lua 'coop_set("game", "char_follow", 1)'
lua 'mdkNewGame(1, 2)'; sleep 25
check "char_follow on: the game's hero is back ($K)"    test "$(ptype)" = "$K"

check "game running"                                    bash -c "$(declare -f mdk_alive); mdk_alive $(mdk_pid 1)"
check "no Lua errors"                                   bash -c "! grep -q 'LUA ERROR' '$(log)'"
cp "$(log)" "$out/game1.log"
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
