# Helpers for driving two local game instances from WSL. Source this file.
#   MDK2_DIR1 / MDK2_DIR2   game copies as Windows paths (default C:\coop\mdk2-test, C:\coop\mdk2-test2)
#   MDK2_TOOLS              where tools/win/*.ps1 are copied (default C:\coop\tools)
# Instance 1 is the host, instance 2 the client. Keys are DirectInput scancodes
# (1 Esc, 17 W, 30 A, 31 S, 32 D, 57 Space = jump with the co-op bindings, 59-68 F1-F10, 87 F11).
MDK2_DIR1="${MDK2_DIR1:-C:\\coop\\mdk2-test}"
MDK2_DIR2="${MDK2_DIR2:-C:\\coop\\mdk2-test2}"
MDK2_TOOLS="${MDK2_TOOLS:-C:\\coop\\tools}"
MDK2_PIDS="${MDK2_PIDS:-/tmp/mdk2-coop-pids}"

mdk_wsl()   { wslpath -u "$1"; }
mdk_dir()   { [ "${1:-1}" = 1 ] && echo "$MDK2_DIR1" || echo "$MDK2_DIR2"; }
mdk_log()   { echo "$(mdk_wsl "$(mdk_dir "$1")")/coop/bridge.log"; }
mdk_tools_install() { local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; mkdir -p "$(mdk_wsl "$MDK2_TOOLS")"; cp "$here"/win/*.ps1 "$(mdk_wsl "$MDK2_TOOLS")/"; }
mdk_start() { local d; d=$(mdk_dir "${1:-1}"); powershell.exe -NoProfile -Command "(Start-Process -PassThru -FilePath '$d\\mdk2hd.exe' -WorkingDirectory '$d').Id" | tr -d '\r'; }
# Input: in background test mode (MDK2_BACKGROUND=1 at install) a Lua line goes to the game
# through <game dir>\coop\command.lua and the bridge fakes the press; the desktop is left alone.
# Otherwise tools/win/*.ps1 bring the window to the front and send real input.
mdk_inst()  { read -r a _ < "$MDK2_PIDS"; [ "$1" = "$a" ] && echo 1 || echo 2; }    # pid -> 1 / 2
mdk_bg()    { grep -q '^background=1' "$(mdk_wsl "$(mdk_dir "$(mdk_inst "$1")")")/coop/coop.ini" 2>/dev/null; }
# mdk_cmd <pid> <lua>: run a Lua line in that game, wait until it has picked it up
mdk_cmd()   { local d i; d="$(mdk_wsl "$(mdk_dir "$(mdk_inst "$1")")")/coop"
              printf '%s\r\n' "$2" > "$d/command.tmp"; mv "$d/command.tmp" "$d/command.lua"
              for i in $(seq 1 100); do [ -e "$d/command.lua" ] || return 0; sleep 0.1; done
              echo "mdk_cmd: game $1 did not pick up: $2" >&2; return 1; }
# mdk_press <pid> <code> <count> <holdMs> <gapMs>: code 0-255 scancode, 256 left / 257 right mouse
mdk_press() { local n; for n in $(seq 1 "$3"); do
                  mdk_cmd "$1" "coop_press($2, $4)" || return 1; sleep "$(echo "($4 + $5) / 1000" | bc -l)"; done; }
# mdk_key <pid> <scancode> [count] [holdMs]
mdk_key()   { if mdk_bg "$1"; then mdk_press "$1" "$2" "${3:-1}" "${4:-80}" 250; else
              powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\key.ps1" -id "$1" -sc "$2" -count "${3:-1}" -hold "${4:-80}"; fi; }
# mdk_click <pid> [holdMs] [count]: left mouse button (fire)
mdk_click() { if mdk_bg "$1"; then mdk_press "$1" 256 "${3:-1}" "${2:-100}" 200; else
              powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\mouse.ps1" -id "$1" -hold "${2:-100}" -count "${3:-1}"; fi; }
# mdk_rclick <pid>: right mouse button (sniper mode toggle with the co-op bindings)
mdk_rclick() { if mdk_bg "$1"; then mdk_press "$1" 257 1 100 200; else
               powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\mouse.ps1" -id "$1" -right; fi; }
# mdk_shot <pid> <file.png>: screenshot of that game (background: taken by the bridge)
mdk_shot()  { local d i; if mdk_bg "$1"; then
                  d="$(mdk_wsl "$(mdk_dir "$(mdk_inst "$1")")")/coop"; rm -f "$d/shot.tga"
                  mdk_cmd "$1" 'coop_shot("shot")' || return 1
                  for i in $(seq 1 20); do [ -s "$d/shot.tga" ] && break; sleep 0.1; done; sleep 0.2
                  python3 "$(dirname "${BASH_SOURCE[0]}")/tga2png.py" "$d/shot.tga" "$2" && rm -f "$d/shot.tga"
              else
                  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\winshot.ps1" -id "$1" -out "$MDK2_TOOLS\\shot_$$.png" &&
                      mv "$(mdk_wsl "$MDK2_TOOLS")/shot_$$.png" "$2"
              fi; }
# mdk_state <tag> <outdir>: both games dump their state (coop_state), saved as
# <outdir>/state_<tag>_host.txt / _client.txt and compared by statecheck.py (its report on stdout,
# fails on a FAIL line)
mdk_state() { local A B d1 d2 i; read -r A B < "$MDK2_PIDS"
              d1="$(mdk_wsl "$MDK2_DIR1")/coop"; d2="$(mdk_wsl "$MDK2_DIR2")/coop"
              rm -f "$d1/state_$1.txt" "$d2/state_$1.txt"
              mdk_cmd "$A" "coop_state(\"$1\")" || return 1
              for i in $(seq 1 50); do [ -e "$d1/state_$1.txt" ] && [ -e "$d2/state_$1.txt" ] && break; sleep 0.1; done
              mv "$d1/state_$1.txt" "$2/state_$1_host.txt" 2>/dev/null || { echo "state: no host dump"; return 1; }
              mv "$d2/state_$1.txt" "$2/state_$1_client.txt" 2>/dev/null || { echo "state: no client dump"; return 1; }
              python3 "$(dirname "${BASH_SOURCE[0]}")/statecheck.py" "$2/state_$1_host.txt" "$2/state_$1_client.txt"; }
mdk_alive() { tasklist.exe /FI "PID eq $1" | grep -i mdk2hd >/dev/null; }   # no -q: SIGPIPE under pipefail
mdk_kill()  { taskkill.exe /IM mdk2hd.exe /F >/dev/null 2>&1 || true; }   # fine when none is running
# mdk_boot: start host and client and wait until the client has followed the host into a
# level (install with MDK2_START so the host loads one by itself)
mdk_boot()  { mdk_kill; sleep 2; local A B i; A=$(mdk_start 1); sleep 3; B=$(mdk_start 2); echo "$A $B" > "$MDK2_PIDS"
              for i in $(seq 1 30); do sleep 2; grep -q "^level [1-9][0-9]* checkpoint [0-9]* started" "$(mdk_log 2)" 2>/dev/null && break; done; sleep 6; }
