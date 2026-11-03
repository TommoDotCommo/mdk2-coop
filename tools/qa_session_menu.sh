#!/usr/bin/env bash
# The in-game Session page and the DPS chooser, live, with two games started from the launcher's lobby:
#  * the host's Options > Co-op > Session > Copy invite puts the lobby's invite on the clipboard: the same code the
#    launcher's own dump showed (so same secret and routes), which an independent decoder (tools/invite_decode.py)
#    accepts, in the message form "<name> is hosting MDK2. Join: https://<join page>/#<CODE>"
#  * a guest's Session > Leave asks once ("The others keep playing"), the guest is still in the level until it
#    says yes, then it is out of the level
#  * Ctrl+M cycles the DPS meter and writes [hud] dps back; the in-game Players and interface chooser then shows
#    the same value
#   tools/qa_session_menu.sh
#   MUTANT=nowriteback tools/qa_session_menu.sh   Ctrl+M leaves coop.ini alone: passes only if the DPS check fails
# The clipboard is the one thing shared with the desktop: it is saved before the copy check and put back after.
# Game copies: see tools/mdk.sh. About 3 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
python3 "$here/invite_decode.py" --selftest || { echo "FAIL the invite decoder's own check"; exit 1; }
if [ "${MUTANT:-}" = nowriteback ]; then
    make -s -B -C "$here/../bridge" COOP_MUTANT_CFLAGS=-DCOOP_MUTANT_DPS_NO_WRITEBACK >/dev/null || exit 1
fi
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null || exit 1
[ "${MUTANT:-}" = nowriteback ] && make -s -B -C "$here/../bridge" >/dev/null    # leave a normal DLL in the tree

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-session-menu"; mkdir -p "$out"
check() { local name="$1"; shift
          if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
lua()   { mdk_cmd "$(mdk_pid "$1")" "$2"; }
ini_dps() { tr -d '\r' < "$(dir 1)/coop/coop.ini" | awk -F= '/^\[/ { s = $0 } s == "[hud]" && $1 == "dps" { print $2 }'; }
clip()  { powershell.exe -NoProfile -Command 'Get-Clipboard -Raw' | tr -d '\r'; }
# Esc, then Options > Co-op with keys as a player would (the pages after that by name)
open_coop() { local p; p=$(mdk_pid "$1")
              mdk_key "$p" 1; sleep 2
              mdk_key "$p" 208 3; sleep 0.5; mdk_key "$p" 28; sleep 2
              mdk_key "$p" 208 4; sleep 0.5; mdk_key "$p" 28; sleep 1.5; }
# the last "T92 <tag> <value>" line the game wrote with coop_log
tagged() { grep -E "T92 $2 " "$(mdk_log "$1")" | tail -1 | sed "s/.*T92 $2 //" | tr -d '\r'; }

trap 'mdk_kill' EXIT
names=(Hosty Alice)
for i in 1 2; do
    setini $i test launcher 1; setini $i launcher show 1; setini $i launcher lobby 1
    printf '[player]\r\nid=0000000%d\r\nname=%s\r\n' "$i" "${names[$((i - 1))]}" > "$(dir $i)/coop/player.ini"
    rm -f "$(dir $i)/coop/launcher.txt"
done
setini 1 hud dps 0
printf '\r\ncoop.autostart = { 1, 2 }\r\ncoop.autostart_players = 1\r\n' >> "$(dir 1)/coop/coop.lua"
mdk_kill; sleep 2
: > "$MDK2_PIDS"
for i in 1 2; do mdk_start "$i" >> "$MDK2_PIDS"; sleep 2; done
for i in 1 2; do wait_log "$i" "^launcher: shown" 60 || true; done

# the lobby: the host opens it, Alice joins and is ready; the launcher's own dump has the invite it would copy
lcmd 1 "click 1000 660"
check "host: lobby open"                         wait_log 1 "^lobby: open as host" 10
lcmd 2 "click 1000 660"
check "guest: connected to the host"             wait_log 2 "^lobby: connected to the host" 15
sleep 3                                          # the host's checks (adapters, probe of its own lobby) publish a route
lcmd 1 dump
launcher_code=$(tr -d '\r' < "$(dir 1)/coop/launcher-state.ini" | sed -n 's/^invite_code=//p' | head -1)
check "the launcher's own dump has an invite code" test -n "$launcher_code"
lcmd 2 "key 13"; sleep 2; lcmd 1 "key 13"
check "host: game started"                       wait_log 1 "^launcher: play, host over UDP" 25
check "guest: followed the host into level 1"    wait_log 2 "host started level 1 checkpoint 2: following" 120
check "host: guest in the game"                  wait_log 1 "^player 2 joined" 30
sleep 4
lua 1 'coop_log("T92 hostlevel " .. tostring(coop.level))'; lua 2 'coop_log("T92 guestlevel " .. tostring(coop.level))'
check "both in a level"                          bash -c "[ '$(tagged 1 hostlevel)' = 1 ] && [ '$(tagged 2 guestlevel)' = 1 ]"

# ---- host: Session > Copy invite ----
saved_clip=$(powershell.exe -NoProfile -Command 'Get-Clipboard -Raw' | tr -d '\r')
powershell.exe -NoProfile -Command 'Set-Clipboard -Value "t92-sentinel"'
open_coop 1
lua 1 'T_k = 7 if (coop.ResumePoint()) then T_k = T_k + 1 end if (coop.level and coop.level >= 1) then T_k = T_k + 1 end coop.ShowPage("coopsession") mdkMenuSetHighlighted(menu.coopsession, T_k)'
sleep 1; mdk_shot "$(mdk_pid 1)" "$out/host_session.png"
mdk_key "$(mdk_pid 1)" 28; sleep 1.5
copied=$(clip)
powershell.exe -NoProfile -Command "Set-Clipboard -Value @'
$saved_clip
'@" >/dev/null 2>&1
printf '%s\n' "$copied" > "$out/clipboard.txt"
check "Copy invite: the clipboard holds an invite"   bash -c "! [ '$copied' = 't92-sentinel' ] && grep -q 'MDK2-' '$out/clipboard.txt'"
decoded=$(python3 "$here/invite_decode.py" "$copied" 2>"$out/decode.err"); echo "$decoded" > "$out/decoded.json"
check "Copy invite: the independent decoder accepts it" test -n "$decoded"
check "Copy invite: message form 'is hosting' + link"   bash -c "echo '$decoded' | grep -q '\"message\": true' && echo '$decoded' | grep -q '\"link\": true'"
check "Copy invite: the host's port"                    bash -c "echo '$decoded' | grep -q '\"port\": $MDK2_PORT'"
check "Copy invite: at least one route"                 bash -c "echo '$decoded' | grep -q '\"routes\": \[{'"
launcher_code_norm=$(echo "$launcher_code" | tr -d '-' | tr a-z A-Z); copied_code=$(echo "$copied" | grep -o 'MDK2-[0-9A-Za-z-]*' | head -1 | tr -d '-' | tr a-z A-Z)
check "Copy invite: the very code the launcher showed"  test -n "$launcher_code_norm" -a "$launcher_code_norm" = "$copied_code"
lua 1 'coop_ctrl_show(0) HideMenu() mdkPauseGame(0)'; sleep 1

# ---- guest: Session > Leave ----
open_coop 2
lua 2 'coop.ShowPage("coopsession") mdkMenuSetHighlighted(menu.coopsession, 6)'
sleep 1; mdk_shot "$(mdk_pid 2)" "$out/guest_session.png"
before=$(count 2 "^lua: leave: The others keep playing|leave: The others keep playing")
mdk_key "$(mdk_pid 2)" 28; sleep 1.5
lua 2 'coop_log("T92 guestlevel_asking " .. tostring(coop.level))'
mdk_shot "$(mdk_pid 2)" "$out/guest_leave_confirm.png"
check "Leave asks once, with what happens to the others" bash -c "[ $(count 2 'leave: The others keep playing') -eq $((before + 1)) ]"
check "Leave: still in the level until it says yes"      bash -c "[ '$(tagged 2 guestlevel_asking)' = 1 ]"
lua 2 'mdkMenuSetHighlighted(menu.quitconfirm, 0)'; mdk_key "$(mdk_pid 2)" 28; sleep 3
lua 2 'coop_log("T92 guestlevel_after " .. tostring(coop.level))'
check "Leave: yes leaves the level"                      bash -c "[ '$(tagged 2 guestlevel_after)' != 1 ]"
check "Leave asked once only"                            bash -c "[ $(count 2 'leave: The others keep playing') -eq $((before + 1)) ]"

# ---- host: Ctrl+M writes the value back and the chooser agrees ----
lua 1 'coop_set("hud", "dps", 0)'; sleep 3
for step in 1 2 0 1; do
    lua 1 'coop_press(29,500) coop_press(50,250)'; sleep 1.2
    lua 1 'coop.ShowPage("coopui") coop_log("T92 dpsrow " .. mdkMenuItemGetWidgitValue(coop.dps_item)) HideMenu() mdkPauseGame(0)'; sleep 1
    check "Ctrl+M -> $step: [hud] dps in coop.ini"        test "$(ini_dps)" = "$step"
    check "Ctrl+M -> $step: the Players and interface chooser shows $step" test "$(tagged 1 dpsrow)" = "$step"
done
lua 1 'coop.ShowPage("coopui")'; sleep 1; mdk_shot "$(mdk_pid 1)" "$out/host_players_and_interface.png"

for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "evidence: $out"
echo "$pass passed, $fail failed"
if [ "${MUTANT:-}" = nowriteback ]; then [ "$fail" -gt 0 ]; else [ "$fail" -eq 0 ]; fi
