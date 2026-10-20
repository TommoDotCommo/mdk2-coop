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
# mdk_key <pid> <scancode> [count] [holdMs]
mdk_key()   { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\key.ps1" -id "$1" -sc "$2" -count "${3:-1}" -hold "${4:-80}"; }
# mdk_click <pid> [holdMs] [count]: left mouse button (fire)
mdk_click() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\mouse.ps1" -id "$1" -hold "${2:-100}" -count "${3:-1}"; }
# mdk_rclick <pid>: right mouse button (sniper mode toggle with the co-op bindings)
mdk_rclick() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\mouse.ps1" -id "$1" -right; }
# mdk_shot <pid> <name>: window capture to $MDK2_TOOLS\<name>.png
mdk_shot()  { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$MDK2_TOOLS\\winshot.ps1" -id "$1" -out "$MDK2_TOOLS\\$2.png"; }
mdk_alive() { tasklist.exe /FI "PID eq $1" | grep -qi mdk2hd; }
mdk_kill()  { taskkill.exe /IM mdk2hd.exe /F >/dev/null 2>&1 || true; }   # fine when none is running
# mdk_boot: start host and client and wait until the client has followed the host into a
# level (install with MDK2_START so the host loads one by itself)
mdk_boot()  { mdk_kill; sleep 2; local A B i; A=$(mdk_start 1); sleep 3; B=$(mdk_start 2); echo "$A $B" > "$MDK2_PIDS"
              for i in $(seq 1 30); do sleep 2; grep -q "^level [1-9][0-9]* checkpoint [0-9]* started" "$(mdk_log 2)" 2>/dev/null && break; done; sleep 6; }
