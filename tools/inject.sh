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
settings="${MDK2_SETTINGS:-/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')/AppData/Local/MDK2HD/settings.lua}"
game_dirs="${GAME_DIRS:-/mnt/c/coop/mdk2-test /mnt/c/coop/mdk2-test2}"
begin='-- >>> mdk2-coop >>>'
end='-- <<< mdk2-coop <<<'

[ -f "$settings" ] || { echo "no settings.lua at $settings (run the game once)"; exit 1; }
[ -f "$settings.coop-backup" ] || cp "$settings" "$settings.coop-backup"

sed -i "/^$begin/,/^$end/d" "$settings"
if [ "${1:-}" = "--off" ]; then
    echo "removed from $settings"
    exit 0
fi
printf '%s\r\nif (coop_init) then coop_init() end\r\n%s\r\n' "$begin" "$end" >> "$settings"
for d in $game_dirs; do
    [ -d "$d" ] || continue
    mkdir -p "$d/coop"
    sed 's/$/\r/' "$here/lua/coop_dev.lua" > "$d/coop/coop.lua"
done
echo "loader in $settings ($(wc -c < "$settings") bytes), coop.lua in: $game_dirs"
