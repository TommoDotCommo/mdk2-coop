#!/usr/bin/env bash
# Launcher checks with two games in background test mode, the launcher hidden and driven through
# coop\launcher.txt ([test] launcher=1, see bridge/launcher.c): screenshots of every page, a
# client joining over UDP at an address typed in, the host's settings written on Play, solo
# turning co-op off, Esc quitting, [launcher] show=0, +connect_lobby skipping it and the
# resolution written to settings.lua (restored afterwards). Screenshots go to the output folder.
#   tools/qa_launcher.sh
# Game copies: see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-launcher"; mkdir -p "$out"
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
ini()   { grep -E "^$2=" "$(dir "$1")/coop/$3" | tail -1 | cut -d= -f2 | tr -d '\r'; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
# lcmd <instance> <line>...: run launcher commands, wait until the launcher has read them
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
gone()  { ! mdk_alive "$1"; }
shot()  { lcmd "$1" "shot launcher_$2" && python3 "$here/tga2png.py" "$(dir "$1")/coop/launcher_$2.tga" "$out/$2.png"; }
start() { local pid; pid=$(mdk_start "$1"); echo "$pid"; }
BS=(); for i in $(seq 1 24); do BS+=("key 8"); done     # backspaces: empty a field

# canvas spots (1280x720): tabs, Play page tiles, the connection selector, the UDP join fields,
# the Play button; Settings rows
T_PLAY="click 780 50"; T_SET="click 900 50"; T_ABOUT="click 1080 50"
HOST="click 1000 200"; JOIN="click 1000 283"; SOLO="click 1000 367"
ADDR="click 900 532"; PORT="click 1170 532"; PLAY="click 1000 660"
FF="click 1000 139"; RES="click 1180 459"

for i in 1 2; do
    setini $i test launcher 1
    setini $i launcher show 1
done
setini 1 game friendly_fire 0
setini 2 net role host; setini 2 net peer_host 10.9.9.9; setini 2 net peer_port 1
mdk_kill; sleep 2
for i in 1 2; do rm -f "$(dir $i)/coop/launcher.txt"; done

# 1. host: pages, friendly fire on, Host with Enter (Play has the focus at first)
P1=$(start 1); P2=$(start 2); echo "$P1 $P2" > "$MDK2_PIDS"
check "host: launcher up before the game window"        wait_log 1 "^launcher: shown at 1280x720" 30
check "client: launcher up"                             wait_log 2 "^launcher: shown at 1280x720" 30
check "no co-op before Play"                            bash -c "! grep -q 'coop_init' '$(mdk_log 1)'"
shot 1 play; lcmd 1 "$T_SET"; shot 1 settings; lcmd 1 "$T_ABOUT"; shot 1 about
lcmd 1 "$T_SET" "$FF"; shot 1 settings_ff
lcmd 1 "$T_PLAY" "$HOST" "key 9" "key 9" "key 9" "key 9" "key 13"   # Tab to the port field, Enter plays
check "host: Host chosen"                               wait_log 1 "^launcher: play, host over UDP" 10
check "host: friendly fire written on Play"             test "$(ini 1 friendly_fire coop.ini)" = 1
check "host: co-op on"                                  wait_log 1 "^coop_init: lua functions registered" 30

# 2. client: join over UDP at a typed address
lcmd 2 "$JOIN"; shot 2 join_udp_old
lcmd 2 "$ADDR" "${BS[@]}" "text 127.0.0.1" "$PORT" "${BS[@]}" "text ${MDK2_PORT}"; shot 2 join_udp
lcmd 2 "$PLAY"
check "client: Join chosen"                             wait_log 2 "^launcher: play, join over UDP" 10
check "client: role, address and port written"          bash -c "[ '$(ini 2 role coop.ini)' = client ] && [ \"\$(grep -E '^peer_host=' '$(dir 2)/coop/coop.ini' | tr -d '\r')\" = peer_host=127.0.0.1 ] && grep -qE '^peer_port=${MDK2_PORT}' '$(dir 2)/coop/coop.ini'"
check "host: the client joined from 127.0.0.1"          wait_log 1 "^player 2 joined from 127\.0\.0\.1" 40
check "mode remembered"                                 bash -c "[ '$(ini 1 mode coop.ini)' = host ] && [ '$(ini 2 mode coop.ini)' = join ]"

# 3. client solo, with a new resolution (settings.lua is shared by all copies: kept and put back)
settings="$(mdk_wsl "$(powershell.exe -NoProfile -Command '$env:LOCALAPPDATA' | tr -d '\r')")/MDK2HD/settings.lua"
cp "$settings" "$out/settings.lua.before"
mdk_kill; sleep 2
P2=$(start 2); echo "$P2" > "$MDK2_PIDS.solo"
check "solo: launcher up"                               wait_log 2 "^launcher: shown" 30
lcmd 2 "$T_SET" "$RES"; shot 2 settings_res
lcmd 2 "$T_PLAY" "$SOLO"; shot 2 solo
lcmd 2 "$PLAY"
check "solo: chosen"                                    wait_log 2 "^launcher: play, solo \(co-op off\)" 10
check "solo: video line written"                        wait_log 2 "^launcher: video chSetVideoMode" 5
check "solo: coop_init does nothing"                    wait_log 2 "^coop_init: solo start" 30
check "solo: no co-op"                                  bash -c "! grep -q 'lua functions registered' '$(mdk_log 2)'"
sleep 8
check "solo: game still running"                        mdk_alive "$P2"
check "settings.lua: one video line, loader kept"       bash -c "[ \$(grep -c chSetVideoMode '$settings') = 1 ] && grep -q 'mdk2-coop >>>' '$settings' && ! cmp -s '$settings' '$out/settings.lua.before'"
cp "$settings" "$out/settings.lua.after"
mdk_kill; sleep 2
cp "$out/settings.lua.before" "$settings"

# 4. Esc quits; the game never starts
P2=$(start 2)
check "quit: launcher up"                               wait_log 2 "^launcher: shown" 30
lcmd 2 "key 27"
sleep 3
check "quit: logged"                                    has 2 "^launcher: quit"
check "quit: game gone"                                 gone "$P2"

# 5. show=0: straight into the game, co-op on as last chosen (join)
setini 2 launcher show 0
P2=$(start 2)
check "show=0: no launcher"                             wait_log 2 "^launcher: off" 30
check "show=0: co-op on"                                wait_log 2 "^coop_init: lua functions registered" 30
mdk_kill; sleep 2
setini 2 launcher show 1

# 6. Steam's join launch (+connect_lobby) skips the launcher
if [ "${MDK2_PROCESS_SERVICE:-0}" = 1 ]; then     # as mdk_start: see tools/mdk.sh
    P2=$(powershell.exe -NoProfile -Command "(Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='\"$(mdk_dir 2)\\mdk2hd.exe\" +connect_lobby 109775240000000000';CurrentDirectory='$(mdk_dir 2)'}).ProcessId" | tr -d '\r')
else
    P2=$(powershell.exe -NoProfile -Command "(Start-Process -PassThru -FilePath '$(mdk_dir 2)\\mdk2hd.exe' -WorkingDirectory '$(mdk_dir 2)' -ArgumentList '+connect_lobby','109775240000000000').Id" | tr -d '\r')
fi
check "+connect_lobby: launcher skipped"                wait_log 2 "^launcher: skipped, Steam started the game to join a lobby" 30
mdk_kill; sleep 2
for i in 1 2; do setini $i test launcher 0; done

printf '%s\n' "${results[@]}" | tee "$out/results.txt"
echo "$pass passed, $fail failed; shots in $out"
[ "$fail" = 0 ]
