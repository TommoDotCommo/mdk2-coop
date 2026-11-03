#!/usr/bin/env bash
# More showcase scenes with two background games, each shot from the host's view (saved under
# local/showcase/<time>-scenes/): the lounge cutscene with its time-left box, the skydive and the level 9 torpedo
# run with the other player as a ghost, the other player under Kurt's canopy, with Max's jetpack flame and with
# Doc's leaf blower, and the pad layer on View (the player list, the quick chat wheel). One boot per scene.
# Not a test: nothing is checked.
#   ONLY=cutscene|skydive|torpedo|canopy|jetpack|blower|pad tools/showcase_scenes.sh   (default: all)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
out="$here/../local/showcase/$(date +%Y%m%d-%H%M%S)-scenes"; mkdir -p "$out"
trap mdk_kill EXIT
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
shot() { sleep "${2:-0.5}"; mdk_shot "$(mdk_pid 1)" "$out/$1.png" && echo "$1.png"; }
has()  { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i=$1 pattern=$2 deadline=$((SECONDS + $3)); while [ "$SECONDS" -le "$deadline" ]; do has "$i" "$pattern" && return 0; sleep 0.5; done; echo "timeout: $2 (game $1)" >&2; return 1; }
ask()  { local tag="ask$RANDOM"; lua "$1" "coop_log(\"$tag \" .. $2)"; sleep 0.7
         grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //"; }
want() { [ -z "${ONLY:-}" ] || [ "$ONLY" = "$1" ]; }
boot() {   # boot <level> <checkpoint>
    mdk_kill; sleep 2
    MDK2_BACKGROUND=1 MDK2_START="$1 $2" "$here/install.sh" >/dev/null || exit 1
    mdk_boot
    lua 1 "function showcase_pose(g) local x, y, z, w, i, j, k = coop_pose(g) return format('%f %f %f %f %f %f %f', x, y, z, w, i, j, k) end"
    lua 1 'coop_set_name("Hank")'; lua 2 'coop_set_name("Doc Hawkins")'
    lua 1 'coop_set("hud", "outline", 0) coop.hud_outline = nil'
}
# beside <ahead> <up>: the client stands <ahead> units in front of the host's camera view and <up> units higher,
# turned to face the host
beside() {
    local h c
    h=$(ask 1 'showcase_pose(mdkGetPlayerGob())'); c=$(ask 1 'showcase_pose(mdkGetCameraGob())')
    python3 - "$h" "$c" "$1" "$2" <<'PY' | while read -r x y z w k; do
import math, sys
hx, hy, hz, hw, hi, hj, hk = map(float, sys.argv[1].split()); cx, cy = map(float, sys.argv[2].split()[:2])
ahead, up = float(sys.argv[3]), float(sys.argv[4])
fx, fy = hx - cx, hy - cy; n = math.hypot(fx, fy) or 1; fx, fy = fx / n, fy / n
print(hx + fx * ahead, hy + fy * ahead, hz + up, -hk, hw)
PY
        lua 2 "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $x, $y, $z) coop_set_quat(mdkGetPlayerGob(), $w, 0, 0, $k)"; done
    sleep 1.5
}
# reach_minigame: Backspace votes carry the movie up to the minigame on both games
reach() {
    local deadline=$((SECONDS + 120)) i
    while [ "$SECONDS" -le "$deadline" ]; do
        for i in 1 2; do lua "$i" 'if Save.inminigame then coop_log("T mini".."game") end'; done
        has 1 'lua: T minigame' && has 2 'lua: T minigame' && return 0
        mdk_press "$(mdk_pid 1)" 14 1 100 200; mdk_press "$(mdk_pid 2)" 14 1 100 200
        sleep 4
    done
    return 1
}

if want cutscene; then
    boot 1 9
    wait_log 1 'movie 1: go' 60 && wait_log 2 'movie 1: (go|the host had already said go)' 60
    sleep 2
    mdk_press "$(mdk_pid 1)" 23 1 100 200                          # I: no use in a cutscene, but it shows the time left and the skip key
    shot cutscene 1.5
fi

if want skydive; then
    boot 1 12
    wait_log 1 'movie 1: (go|the host had already said go)' 60
    reach
    wait_log 1 'minigame: player 2 plays beside you \(level 1\)' 15
    sleep 3; shot skydive 1
fi

if want torpedo; then
    boot 9 13
    wait_log 1 'movie 1: go' 60
    reach
    wait_log 1 'minigame: player 2 plays beside you \(level 9\)' 15
    lua 2 'jdgob.z = 8; jdgob.x = 0'; sleep 1
    shot torpedo 1
fi

if want canopy; then
    boot 1 5
    sleep 8
    lua 1 'mdkGobSetPosition(mdkGetPlayerGob(), "l1_r8doganland1")'; sleep 3
    beside 7 0
    lua 2 'local p = mdkGetPlayerGob() qa_z0 = p.position.z mdkGobSetPositionXYZ(p, p.position.x, p.position.y, qa_z0 + 70) coop_press(57, 14000)'
    for i in 1 2 3 4 5 6; do shot canopy_$i 0.3; lua 2 'local p = mdkGetPlayerGob() if p.position.z < qa_z0 + 30 then mdkGobSetPositionXYZ(p, p.position.x, p.position.y, qa_z0 + 70) end'; done
fi

if want jetpack; then
    boot 2 8
    sleep 8
    beside 8 4
    lua 2 'coop_press(57, 4500)'
    for i in 1 2 3 4 5 6; do shot jetpack_$i 0.3; done
fi

if want blower; then
    boot 3 6
    sleep 8
    beside 7 0
    lua 2 'coop_test_blower(mdkGetPlayerGob())'; sleep 2.5
    lua 2 'coop_press(256, 4500)'
    for i in 1 2 3 4 5 6; do shot blower_$i 0.3; done
fi

if want pad; then
    boot 1 2
    sleep 6
    lua 1 'coop.SetPreset("modern")'; lua 2 'coop.SetPreset("modern")'; sleep 2
    lua 1 'coop_pad_fake(0, 0, 0, 0, 0, 0, 0)'; sleep 2
    VIEW=32; RB=512
    lua 1 "coop_pad_fake(0, 0, 0, 0, $VIEW, 0, 0)"; shot pad_list 1.2
    lua 1 "coop_pad_fake(0, 0, 0, 0, $((VIEW | RB)), 0, 0)"; sleep 0.4
    lua 1 "coop_pad_fake(0, 0, 0, 1, $VIEW, 0, 0)"; shot pad_quickchat 0.8
    lua 1 'coop_pad_fake(0, 0, 0, 0, 0, 0, 0)'; sleep 1
    lua 1 'coop_pad_fake()'
fi
echo "scenes: $out"
