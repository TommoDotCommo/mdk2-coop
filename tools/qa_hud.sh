#!/usr/bin/env bash
# HUD checks with MDK2_N (default 3) games in background test mode: the players panel's rows
# (names, hp, character, distance), the panel in the frame (pixels), switched off from the
# menu, hidden under a menu, and through a level change (Max). Screenshots in the output folder.
#   MDK2_N=3 tools/qa_hud.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-hud$MDK2_N"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()  { grep -qE "$2" "$(mdk_log "$1")"; }
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
rows() { lua "$1" 'coop_hud_dump()'; sleep 1; grep -E "^hud row: " "$(mdk_log "$1")" | tail -"$2" | tr -d '\r'; }
# mean brightness of the panel corner (top left, 25% x 10%) of a screenshot
corner() { python3 - "$1" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], "rb").read(); i = 8; w = h = 0; idat = b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8]); c = d[i + 8:i + 8 + n]; i += 12 + n
    if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
    if t == b"IDAT": idat += c
raw = zlib.decompress(idat); stride = 1 + w * 3; tot = cnt = 0
for y in range(h // 10):
    r = raw[y * stride + 1:y * stride + 1 + (w // 4) * 3]
    tot += sum(r); cnt += len(r)
print(round(tot / cnt, 1))
PY
}
L=$MDK2_N

mdk_boot
check "HUD drawing set up (GL found)"                  wait_log 1 "^hud: GL " 10
lua 2 'coop_set_name("Kurt Fan")'; lua $L 'coop_set_name("Doc Hawkins") mdkDealDamage(nil, mdkGetPlayerGob(), 35, 2, -1)'
lua 2 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 83.4, 181.5, -30)'; sleep 2
r=$(rows 1 $((MDK2_N - 1))); echo "$r" > "$out/rows_host.txt"
check "host panel: Kurt Fan, full hp, Kurt, distance"  bash -c "grep -qE '^hud row: 2 Kurt Fan \| 100  Kurt  [0-9]+ away \|' '$out/rows_host.txt'"
check "host panel: Doc Hawkins at 65 hp"               bash -c "grep -qE '^hud row: $L Doc Hawkins \| 65  Kurt' '$out/rows_host.txt'"
r=$(rows 2 $((MDK2_N - 1))); echo "$r" > "$out/rows_client2.txt"
check "client panel: the host, with ping"              bash -c "grep -qE '^hud row: 1 .* \| [0-9]+ ms$' '$out/rows_client2.txt'"
check "client panel: Doc Hawkins at 65 (relayed)"      bash -c "grep -qE '^hud row: $L Doc Hawkins \| 65 ' '$out/rows_client2.txt'"
mdk_shot "$(mdk_pid 1)" "$out/hud_on.png"
lua 1 'coop_set("hud", "show", 0) coop.hud_on = nil'; sleep 1
mdk_shot "$(mdk_pid 1)" "$out/hud_off.png"
on=$(corner "$out/hud_on.png"); off=$(corner "$out/hud_off.png")
check "the panel changes the frame ($on vs $off without)" python3 -c "import sys; sys.exit(0 if abs($on - $off) > 3 else 1)"
lua 1 'coop_hud_dump()'; sleep 1
f1=$(grep -E "^hud: [0-9]+ frames drawn" "$(mdk_log 1)" | tail -1 | sed -E 's/hud: ([0-9]+).*/\1/'); sleep 2
lua 1 'coop_hud_dump()'; sleep 1
f2=$(grep -E "^hud: [0-9]+ frames drawn" "$(mdk_log 1)" | tail -1 | sed -E 's/hud: ([0-9]+).*/\1/')
check "switched off: no frames drawn ($f1 -> $f2)"     test "$f1" = "$f2"
lua 1 'coop_set("hud", "show", 1) coop.hud_on = 1 BuildOptionsMenu() ShowMenu("options")'; sleep 1
lua 1 'coop_hud_dump()'; sleep 1
g1=$(grep -E "^hud: [0-9]+ frames drawn" "$(mdk_log 1)" | tail -1 | sed -E 's/hud: ([0-9]+).*/\1/'); sleep 2
lua 1 'coop_hud_dump()'; sleep 1
g2=$(grep -E "^hud: [0-9]+ frames drawn" "$(mdk_log 1)" | tail -1 | sed -E 's/hud: ([0-9]+).*/\1/')
check "hidden under a menu ($g1 -> $g2)"               test "$g1" = "$g2"
mdk_shot "$(mdk_pid 1)" "$out/menu_up.png"
lua 1 'HideMenu()'; sleep 1
mdk_key "$(mdk_pid 1)" 66                              # F8: level 2, everyone is Max
for i in $(seq 1 40); do sleep 1; has 2 "^level 2 checkpoint 1 started" && has $L "^level 2 checkpoint 1 started" && break; done
sleep 8
r=$(rows 1 $((MDK2_N - 1))); echo "$r" > "$out/rows_level2.txt"
check "level 2: the panel shows Max"                   bash -c "grep -qE '\| [0-9]+  Max' '$out/rows_level2.txt'"
mdk_shot "$(mdk_pid 1)" "$out/hud_level2.png"
check "all games running"                              bash -c "$(declare -f mdk_alive mdk_pids); for p in \$(cat '$MDK2_PIDS'); do mdk_alive \$p || exit 1; done"
check "no Lua errors"                                  bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
