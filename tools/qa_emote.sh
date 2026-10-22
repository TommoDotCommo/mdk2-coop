#!/usr/bin/env bash
# Emote browser (dev keys) in one background game: PgDn / PgUp / Home / End step through the animations,
# the wheel plays picks; the caption is drawn while one plays (pixels), Insert keeps, Delete lists, 0 and moving stop. Needs a lab lease.
#   MDK2_N=1 tools/qa_emote.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-1}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-emote"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
lua()  { mdk_cmd "$(mdk_pid 1)" "$1"; }
log()  { mdk_log 1; }
emoting() { lua 'coop_log(format("EMOTING %d", coop_emoting()))'; sleep 1; grep -E "lua: EMOTING " "$(log)" | tail -1 | tr -d '\r' | awk '{print $NF}'; }
# mean brightness of the caption strip (top centre, 40% x 12%) of a screenshot
strip() { python3 - "$1" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read(); i = 8; w = h = 0; idat = b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8]); c = d[i + 8:i + 8 + n]; i += 12 + n
    if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
    if t == b"IDAT": idat += c
raw = zlib.decompress(idat); stride = 1 + w * 3; tot = cnt = 0
for y in range(h // 12):
    r = raw[y * stride + 1 + (w * 3 // 10) * 3:y * stride + 1 + (w * 7 // 10) * 3]
    tot += sum(r); cnt += len(r)
print(round(tot / cnt, 1))
PY
}

# mean brightness of the middle (30% x 40%) of a screenshot
centre() { python3 - "$1" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read(); i = 8; w = h = 0; idat = b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8]); c = d[i + 8:i + 8 + n]; i += 12 + n
    if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
    if t == b"IDAT": idat += c
raw = zlib.decompress(idat); stride = 1 + w * 3; tot = cnt = 0
for y in range(h * 3 // 10, h * 7 // 10):
    r = raw[y * stride + 1 + (w * 35 // 100) * 3:y * stride + 1 + (w * 65 // 100) * 3]
    tot += sum(r); cnt += len(r)
print(round(tot / cnt, 1))
PY
}

mdk_boot
# solo: mdk_boot has no client to wait for, so wait for the host's autostart into the level
for i in $(seq 1 90); do grep -q "^level 1 checkpoint 2 started" "$(log)" 2>/dev/null && break; sleep 2; done
sleep 6
lua 'coop_set("controls", "devkeys", 1) coop.ApplyDevKeys(1)'; sleep 1
before=$(emoting)
check "no emote before any key"                         test "$before" = "-1"

mdk_shot "$(mdk_pid 1)" "$out/idle.png"
mdk_key "$(mdk_pid 1)" 209                               # PgDn
sleep 1
a=$(emoting)
check "PgDn starts an emote (anim $a)"              test "$a" -ge 0
mdk_shot "$(mdk_pid 1)" "$out/emote.png"
on=$(strip "$out/emote.png"); off=$(strip "$out/idle.png")
check "caption changes the frame ($on vs $off)"         python3 -c "import sys; sys.exit(0 if abs($on - $off) > 3 else 1)"
check "caption text set (name in log)"                  grep -qE "lua: emote $a ANIM_" "$(log)"

mdk_key "$(mdk_pid 1)" 209; sleep 1
b=$(emoting)
check "PgDn again moves on ($a -> $b)"              test "$b" != "$a" -a "$b" -ge 0
mdk_key "$(mdk_pid 1)" 201; sleep 1
c=$(emoting)
check "PgUp goes back ($b -> $c)"                   test "$c" = "$a"
mdk_key "$(mdk_pid 1)" 199; sleep 1                      # Home: +10
d=$(emoting)
check "Home jumps ahead ($c -> $d)"                 test "$d" != "$c" -a "$d" -ge 0
mdk_key "$(mdk_pid 1)" 207; sleep 1                      # End: -10
e=$(emoting)
check "End jumps back ($d -> $e)"                  test "$e" != "$d" -a "$e" -ge 0

mdk_key "$(mdk_pid 1)" 210; sleep 1                      # Insert: keep
check "Insert keeps ($e)"                             grep -qE "lua: EMOTE KEEP $e ANIM_" "$(log)"
mdk_key "$(mdk_pid 1)" 210; sleep 1
check "Insert again un-keeps"                         grep -qE "lua: EMOTE UNKEEP $e ANIM_" "$(log)"
mdk_key "$(mdk_pid 1)" 210; sleep 1
mdk_key "$(mdk_pid 1)" 211; sleep 1                      # Delete: list
check "Delete lists the kept emote"                   grep -qE "lua: EMOTE KEPT: .*$e=ANIM_" "$(log)"

mdk_key "$(mdk_pid 1)" 14; sleep 1                      # Backspace: stop
f=$(emoting)
check "Backspace stops"                                  test "$f" = "-1"
mdk_key "$(mdk_pid 1)" 209; sleep 1
mdk_key "$(mdk_pid 1)" 17 1 400; sleep 1                # W: moving ends it
g=$(emoting)
check "moving stops an emote"                           test "$g" = "-1"

# the wheel: hold H (DIK 35), steer with the mouse, let go to play. Slots clockwise from the top:
# 2 3 4 16 57 81 82; down-left of the middle is slot 5 (id 57), nothing selected stops it
whl() { lua "coop_press(35, $1)"; }
lua 'coop_emote(-1)'; sleep 1
whl 3000; sleep 0.6
lua 'coop_mouse(-40, 70)'; sleep 0.6
mdk_shot "$(mdk_pid 1)" "$out/wheel.png"
sleep 2.4
w=$(emoting)
check "wheel: down-left plays slot 5 (57, got $w)"      test "$w" = "57"
whl 1500; sleep 0.5
lua 'coop_mouse(0, -80)'; sleep 2
w=$(emoting)
check "wheel: straight up plays slot 1 (2, got $w)"     test "$w" = "2"
whl 1500; sleep 2.5
w=$(emoting)
check "wheel: let go in the middle stops it (got $w)"   test "$w" = "-1"
wheel=$(centre "$out/wheel.png"); idle=$(centre "$out/idle.png")
check "wheel is drawn ($wheel vs $idle)"               python3 -c "import sys; sys.exit(0 if abs($wheel - $idle) > 3 else 1)"

check "game running"                                    bash -c "$(declare -f mdk_alive); mdk_alive $(mdk_pid 1)"
check "no Lua errors"                                   bash -c "! grep -q 'LUA ERROR' '$(log)'"
cp "$(log)" "$out/game1.log"
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
