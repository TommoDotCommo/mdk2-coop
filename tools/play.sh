#!/usr/bin/env bash
# Start a two-window co-op session on this PC for a manual playtest.
#   tools/play.sh          install the bridge into both game copies, start host (left window)
#                          and client (right window)
#   tools/play.sh stop     close both, take the line out of settings.lua again, and copy both
#                          bridge.logs to local/sessions/<date-time>/
# Game copies: MDK2_DIR1 (host) and MDK2_DIR2 (client), see tools/mdk.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"

if [ "${1:-}" = "stop" ]; then
    mdk_kill
    sleep 2
    "$here/tools/inject.sh" --off
    out="$here/local/sessions/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$out"
    cp "$(mdk_log 1)" "$out/host.log"
    cp "$(mdk_log 2)" "$out/client.log"
    echo "logs saved to $out"
    exit 0
fi

make -s -C "$here/bridge"
mdk_kill            # running copies keep dinput8.dll locked
sleep 2
mdk_tools_install
for i in 1 2; do
    d="$(mdk_wsl "$(mdk_dir $i)")"
    [ -f "$d/mdk2hd.exe" ] || { echo "no game in $d"; exit 1; }
    mkdir -p "$d/coop"
    cp "$here/bridge/dinput8.dll" "$d/"
done
printf '[net]\r\nrole=host\r\nlisten_port=27900\r\npeer_host=127.0.0.1\r\npeer_port=27901\r\n' \
    > "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.ini"
printf '[net]\r\nrole=client\r\nlisten_port=27901\r\npeer_host=127.0.0.1\r\npeer_port=27900\r\n' \
    > "$(mdk_wsl "$(mdk_dir 2)")/coop/coop.ini"
GAME_DIRS="$(mdk_wsl "$(mdk_dir 1)") $(mdk_wsl "$(mdk_dir 2)")" "$here/tools/inject.sh"

A=$(mdk_start 1); sleep 3; B=$(mdk_start 2)
echo "$A $B" > "$MDK2_PIDS"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$A" -x 20 -y 80
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$B" -x 900 -y 80

cat <<'TXT'

Host = left window, client = right window. Play in the LEFT window; the right one follows.
  - Start a New Game from the left window's menu (or F7 for level 1 past the skydive).
    The right window loads the same level by itself.
  - F2   drop a marker in both logs ("MARK 1", "MARK 2", ...) when something looks wrong
  - F8   skip to the next level (both windows go)
  - F9   BioWare debug menu, F5 spawn a Doganboy in front of you
  - If you would die you are revived next to the other Kurt instead.
Note what you saw at each marker. When done: tools/play.sh stop
TXT
