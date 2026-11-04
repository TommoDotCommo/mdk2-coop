#!/usr/bin/env bash
# Start a co-op session on this PC for a manual playtest: MDK2_N windows (default 2), the first
# the host, the others clients.
#   tools/play.sh          install the bridge into the game copies, start the host (top left)
#                          and the clients (to its right, then a second row)
#   tools/play.sh 3 4      same, and the host goes straight to level 3 checkpoint 4
#   MDK2_N=3 tools/play.sh host and two clients
#   tools/play.sh stop     close them, take the line out of settings.lua again, and move every
#                          bridge.log, F2 screenshot and crash dump to local/sessions/<date-time>/
#                          (use the same MDK2_N as for starting)
#   MDK2_WINDOW=WxH        each copy's window size (default 1200x675 for two, 800x450 for more, so the windows sit
#                          side by side and leave the desktop usable: a copy's settings come from your own, often a
#                          window the size of the screen, which put the games over everything); MDK2_WINDOW=keep
#                          keeps your own. Only isolated copies (MDK2_SAVE_ISOLATE, the default) are changed.
# Game copies: MDK2_DIR<i> or MDK2_DIR_BASE, see tools/mdk.sh (tools/copies.sh makes more).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"

if [ "${1:-}" = "stop" ]; then
    mdk_kill
    sleep 2
    "$here/tools/inject.sh" --off
    out="$here/local/sessions/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$out"
    for i in $(mdk_seq); do      # logs, F2 screenshots and crash dumps
        case $i in 1) n=host;; 2) n=client;; *) n=client$i;; esac
        cp "$(mdk_log "$i")" "$out/$n.log"
        d="$(mdk_wsl "$(mdk_dir "$i")")"
        for f in "$d"/coop_mark*.tga "$d"/mdk2hd_crash_*.mdmp; do
            [ -e "$f" ] && mv "$f" "$out/$n-$(basename "$f")"
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

# window size: the video line of each isolated copy's settings.lua (chSetVideoMode(name, x, y, w, h, hz, fullscreen))
win="${MDK2_WINDOW:-}"; [ -n "$win" ] || { if [ "$MDK2_N" -le 2 ]; then win=1200x675; else win=800x450; fi; }
w=1200; h=675
if [ "$win" != keep ] && [[ "$win" =~ ^([0-9]+)x([0-9]+)$ ]] && mdk_isolated; then
    w=${BASH_REMATCH[1]}; h=${BASH_REMATCH[2]}
    for i in $(mdk_seq); do
        f="$(mdk_profile "$i")/settings.lua"
        [ -f "$f" ] && sed -i "s/^chSetVideoMode(.*/chSetVideoMode('', 0, 0, $w, $h, 0, 0)/" "$f"
    done
fi

pids=""
for i in $(mdk_seq); do
    p=$(mdk_start "$i"); pids="$pids $p"; echo "$pids" > "$MDK2_PIDS"
    # two side by side; more in rows of three
    if [ "$MDK2_N" -le 2 ]; then x=$((20 + (i - 1) * (w + 20))); y=80
    else x=$((20 + (i - 1) % 3 * (w + 20))); y=$((80 + (i - 1) / 3 * (h + 60))); fi
    sleep 3
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$p" -x "$x" -y "$y"
done

cat <<'TXT'

Host = first window (top left), clients = the others. Play in the host window; clients follow.
  - Start a New Game from the host window's menu, or start at a checkpoint:
    tools/play.sh <level> <checkpoint>. The client windows load the same level by themselves.
  - F6 / F7  next / previous checkpoint (1-1 .. 1-8, 2-1, ...), F8 next level
  - F2   when something looks wrong: marker in both logs ("MARK 1", ...) plus a screenshot
  - F9   BioWare debug menu, F5 spawn a Doganboy in front of you
  - If you would die you are revived next to the other Kurt instead.
Note what you saw at each marker. When done: tools/play.sh stop
TXT
