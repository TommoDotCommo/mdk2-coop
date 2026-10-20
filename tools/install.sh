#!/usr/bin/env bash
# Build the bridge and install everything into both game copies (closes running copies,
# which keep dinput8.dll locked): dinput8.dll, coop.ini (host / client on localhost),
# coop.lua, the settings.lua loader line and the Windows input helpers.
# MDK2_START="<level> <checkpoint>" makes the host load that checkpoint from the title screen.
# MDK2_BACKGROUND=1 installs background test mode for both: the games ignore the real keyboard
# and mouse, never take focus and are muted; scripts drive them through coop\command.lua.
# Their windows go side by side on the second monitor (the primary one if there is no other).
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
mx=0; my=0
if [ "${MDK2_BACKGROUND:-0}" = 1 ]; then
    read -r mx my < <(powershell.exe -NoProfile -Command 'Add-Type -AssemblyName System.Windows.Forms;
        $s = [System.Windows.Forms.Screen]::AllScreens | Sort-Object Primary | Select-Object -First 1;
        "$($s.WorkingArea.X) $($s.WorkingArea.Y)"' | tr -d '\r')
fi
bg() { printf '[test]\r\nbackground=%s\r\nx=%d\r\ny=%d\r\n' "${MDK2_BACKGROUND:-0}" "$1" "$2"; }
{ printf '[net]\r\nrole=host\r\nlisten_port=27900\r\npeer_host=127.0.0.1\r\npeer_port=27901\r\n'
  bg $((mx + 20)) $((my + 40)); } > "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.ini"
{ printf '[net]\r\nrole=client\r\nlisten_port=27901\r\npeer_host=127.0.0.1\r\npeer_port=27900\r\n'
  bg $((mx + 860)) $((my + 40)); } > "$(mdk_wsl "$(mdk_dir 2)")/coop/coop.ini"
GAME_DIRS="$(mdk_wsl "$(mdk_dir 1)") $(mdk_wsl "$(mdk_dir 2)")" "$here/tools/inject.sh"
# MDK2_START="<level> <checkpoint>": the host loads it by itself from the title screen
if [ -n "${MDK2_START:-}" ]; then
    read -r start_level start_cp <<< "$MDK2_START"
    printf '\r\ncoop.autostart = { %d, %d }\r\n' "$start_level" "$start_cp" >> "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
fi
# background test mode: mute both (settings.lua has set the volumes by the time coop.lua runs;
# the games are killed, not quit, so they never write these back)
if [ "${MDK2_BACKGROUND:-0}" = 1 ]; then
    for i in 1 2; do
        printf '\r\nchSndSetEffectsVolume(0)\r\nchSndMusicVolume(0)\r\nchSndSetVoiceVolume(0)\r\n' \
            >> "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.lua"
    done
fi
