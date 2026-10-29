#!/usr/bin/env bash
# Build the bridge and install everything into the game copies (closes running copies,
# which keep dinput8.dll locked): dinput8.dll, coop.ini (host + clients on localhost),
# coop.lua, the settings.lua loader line and the Windows input helpers.
# MDK2_START="<level> <checkpoint>" makes the host load that checkpoint from the title screen.
# MDK2_SIM="<latency ms> <jitter ms> <loss %>" adds a faked bad connection ([sim]) to every game.
# MDK2_BACKGROUND=1 installs background test mode for all: the games ignore the real keyboard
# and mouse, never take focus and are muted; scripts drive them through coop\command.lua.
# Their windows go side by side on the second monitor (the primary one if there is no other).
# MDK2_BUILD=overhaul|steam (or MDK2_BUILD<i> per copy) puts that build's mdk2hd.exe into the copies first;
# overhaul needs MDK2_OVERHAUL_EXE, steam uses MDK2_SRC.
# Every copy gets its own user data folder (MDK2_SAVE_ISOLATE, default on; see tools/mdk.sh): a clean profile
# is seeded at each install, so a run never reads or writes the real saves.
# Game copies: MDK2_N of them, instance 1 the host (see tools/mdk.sh).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"

[ "${MDK2_NO_BUILD:-0}" = 1 ] || make -s -C "$here/bridge"     # MDK2_NO_BUILD=1: already built (parallel sweeps)
mdk_kill
sleep 2
mdk_tools_install
dirs=""
seen_ids=""
for i in $(mdk_seq); do
    d="$(mdk_wsl "$(mdk_dir $i)")"
    [ -f "$d/mdk2hd.exe" ] || { echo "no game in $d (tools/copies.sh makes more copies)"; exit 1; }
    mkdir -p "$d/coop"
    mdk_place_build "$i" || exit 1
    # A previous run may have stopped before consuming its last background
    # command. Never execute that command against a fresh game at the title.
    rm -f "$d/coop/command.lua" "$d/coop/command.tmp"
    cp "$here/bridge/dinput8.dll" "$d/"
    # a host keeps the slot each player id had ([slots] in player.ini) and gives a new id one nobody has had:
    # a test copy that has hosted earlier runs would make its client player 3, 4, ... Forget them (a new file,
    # not written in place: test copies may be hard-linked)
    p="$d/coop/player.ini"
    if [ -f "$p" ] && grep -q '^\[\(steam_\)\?slots\]' "$p"; then
        awk '/^\[/ { skip = ($0 ~ /^\[(steam_)?slots\]/) } !skip' "$p" > "$p.new" && rm -f "$p" && mv "$p.new" "$p"
    fi
    # Two copies with one player id are one player to the host: a lobby run leaves id=0000000N behind and a slot
    # reused at another position then holds the same id as its neighbour, so both clients take each other's
    # connection forever. Drop the id of a later copy that repeats an earlier one (the game makes a new one).
    if [ -f "$p" ]; then
        pid=$(sed -n 's/^id=\([0-9a-fA-F]*\).*/\1/p' "$p" | head -1)
        if [ -n "$pid" ] && case " $seen_ids " in *" $pid "*) true ;; *) false ;; esac; then
            echo "install: $d repeats player id $pid; giving it a new one"
            grep -v '^id=' "$p" > "$p.new" && rm -f "$p" && mv "$p.new" "$p"
        elif [ -n "$pid" ]; then seen_ids="$seen_ids $pid"; fi
    fi
    dirs="$dirs $d"
    if mdk_isolated; then mdk_profile_seed "$i" || { echo "cannot seed the profile of game $i"; exit 1; }; fi
done
mx=0; my=0
if [ "${MDK2_BACKGROUND:-0}" = 1 ]; then
    read -r mx my < <(powershell.exe -NoProfile -Command 'Add-Type -AssemblyName System.Windows.Forms;
        $s = [System.Windows.Forms.Screen]::AllScreens | Sort-Object Primary | Select-Object -First 1;
        "$($s.WorkingArea.X) $($s.WorkingArea.Y)"' | tr -d '\r')
fi
# windows: two side by side, more in a grid of four across (they overlap; screenshots come from
# the back buffer, so that does not matter)
for i in $(mdk_seq); do
    if [ "$MDK2_N" -le 2 ]; then x=$((mx + 20 + (i - 1) * 840)); y=$((my + 40))
    else x=$((mx + 10 + (i - 1) % 4 * 630)); y=$((my + 20 + (i - 1) / 4 * 650)); fi
    sd=""; if mdk_isolated; then sd=$'save_dir=profile\r\n'; fi
    # MDK2_TIMESCALE=k: background games run their clock k times as fast (bridge/input.c)
    if [ -n "${MDK2_TIMESCALE:-}" ]; then sd="${sd}timescale=${MDK2_TIMESCALE}"$'\r\n'; fi
    # MDK2_CPITEMS=0: no checkpoint item grants (lua/coop_dev.lua coop.CheckpointItems)
    if [ -n "${MDK2_CPITEMS:-}" ]; then sd="${sd}cpitems=${MDK2_CPITEMS}"$'\r\n'; fi
    if [ "$i" = 1 ]; then net="role=host\r\nlisten_port=$MDK2_PORT"
    else net="role=client\r\nlisten_port=$((MDK2_PORT + i - 1))\r\npeer_host=127.0.0.1\r\npeer_port=$MDK2_PORT"; fi
    printf "[net]\r\ntransport=udp\r\n$net\r\n[controls]\r\ndevkeys=1\r\n[launcher]\r\nlobby=%s\r\n[test]\r\nbackground=%s\r\nx=%d\r\ny=%d\r\n%s" "${MDK2_LOBBY:-0}" "${MDK2_BACKGROUND:-0}" "$x" "$y" "$sd" \
        > "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.ini"
    # MDK2_SIM="<latency ms> <jitter ms> <loss %>": every game fakes a bad connection on what it receives
    if [ -n "${MDK2_SIM:-}" ]; then
        read -r sl sj sp <<< "$MDK2_SIM"
        printf "[sim]\r\nlatency_ms=%d\r\njitter_ms=%d\r\nloss_pct=%d\r\n" "$sl" "$sj" "$sp" \
            >> "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.ini"
    fi
done
GAME_DIRS="$dirs" "$here/tools/inject.sh"
# MDK2_START="<level> <checkpoint>": the host loads it by itself from the title screen, once the
# other instances have joined
if [ -n "${MDK2_START:-}" ]; then
    read -r start_level start_cp <<< "$MDK2_START"
    printf '\r\ncoop.autostart = { %d, %d }\r\ncoop.autostart_players = %d\r\n' "$start_level" "$start_cp" \
        $((MDK2_N - 1)) >> "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
fi
# MDK2_SAVE_INTERVAL=<seconds>: co-op saves that often (default 120)
if [ -n "${MDK2_SAVE_INTERVAL:-}" ]; then
    printf '\r\ncoop.save_interval = %d\r\n' "$MDK2_SAVE_INTERVAL" >> "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
fi
# background test mode: mute all (settings.lua has set the volumes by the time coop.lua runs;
# the games are killed, not quit, so they never write these back)
if [ "${MDK2_BACKGROUND:-0}" = 1 ]; then
    for i in $(mdk_seq); do
        printf '\r\nchSndSetEffectsVolume(0)\r\nchSndMusicVolume(0)\r\nchSndSetVoiceVolume(0)\r\n' \
            >> "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.lua"
    done
fi
