#!/usr/bin/env bash
# Start a two-window co-op session on this PC for a manual playtest.
#   tools/play.sh          install the bridge into both game copies, start host (left window)
#                          and client (right window)
#   tools/play.sh 3 4      same, and the host goes straight to level 3 checkpoint 4
#   tools/play.sh stop     close both, take the line out of settings.lua again, and move both
#                          bridge.logs, F2 screenshots and crash dumps to local/sessions/<date-time>/
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
    for i in 1 2; do      # F2 screenshots and crash dumps
        d="$(mdk_wsl "$(mdk_dir $i)")"
        for f in "$d"/coop_mark*.tga "$d"/mdk2hd_crash_*.mdmp; do
            [ -e "$f" ] && mv "$f" "$out/$([ $i = 1 ] && echo host || echo client)-$(basename "$f")"
        done
    done
    echo "logs saved to $out"
    exit 0
fi

if [ $# -ge 2 ]; then
    MDK2_START="$1 $2" "$here/tools/install.sh"
else
    "$here/tools/install.sh"
fi

A=$(mdk_start 1); sleep 3; B=$(mdk_start 2)
echo "$A $B" > "$MDK2_PIDS"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$A" -x 20 -y 80
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$B" -x 900 -y 80

cat <<'TXT'

Host = left window, client = right window. Play in the LEFT window; the right one follows.
  - Start a New Game from the left window's menu, or start at a checkpoint:
    tools/play.sh <level> <checkpoint>. The right window loads the same level by itself.
  - F6 / F7  next / previous checkpoint (1-1 .. 1-8, 2-1, ...), F8 next level
  - F2   when something looks wrong: marker in both logs ("MARK 1", ...) plus a screenshot
  - F9   BioWare debug menu, F5 spawn a Doganboy in front of you
  - If you would die you are revived next to the other Kurt instead.
Note what you saw at each marker. When done: tools/play.sh stop
TXT
