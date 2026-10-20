#!/usr/bin/env bash
# Append lua/coop_dev.lua to the game's settings.lua between markers.
#   tools/inject.sh         inject (replaces any previous injection)
#   tools/inject.sh --off   remove the injection
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
settings="${MDK2_SETTINGS:-/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')/AppData/Local/MDK2HD/settings.lua}"
begin='-- >>> mdk2-coop >>>'
end='-- <<< mdk2-coop <<<'

[ -f "$settings" ] || { echo "no settings.lua at $settings (run the game once)"; exit 1; }
[ -f "$settings.coop-backup" ] || cp "$settings" "$settings.coop-backup"

sed -i "/^$begin/,/^$end/d" "$settings"
if [ "${1:-}" != "--off" ]; then
    { printf '%s\r\n' "$begin"; sed 's/$/\r/' "$here/lua/coop_dev.lua"; printf '%s\r\n' "$end"; } >> "$settings"
    echo "injected into $settings"
else
    echo "removed from $settings"
fi
