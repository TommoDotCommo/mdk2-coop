#!/usr/bin/env bash
# Record a person playing a level, for the campaign bot to follow (docs/CAMPAIGN-BOT.md).
#   tools/record_level.sh <level> [checkpoint]   one game copy from the resource hook, started at that
#                                                checkpoint (default 1) for a person to play: in front,
#                                                with sound, on the main monitor. Play on; quit the game
#                                                when done. Every frame is journalled ([test] journal=1),
#                                                and on exit the journal becomes segment routes
#                                                local/routes/seg_<level>_<from>_<to>.route
#   tools/record_level.sh --convert <folder> [out]   only the conversion, of a journal folder (or a copy)
# The copy is leased like the test suites (MDK2_QA_RESOURCES or local/qa_resources.py: `acquire 1 <ttl>`
# prints its environment as JSON; `release` frees it) and released when the script ends, however it
# ends. MDK2_BUILD=overhaul with MDK2_OVERHAUL_EXE records on that build (the one most people play).
# The journals are kept under local/journal/<level>_<checkpoint>_<time>/.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = "--convert" ]; then
    [ $# -ge 2 ] || { echo "usage: $0 --convert <journal folder> [out folder]" >&2; exit 2; }
    exec python3 "$here/tools/campaign_bot.py" journal "$2" --out "${3:-$here/local/routes}"
fi
case "${1:-}" in ''|*[!0-9]*) echo "usage: $0 <level> [checkpoint]   or   $0 --convert <folder> [out]" >&2; exit 2;; esac
level="$1"; cp="${2:-1}"
case "$cp" in *[!0-9]*) echo "checkpoint must be a number" >&2; exit 2;; esac

hook="${MDK2_QA_RESOURCES:-}"
[ -n "$hook" ] || { [ -f "$here/local/qa_resources.py" ] && hook="$here/local/qa_resources.py"; }
[ -n "$hook" ] || { echo "no resource hook: set MDK2_QA_RESOURCES (see docs/TESTING.md)" >&2; exit 1; }
owner="${MDK2_QA_OWNER:-${LAB_OWNER:-}}"
[ -n "$owner" ] || { echo "set LAB_OWNER or MDK2_QA_OWNER (a unique identity for this recording)" >&2; exit 1; }
export MDK2_QA_OWNER="$owner" LAB_OWNER="$owner"

lease="$(python3 "$hook" acquire 1 "${MDK2_RECORD_TTL:-14400}")" || { echo "no game copy free now; try again later" >&2; exit 3; }
release() { set +e; [ -n "${MDK2_DIR1:-}" ] && (source "$here/tools/mdk.sh" && mdk_kill) >/dev/null 2>&1
            python3 "$hook" release >/dev/null 2>&1; }
trap release EXIT
eval "$(python3 -c 'import json, shlex, sys
for k, v in json.load(sys.stdin).items(): print(f"export {k}={shlex.quote(str(v))}")' <<< "$lease")"
# a person plays: in front, with sound, real keyboard and mouse; one player, so the host starts at once
# (MDK2_RECORD_BACKGROUND=1: a background game instead, for checking this script without a person)
export MDK2_N=1 MDK2_BACKGROUND="${MDK2_RECORD_BACKGROUND:-0}" MDK2_JOURNAL=1 MDK2_START="$level $cp"
source "$here/tools/mdk.sh"
trap release EXIT                       # again: tools/mdk.sh sets its own
d="$(mdk_wsl "$(mdk_dir 1)")"
"$here/tools/install.sh" >/dev/null
rm -rf "$d/coop/journal"
pid="$(mdk_start 1)"
echo "$pid" > "$MDK2_PIDS"
sleep 3
[ "$MDK2_BACKGROUND" = 1 ] ||
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\place.ps1" -id "$pid" -x 40 -y 40 >/dev/null 2>&1 || true
echo "Level $level checkpoint $cp is loading in the game window (pid $pid). Play; quit the game when done."
echo "Every checkpoint reached is recorded. Dying and reloading are fine: only the last attempt is kept."
while powershell.exe -NoProfile -Command "if (Get-Process -Id $pid -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" >/dev/null 2>&1; do
    sleep 5
done
keep="$here/local/journal/${level}_${cp}_$(date +%Y%m%d-%H%M%S)"
mkdir -p "$keep"
if ls "$d/coop/journal/"*.route >/dev/null 2>&1; then
    cp "$d/coop/journal/"*.route "$keep/"
else
    echo "the game left no journal (did a level load?)" >&2; exit 1
fi
echo "journal kept in $keep"
python3 "$here/tools/campaign_bot.py" journal "$keep" --out "$here/local/routes"
