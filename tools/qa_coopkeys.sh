#!/usr/bin/env bash
# The shared co-op keys with 2 games in background test mode (scripted input only): the key bindings the bridge
# holds, hold Tab (the player list: its rows read from the bridge, mute and volume clicked with the mouse, closed
# when Tab is released), V and the middle mouse ping, the mouse side button (and B) push to talk (frames sent),
# [ and ] and the list's Watch while out of the fight, the Modern preset's open inventory without Tab, the
# join notice and the leave box's "what happens to the others" line. Everything asserted from bridge state.
#   MDK2_BUILD=overhaul MDK2_OVERHAUL_EXE=<exe> tools/qa_coopkeys.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-coopkeys"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
lua()   { mdk_cmd "$(mdk_pid "$1")" "$2"; }
wait_log() { local i; for i in $(seq 1 $(( $3 * 4 ))); do has "$1" "$2" && return 0; sleep 0.25; done; return 1; }
# ask <instance> <lua expression> <regex>: log the expression's value there and match it
ask() { local tag="ask$RANDOM"; lua "$1" "coop_log(\"$tag \" .. tostring($2))"
        wait_log "$1" "^lua: $tag " 4 && grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //" | grep -qE "$3"; }
# plist <instance>: "open rows clicks" of the player list as that game's bridge holds it
plist() { local tag="PL$RANDOM"; lua "$1" "local o, r, c = coop_plist_info() coop_log(\"$tag \" .. o .. \" \" .. r .. \" \" .. c)"; sleep 0.8
          grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed "s/^lua: $tag //" | tr -d '\r'; }
# gain <instance> <slot>: how loud that speaker is for that instance (%)
gain() { local tag="VG$RANDOM"; lua "$1" "local f, g, l = coop_voice_info($2) coop_log(\"$tag \" .. g)"; sleep 0.8
         grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed "s/^lua: $tag //" | tr -d '\r'; }
frames() { local tag="VF$RANDOM"; lua "$1" "local f, g, l = coop_voice_info($2) coop_log(\"$tag \" .. f)"; sleep 0.8
           grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed "s/^lua: $tag //" | tr -d '\r'; }
tx() { local tag="TX$RANDOM"; lua "$1" "coop_log(\"$tag \" .. coop_voice_tx())"; sleep 0.8
       grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed "s/^lua: $tag //" | tr -d '\r'; }
point() { local tag="PT$RANDOM"; lua "$1" "coop_log(\"$tag \" .. tostring(coop_plist_point($2, $3)))"; sleep 0.6
          grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed "s/^lua: $tag //" | tr -d '\r'; }
click() { lua "$1" 'coop_press(256, 200)'; sleep 1; }

mdk_boot
H=1; C=2
lua $H 'coop.SetPreset("modern")'; lua $C 'coop.SetPreset("modern")'; sleep 2
sleep 4

# 1. the bindings the bridge and the game hold
check "player list: Tab"                                has $H "^game keys 6: 15/-1"
check "push to talk: B and the mouse side button"       has $H "^game keys 3: 48/259"
check "watch previous / next: [ and ]"                  bash -c "grep -qE '^game keys 7: 26/-1' '$(mdk_log $H)' && grep -qE '^game keys 8: 27/-1' '$(mdk_log $H)'"
check "ping: V and the middle mouse"                    has $H "^game keys 1: 47/258"
check "Modern: the game's open-inventory command is F only, not Tab"  ask $H 'omGetCommandBinding(COM_INVENABLE) .. " " .. omGetCommandBinding2(COM_INVENABLE)' "^33 -1$"
check "voice and watch keys cannot be given to another action (reserved)"  ask $H 'coop.Reserved("kuse", 15) .. coop.Reserved("jump", 1003) .. coop.Reserved("jump", 26)' "^listvoicewatchp$"

# 2. hold Tab: the list opens with both players, and closes when Tab is let go
lua $H 'coop_press(15, 120000)'; sleep 2
mdk_shot "$(mdk_pid $H)" "$out/list_open.png"
check "Tab held: the list is open on the host"          test "$(plist $H | cut -d' ' -f1-2)" = "1 2"
lua $H 'local a = coop_plist_row(1) local b = coop_plist_row(2) coop_log("ROWS " .. a .. " " .. b)'; sleep 0.8
check "  the rows are the host (slot 0) and the client (slot 1)"  has $H "^lua: ROWS 0 1$"
check "  the client's list is not open"                test "$(plist $C | cut -d' ' -f1)" = "0"

# 3. the mouse: mute and volume of the client's voice, as the host hears it
lua $C 'coop_voice_test(20000)'; sleep 2
g0=$(gain $H 1)
check "the client's voice is at full volume first ($g0%)"  test "${g0:-0}" -ge 90
check "the Mute button is on screen"                    test "$(point $H 0 1)" = "1"
click $H
check "Mute: the client is silent for the host"         test "$(gain $H 1)" = "0"
lua $H 'coop_log("MUTED " .. coop_voice_mute(1))'; sleep 0.8
check "  and muted is state in the bridge"              has $H "^lua: MUTED 1$"
point $H 0 1 >/dev/null; click $H
check "Unmute: back at full volume"                     bash -c "[ \"$(gain $H 1)\" -ge 90 ]"
point $H 2 1 >/dev/null; click $H
check "Volume +: 120% for that speaker only"            test "$(gain $H 1)" -ge 115
point $H 1 1 >/dev/null; click $H; click $H
check "Volume - twice: 80%"                             bash -c "g=$(gain $H 1); [ \$g -ge 75 ] && [ \$g -le 85 ]"
check "Watch is not offered while we are in the fight"  test "$(point $H 3 1)" = "0"
check "the clicks were counted by the list"             bash -c "[ $(plist $H | cut -d' ' -f3) -ge 6 ]"
mdk_shot "$(mdk_pid $H)" "$out/list_clicked.png"
lua $H 'coop_voice_mute(1, 0) coop_press(15, 0)'; sleep 1.5
check "Tab let go: the list is closed"                  test "$(plist $H | cut -d' ' -f1)" = "0"

# 4. V and the middle mouse ping; the other copy gets them
p0=$(count $C "^ping: "); lua $H 'coop_press(47, 200)'; sleep 2
p1=$(count $C "^ping: ")
check "V pings: the other copy got it ($p0 -> $p1)"     test "$p1" -gt "$p0"
lua $H 'coop_press(258, 200)'; sleep 2
check "the middle mouse button pings too ($p1 -> $(count $C '^ping: '))"  test "$(count $C '^ping: ')" -gt "$p1"

# 5. push to talk on the mouse side button (and B); nothing pressed sends nothing
sleep 3
t0=$(tx $H); sleep 2; t1=$(tx $H)
check "nothing pressed: no voice sent ($t0 -> $t1)"     test "$t1" = "$t0"
lua $H 'coop_press(259, 1500)'; sleep 3
t2=$(tx $H)
check "the side button talks: frames sent ($t1 -> $t2)" test "$t2" -gt "$((t1 + 20))"
f0=$(frames $C 0); lua $H 'coop_press(259, 1200)'; sleep 3; f1=$(frames $C 0)
check "  and the client received them ($f0 -> $f1)"    test "$f1" -gt "$((f0 + 15))"
lua $H 'coop_press(48, 1500)'; sleep 3
check "B still talks ($t2 -> $(tx $H))"                 test "$(tx $H)" -gt "$((t2 + 20))"

# 6. out of the fight: [ and ] and the list's Watch. The client is put out (lava), then spectates the host.
lua $C 'mdkDealDamage(nil, mdkGetPlayerGob(), 500, DAMAGE_LAVAFALL, -1)'
check "the client is out of the fight"                  wait_log $C "life: alive -> dead" 5
check "  and watches the host"                          wait_log $C "revive: spectating player" 8
k0=$(count $C "^revive: watch "); lua $C 'coop_press(27, 200)'; sleep 1.5
check "] steps to the next player (state machine ran: $k0 -> $(count $C '^revive: watch '))"  test "$(count $C '^revive: watch ')" -gt "$k0"
check "  and says which way"                            has $C "^revive: watch next -> player 1"
lua $C 'coop_press(26, 200)'; sleep 1.5
check "[ steps to the previous player"                  has $C "^revive: watch previous -> player 1"
lua $C 'coop_press(15, 120000)'; sleep 2
check "out of the fight: the list is open on the client"  test "$(plist $C | cut -d' ' -f1-2)" = "1 2"
check "Watch is offered for the host"                   test "$(point $C 3 0)" = "1"
click $C
check "Watch: the revive state machine took it"         wait_log $C "^plist: watch player 1: yes" 4
mdk_shot "$(mdk_pid $C)" "$out/list_watch.png"
lua $C 'coop_press(15, 0)'; sleep 1.5
check "the client's list closed on release"             test "$(plist $C | cut -d' ' -f1)" = "0"
lua $H 'coop_revive_checkpoint()'; sleep 3

# 7. words: the join notice, and what leaving does to the others
check "the host got 'joined' for the client"            has $H "^chat: \(game\): .* joined$"
lua $C 'menu.SwitchDlg("quitconfirm")'; sleep 1
check "client: leaving keeps the others playing"        has $C "^lua: leave: The others keep playing"
lua $C 'HideMenu() mdkPauseGame(0)'
lua $H 'menu.SwitchDlg("quitconfirm")'; sleep 1
check "host: leaving returns everyone to the lobby"     has $H "^lua: leave: Everyone returns to the lobby"
lua $H 'HideMenu() mdkPauseGame(0)'

check "no Lua errors"                                   bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
