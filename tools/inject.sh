#!/usr/bin/env bash
# Install the dev harness:
#  - lua/coop_dev.lua -> <game dir>/coop/coop.lua for each game dir
#  - one line in the game's settings.lua that calls coop_init() (the DLL then runs coop.lua)
#   tools/inject.sh         install
#   tools/inject.sh --off   remove the settings.lua line
# The game reads settings.lua into an 8 KB buffer and dies if it is larger, so only
# the loader line goes there.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"
if [ -n "${MDK2_SETTINGS:-}" ]; then settings="$MDK2_SETTINGS"
else settings=""; fi          # the real one is looked up only when a copy needs it
if [ -n "${GAME_DIRS:-}" ]; then game_dirs="$GAME_DIRS"
else
    game_dirs=""
    for i in $(mdk_seq); do game_dirs="$game_dirs $(mdk_wsl "$(mdk_dir "$i")")"; done
fi
begin='-- >>> mdk2-coop >>>'
end='-- <<< mdk2-coop <<<'

# settings_of <game dir>: the settings.lua that copy's game reads: its own profile's when coop.ini has
# [test] save_dir (tools/install.sh), otherwise the real one (shared by every copy that is not isolated)
settings_of() {
    local d="$1" dir
    dir=$(tr -d '\r' < "$d/coop/coop.ini" 2>/dev/null | sed -n 's/^save_dir=//p' | head -1)
    if [ -n "$dir" ]; then echo "$d/$dir/MDK2HD/settings.lua"
    elif [ -n "$settings" ]; then echo "$settings"
    else echo "$(mdk_real_profile)/settings.lua"; fi
}

# every non-isolated game copy shares settings.lua: parallel installs (sweep shards) take turns editing it
exec 9> /tmp/mdk2-inject.lock
flock 9

strip_loader() {   # a game that is starting (another session's) has the file open and Windows refuses sed's rename for a
                   # moment ("sed: cannot rename ... Permission denied"): try again, and take away sed's leftover
    local f="$1" try
    for try in $(seq 1 20); do
        if sed -i "/^$begin/,/^$end/d" "$f" 2>/dev/null; then return 0; fi
        find "$(dirname "$f")" -maxdepth 1 -name 'sed??????' -newer "$f.coop-backup" -delete 2>/dev/null || true
        sleep 0.5
    done
    echo "settings.lua stayed locked: $f"; return 1
}

for d in $game_dirs; do
    [ -d "$d" ] || continue
    f="$(settings_of "$d")"
    case "$f" in "$d"/*) [ -f "$f" ] || { mkdir -p "$(dirname "$f")"; : > "$f"; } ;; esac    # an isolated profile with no seeded settings
    [ -f "$f" ] || { echo "no settings.lua at $f (run the game once)"; exit 1; }
    [ -f "$f.coop-backup" ] || cp "$f" "$f.coop-backup"
    strip_loader "$f" || exit 1
    if [ "${1:-}" = "--off" ]; then echo "removed from $f"; continue; fi
    printf '%s\r\nif (coop_init) then coop_init() end\r\n%s\r\n' "$begin" "$end" >> "$f"
    mkdir -p "$d/coop"
    sed 's/$/\r/' "$here/lua/coop_dev.lua" > "$d/coop/coop.lua"
    echo "loader in $f ($(wc -c < "$f") bytes), coop.lua in: $d"
done
