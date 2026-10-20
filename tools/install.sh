#!/usr/bin/env bash
# Build the bridge and install everything into both game copies (closes running copies,
# which keep dinput8.dll locked): dinput8.dll, coop.ini (host / client on localhost),
# coop.lua, the settings.lua loader line and the Windows input helpers.
# MDK2_START="<level> <checkpoint>" makes the host load that checkpoint from the title screen.
# Game copies: MDK2_DIR1 (host) and MDK2_DIR2 (client), see tools/mdk.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"

make -s -C "$here/bridge"
mdk_kill
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
# MDK2_START="<level> <checkpoint>": the host loads it by itself from the title screen
if [ -n "${MDK2_START:-}" ]; then
    read -r start_level start_cp <<< "$MDK2_START"
    printf '\r\ncoop.autostart = { %d, %d }\r\n' "$start_level" "$start_cp" >> "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
fi
