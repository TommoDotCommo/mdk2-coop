# Helpers for driving test game instances. Source this file.
#   MDK2_N                  number of instances (default 2); instance 1 is the host, the rest clients
#   MDK2_DIR<i>             disposable game copy i as a Windows path (required), or
#   MDK2_DIR_BASE           Windows path prefix; instance number is appended
#   MDK2_PORT               UDP port of the host (default 27900); instance i listens on MDK2_PORT + i - 1
#   MDK2_TOOLS              Windows folder for tools/win/*.ps1 (default: tools in copy 1)
#   MDK2_PIDS               file with the instances' pids, in order
#   MDK2_REGISTER_PID       optional executable called with each newly started PID
#   MDK2_PROCESS_SERVICE=1  launch through Win32_Process.Create when the calling process
#                           forces system DLLs ahead of the local dinput8.dll proxy
# Keys are DirectInput scancodes (1 Esc, 17 W, 30 A, 31 S, 32 D, 57 Space = jump with the co-op
# bindings, 59-68 F1-F10, 87 F11).
# Private configuration is optional and never tracked. Export its variables so child scripts
# receive the same copies and ports. Explicit environment values take precedence in that file.
mdk_config="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/local/env.sh"
if [ -f "$mdk_config" ]; then source "$mdk_config"; fi
MDK2_N="${MDK2_N:-2}"
MDK2_PORT="${MDK2_PORT:-27900}"

MDK2_PIDS="${MDK2_PIDS:-/tmp/mdk2-coop-pids}"

mdk_wsl()   { wslpath -u "$1"; }
mdk_dir()   { local i="${1:-1}" v; v="MDK2_DIR$i"
              if [ -n "${!v:-}" ]; then echo "${!v}"
              elif [ -n "${MDK2_DIR_BASE:-}" ]; then echo "$MDK2_DIR_BASE$i"
              else echo "configure MDK2_DIR$i or MDK2_DIR_BASE (see docs/DEVELOPMENT.md)" >&2; return 1; fi; }
MDK2_TOOLS="${MDK2_TOOLS:-$(mdk_dir 1)\\tools}"
mdk_log()   { echo "$(mdk_wsl "$(mdk_dir "$1")")/coop/bridge.log"; }
mdk_seq()   { seq 1 "$MDK2_N"; }
mdk_tools_install() (
    local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    # Shards share helpers. NTFS can reject concurrent creates even when cp would overwrite.
    # A subshell releases this descriptor before the settings-loader lock is acquired.
    exec 9> /tmp/mdk2-tools-install.lock
    flock 9 || return 1
    mkdir -p "$(mdk_wsl "$MDK2_TOOLS")" || return 1
    cp "$here"/win/*.ps1 "$(mdk_wsl "$MDK2_TOOLS")/"
)
mdk_start() { local d p; d=$(mdk_dir "${1:-1}") || return 1
              if [ "${MDK2_PROCESS_SERVICE:-0}" = 1 ]; then
                  p=$(powershell.exe -NoProfile -Command "(Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='\"$d\\mdk2hd.exe\"';CurrentDirectory='$d'}).ProcessId" | tr -d '\r') || return 1
              else
                  p=$(powershell.exe -NoProfile -Command "(Start-Process -PassThru -FilePath '$d\\mdk2hd.exe' -WorkingDirectory '$d').Id" | tr -d '\r') || return 1
              fi
              [[ "$p" =~ ^[1-9][0-9]*$ ]] || { echo "game launch returned no valid PID" >&2; return 1; }
              if [ -n "${MDK2_REGISTER_PID:-}" ]; then "$MDK2_REGISTER_PID" "$p" >&2 || return 1; fi
              echo "$p"; }
mdk_pids()  { cat "$MDK2_PIDS"; }
mdk_pid()   { local i=0 p; for p in $(mdk_pids); do i=$((i + 1)); [ "$i" = "$1" ] && { echo "$p"; return; }; done; }
# Input: in background test mode (MDK2_BACKGROUND=1 at install) a Lua line goes to the game
# through <game dir>\coop\command.lua and the bridge fakes the press; the desktop is left alone.
# Otherwise tools/win/*.ps1 bring the window to the front and send real input.
mdk_inst()  { local i=0 p; for p in $(mdk_pids); do i=$((i + 1)); [ "$p" = "$1" ] && { echo "$i"; return; }; done; echo 1; }   # pid -> 1..N
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
# mdk_state <tag> <outdir>: every game dumps its state (coop_state on the host asks the clients),
# saved as <outdir>/state_<tag>_host.txt, _client.txt (instance 2), _client3.txt, ... and compared
# by statecheck.py (its report on stdout, fails on a FAIL line)
mdk_state() { local i d f files=() name
              for i in $(mdk_seq); do rm -f "$(mdk_wsl "$(mdk_dir "$i")")/coop/state_$1.txt"; done
              mdk_cmd "$(mdk_pid 1)" "coop_state(\"$1\")" || return 1
              for i in $(seq 1 50); do
                  local all=1; for d in $(mdk_seq); do [ -e "$(mdk_wsl "$(mdk_dir "$d")")/coop/state_$1.txt" ] || all=0; done
                  [ $all = 1 ] && break; sleep 0.1; done
              for i in $(mdk_seq); do
                  case $i in 1) name=host;; 2) name=client;; *) name=client$i;; esac
                  f="$2/state_$1_$name.txt"
                  mv "$(mdk_wsl "$(mdk_dir "$i")")/coop/state_$1.txt" "$f" 2>/dev/null || { echo "state: no dump from game $i ($name)"; return 1; }
                  files+=("$f")
              done
              python3 "$(dirname "${BASH_SOURCE[0]}")/statecheck.py" "${files[@]}"; }
mdk_alive() { tasklist.exe /FI "PID eq $1" | grep -i mdk2hd >/dev/null; }   # no -q: SIGPIPE under pipefail
# mdk_kill: stop the games running from our copies only (other sessions may be running other copies)
mdk_kill()  { local i d w=""; for i in $(mdk_seq); do
                  d=$(mdk_dir "$i") || return 1
                  w="$w -or \$_.Path -like '$d\\*'"; done
              powershell.exe -NoProfile -Command "Get-Process mdk2hd -ErrorAction SilentlyContinue | Where-Object { \$false $w } | Stop-Process -Force" >/dev/null 2>&1 || true; }
# mdk_boot: start every instance and wait until all clients have followed the host into a level
# (install with MDK2_START so the host loads one by itself once the others have joined)
mdk_boot()  { mdk_kill; sleep 2; local i c pids="" ok
              for i in $(mdk_seq); do pids="$pids $(mdk_start "$i")"; echo "$pids" > "$MDK2_PIDS"; sleep 2; done
              for i in $(seq 1 60); do sleep 2; ok=1
                  for c in $(seq 2 "$MDK2_N"); do grep -q "^level [1-9][0-9]* checkpoint [0-9]* started" "$(mdk_log "$c")" 2>/dev/null || ok=0; done
                  [ $ok = 1 ] && break; done; sleep 6; }
# mdk_assert_far <target player> <label>: the geometry rule for every targeting test (docs/TESTING.md).
# Each game writes its own state dump; the target player must be at least the rule's distance from every other
# player (tools/targeting_geometry.py), read from those dumps and cross-checked against what the games see of
# each other. Dumps are kept in $out when set. Prints PASS/FAIL, returns nonzero on failure.
mdk_assert_far() { local target="$1" label="$2" tag="geo$RANDOM" files=() i path t
                   for i in $(mdk_seq); do
                       path="$(mdk_wsl "$(mdk_dir "$i")")/coop/state_${tag}_$i.txt"; rm -f "$path"
                       mdk_cmd "$(mdk_pid "$i")" "coop_state(\"${tag}_$i\")" || return 1
                       for t in $(seq 1 150); do [ -f "$path" ] && break; sleep 0.1; done
                       [ -f "$path" ] || { echo "FAIL geometry: game $i wrote no state dump"; return 1; }
                       files+=("$path")
                   done
                   [ -n "${out:-}" ] && for i in $(mdk_seq); do cp "${files[$((i - 1))]}" "$out/geometry_${tag}_$i.txt" 2>/dev/null; done
                   python3 "$(dirname "${BASH_SOURCE[0]}")/targeting_geometry.py" check --target "$target" --label "$label" "${files[@]}"; }
