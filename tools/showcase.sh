#!/usr/bin/env bash
# Showcase screenshots with three background games: the players panel and name tags, a ping, chat bubbles,
# the emote and character wheels, a downed player and the revive, and Max in level 2. Every shot is
# taken from the host's view and saved under local/showcase/<time>/. Not a test: nothing is checked.
#   MDK2_N=3 tools/showcase.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="${SHOWCASE_START:-1 2}" "$here/install.sh" >/dev/null || exit 1

out="$here/../local/showcase/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
shot() { sleep "${2:-0.5}"; mdk_shot "$(mdk_pid 1)" "$out/$1.png" && echo "$1.png"; }
# ask <instance> <lua expression>: its value, logged there
ask()  { local tag="ask$RANDOM"; lua "$1" "coop_log(\"$tag \" .. $2)"; sleep 0.7
         grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //"; }
# pose <instance> <gob expression>: x y z and the orientation quaternion w i j k (needs showcase_pose, defined after boot)
pose() { ask "$1" "showcase_pose($2)"; }
define_pose() { local i; for i in 1 2 3; do lua "$i" "function showcase_pose(g) local x, y, z, w, i, j, k = coop_pose(g) return format('%f %f %f %f %f %f %f', x, y, z, w, i, j, k) end"; done; }
# place <ahead> <apart>: the two clients stand <ahead> units in front of the host (seen from its camera),
# <apart> units from each other, turned to face it
place() {
    local h c
    h=$(pose 1 'mdkGetPlayerGob()'); c=$(pose 1 'mdkGetCameraGob()')
    python3 - "$h" "$c" "$1" "$2" <<'PY' | while read -r i x y z w k; do
import math, sys
hx, hy, hz, hw, hi, hj, hk = map(float, sys.argv[1].split()); cx, cy = map(float, sys.argv[2].split()[:2])
ahead, apart = float(sys.argv[3]), float(sys.argv[4])
fx, fy = hx - cx, hy - cy; n = math.hypot(fx, fy) or 1; fx, fy = fx / n, fy / n
for i, s in ((2, -0.5), (3, 0.5)):    # the host's yaw turned half round: (w, 0, 0, k) * (0, 0, 0, 1)
    print(i, hx + fx * ahead - fy * apart * s, hy + fy * ahead + fx * apart * s, hz, -hk, hw)
PY
        lua "$i" "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $x, $y, $z) coop_set_quat(mdkGetPlayerGob(), $w, 0, 0, $k)"; done
    sleep 1.5; }

mdk_boot
define_pose
lua 1 'coop_set_name("Hank")'; lua 2 'coop_set_name("Kurt Fan")'; lua 3 'coop_set_name("Doc Hawkins")'; sleep 2
lua 1 'coop_set("hud", "outline", 0) coop.hud_outline = nil'
place 4 5
shot group 1

lua 2 'local x, y, z = coop_pose(mdkGetPlayerGob()) coop_ping(x, y, z + 2)'
shot ping 1

lua 2 'coop_say("over here, the door is open")'; sleep 0.5; lua 3 'coop_typing(1)'
shot chat 1
lua 3 'coop_typing(0)'

lua 1 'coop_press(35, 3000)'
shot wheel_emote 1
sleep 3

lua 3 'mdkDealDamage(nil, mdkGetPlayerGob(), 999, DAMAGE_BADGUY, -1)'
shot downed 2.5
h=$(pose 3 'mdkGetPlayerGob()')
lua 1 "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $(echo "$h" | awk '{print $1 - 2 ", " $2 ", " $3}'))"; sleep 1
lua 1 'coop_press(19, 3600)'
shot reviving 1.5
sleep 3

place 6 4
lua 1 'coop_press(36, 3000)'; sleep 0.8; lua 1 'coop_mouse(60, 0)'
shot wheel_character 0.6
sleep 4

mdk_key "$(mdk_pid 1)" 66                                   # F8: level 2, everyone is Max
for i in $(seq 1 40); do sleep 1; grep -q "^level 2 checkpoint 1 started" "$(mdk_log 3)" && break; done
sleep 8
place 6 4
shot level2_max 1

mdk_kill
echo "screenshots: $out"
