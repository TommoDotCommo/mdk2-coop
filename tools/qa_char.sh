#!/usr/bin/env bash
# Character wheel in one background game: hold J (DIK 36), steer with the mouse, let go to switch hero;
# "Level's own" (and the next level load, with [game] char_follow on) puts the game's hero back.
# Needs isolated game copies.
#   MDK2_N=1 tools/qa_char.sh
#   MUTANT=nounstick MDK2_N=1 tools/qa_char.sh   no unsticking after a switch: passes only if the stuck-at-a-wall
#                                                check catches it
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-1}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null
if [ "${MUTANT:-}" = nounstick ]; then
    f="$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
    python3 - "$f" <<'MUT' || exit 2
import sys
path = sys.argv[1]
old = 'coop.unstick = {'
data = open(path, newline='').read()
if old not in data:
    sys.exit('mutant: text not found')
open(path, 'w', newline='').write(data.replace(old, 'coop.unstick_off = {'))
MUT
fi

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

# hp: half of Kurt's goes over as half of Max's (a hero's max hp is read from the fresh one)
hpof() { lua 'coop_log(format("HP %d", coop_hp(mdkGetPlayerGob())))'; sleep 1; grep -E "lua: HP " "$(log)" | tail -1 | tr -d '\r' | awk '{print $NF}'; }
pick -80 0; pick 0 -80                                  # Kurt, full hp
full=$(hpof)
lua 'local p = mdkGetPlayerGob() mdkDealDamage(nil, p, coop_hp(p) / 2, DAMAGE_BADGUY, -1)'; sleep 1
half=$(hpof)
pick 80 0
pct=$(grep -E "hp carried over, [0-9]+%" "$(log)" | tail -1 | tr -d '\r' | sed 's/.*over, //; s/%//')
check "Max keeps Kurt's share of hp (Kurt $half/$full, Max ${pct}%)"  python3 -c "import sys; sys.exit(0 if 40 <= int('${pct:-0}') <= 60 else 1)"
check "Max's hp is a real value ($(hpof))"                test "$(hpof)" -gt 0

# the choice survives a level load only with char_follow off
lua 'coop_set("game", "char_follow", 0)'
pick 80 0
lua 'mdkNewGame(1, 3)'; sleep 25
check "char_follow off: the choice is kept after a load ($M)"  test "$(ptype)" = "$M"
lua 'coop_set("game", "char_follow", 1)'
lua 'mdkNewGame(1, 2)'; sleep 25
check "char_follow on: the game's hero is back ($K)"    test "$(ptype)" = "$K"

# Doc walks left into the wall at 1-1 (x -39), then becomes Kurt, who is wider: on Doc's spot he cannot move at
# all; held move keys put him on the next spot around until he moves, even when the player waits before walking
pos() { local n; n=$(wc -l < "$(log)"); lua 'local x, y = coop_pose(mdkGetPlayerGob()) coop_log(format("T pos %.2f %.2f", x, y))'
        sleep 0.5; tail -n +$((n + 1)) "$(log)" | grep -oE 'T pos .*' | tail -1 | awk '{print $3, $4}'; }
lua 'mdkNewGame(1, 1)'; sleep 25
lua "coop.SwitchChar(mdkGetPlayerGob(), $D)"; sleep 3
mdk_press "$(mdk_pid 1)" 30 1 6000 200; sleep 0.5
wall=$(pos)
lua "coop.SwitchChar(mdkGetPlayerGob(), $K)"; sleep 6
at=$(pos)
for k in 17 31 32; do mdk_press "$(mdk_pid 1)" $k 1 700 200; done; sleep 0.5
after=$(pos)
echo "doc at the wall $wall, kurt at $at, after walking $after" > "$out/wall.txt"
check "Kurt can move after switching from Doc at a wall ($wall -> $at -> $after)" \
    python3 -c "import sys; a, b = [list(map(float, v.split())) for v in sys.argv[1:3]]; sys.exit(0 if ((a[0]-b[0])**2 + (a[1]-b[1])**2) ** 0.5 > 0.5 else 1)" "$at" "$after"

check "game running"                                    bash -c "$(declare -f mdk_alive); mdk_alive $(mdk_pid 1)"
check "no Lua errors"                                   bash -c "! grep -q 'LUA ERROR' '$(log)'"
cp "$(log)" "$out/game1.log"
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
if [ "${MUTANT:-}" = nounstick ]; then
    printf '%s\n' "${results[@]}" | grep -q "^FAIL  Kurt can move after switching" && { echo "mutant nounstick caught"; exit 0; }
    echo "mutant nounstick NOT caught"; exit 1
fi
[ "$fail" -eq 0 ]
