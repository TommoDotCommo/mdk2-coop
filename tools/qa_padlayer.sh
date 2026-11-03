#!/usr/bin/env bash
# The shared co-op pad layer on View (standard K4-K7) with 2 games in background test mode, through the
# pad-injection hook (coop_pad_fake: a stand-in XInput state the bridge reads instead of a real pad). On the host:
# hold View opens the player list; View + D-pad up opens the emote wheel and the player does not walk (the D-pad
# alone does, as a control); View + RB and the right stick up say "Ready" on the quick chat wheel, which the client
# gets; View + D-pad down opens chat with a keyboard, A types on ours and Menu sends; a tap of View pings; the hints
# and voice follow the last input (pad -> pad buttons and open mic, a key -> keys and push to talk). Everything
# asserted from bridge or game state read through Lua.
#   MDK2_BUILD=overhaul MDK2_OVERHAUL_EXE=<exe> tools/qa_padlayer.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-padlayer"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
lua()   { mdk_cmd "$(mdk_pid "$1")" "$2"; }
wait_log() { local i; for i in $(seq 1 $(( $3 * 4 ))); do has "$1" "$2" && return 0; sleep 0.25; done; return 1; }
# val <instance> <lua expression>: the expression's value in that game
val() { local tag="V$RANDOM$RANDOM"; lua "$1" "coop_log(\"$tag \" .. tostring($2))"
        wait_log "$1" "^lua: $tag " 4 >/dev/null; grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //"; }
# pad <instance> <buttons> [lx ly rx ry]: the stand-in pad's state from now on (XInput bits)
pad() { lua "$1" "coop_pad_fake(${3:-0}, ${4:-0}, ${5:-0}, ${6:-0}, $2, 0, 0)"; }
pos() { val "$1" 'format("%.2f %.2f", coop.real_player().position.x, coop.real_player().position.y)'; }
moved() { python3 -c "import sys; a=list(map(float,'$1'.split())); b=list(map(float,'$2'.split())); print(((a[0]-b[0])**2+(a[1]-b[1])**2)**0.5)"; }
plist_open() { val "$1" 'coop_plist_info()'; }
wheel() { val "$1" 'qa_wheel()'; }
# helpers in the game: several values as one line
helpers='function qa_wheel() local w, s = coop_wheel_state() return w .. " " .. s end
function qa_line(i) local s, t = coop_chat_line(i) if (s) then return s .. " " .. t end return "none" end
function qa_chat() local o, k, t = coop_chat_state() return o .. " " .. k .. " " .. t end
function qa_voice() local m, o = coop_voice_mode() return m .. " " .. o end'
VIEW=32; UP=1; DOWN=2; RB=512; A=4096; START=16

mdk_boot
H=1; C=2
lua $H 'coop.SetPreset("modern")'; lua $C 'coop.SetPreset("modern")'; sleep 2
lua $H "$helpers"; lua $C "$helpers"
lua $H 'coop_pad_fake(0, 0, 0, 0, 0, 0, 0)'; sleep 2      # the stand-in pad is found within a second

# 1. hold View: the player list; the input counts as the pad's
pad $H $VIEW; sleep 1
check "hold View: the player list is open on the host"        test "$(plist_open $H)" = "1"
check "  the last input is the pad"                          test "$(val $H 'coop_pad_state()')" = "1"
mdk_shot "$(mdk_pid $H)" "$out/view_list.png"

# 2. View + D-pad up: the emote wheel, and the D-pad does not walk
p0=$(pos $H)
pad $H $((VIEW | UP)); sleep 0.4; pad $H $VIEW; sleep 0.3; pad $H $((VIEW | UP)); sleep 1.2
check "View + D-pad up: the emote wheel is open (wheel 1)"   bash -c "[[ '$(wheel $H)' =~ ^1\  ]]"
check "  and the player list gave way to it"                 test "$(plist_open $H)" = "0"
mdk_shot "$(mdk_pid $H)" "$out/view_emote.png"
p1=$(pos $H); d1=$(moved "$p0" "$p1")
check "  the player did not walk with View held ($d1 units)"  python3 -c "import sys; sys.exit(0 if $d1 < 0.3 else 1)"
pad $H 0; sleep 0.6
check "let go of View: the wheel closed"                      test "$(wheel $H)" = "0 -1"
pad $H $UP; sleep 1.2; pad $H 0; sleep 0.5
p2=$(pos $H); d2=$(moved "$p1" "$p2")
check "control: the D-pad alone walks ($d2 units)"           python3 -c "import sys; sys.exit(0 if $d2 > 1.0 else 1)"

# 3. View + RB, right stick up, let go: "Ready" said on the quick chat wheel; the client got it
pad $H $((VIEW | RB)); sleep 0.4; pad $H $VIEW 0 0 0 1; sleep 0.8
check "View + RB: the quick chat wheel is open on Ready"     test "$(wheel $H)" = "3 0"
mdk_shot "$(mdk_pid $H)" "$out/view_quick.png"
pad $H 0; sleep 2
check "quick chat: the client's newest line is the host's Ready"  test "$(val $C 'qa_line(0)')" = "0 Ready"

# 4. View + D-pad down: chat with a keyboard (Steam's when it can, else ours); A types, Menu sends
pad $H $((VIEW | DOWN)); sleep 0.4; pad $H 0; sleep 0.6
st=$(val $H 'qa_chat()')
check "View + D-pad down: the chat line is open with a keyboard ($st)"  bash -c "[[ '$st' =~ ^1\ [12] ]]"
mdk_shot "$(mdk_pid $H)" "$out/view_chat.png"
if [[ "$st" =~ ^1\ 2 ]]; then
    pad $H $A; sleep 0.3; pad $H 0; sleep 0.3
    check "  ours: A types the selected key (q)"              test "$(val $H 'qa_chat()')" = "1 2 q"
    check "  the game did not pause (the pad typed, not played)"  bash -c "[ '$(val $H 'gamecontrol.showmenu')' != 1 ]"
    pad $H $START; sleep 0.3; pad $H 0; sleep 2
    check "  Menu sends: the client got q"                    test "$(val $C 'qa_line(0)')" = "0 q"
else
    pad $H $START; sleep 0.3; pad $H 0; sleep 1
fi
check "  the line is closed again"                           bash -c "[[ '$(val $H 'qa_chat()')' =~ ^0 ]]"
check "  and Menu, which sent it, did not open the pause menu"  bash -c "[ '$(val $H 'gamecontrol.showmenu')' != 1 ]"

# 5. a tap of View pings (the client sees the host's ping)
sleep 6
check "no ping before the tap"                               test "$(val $C 'coop_ping_state(0)')" = "0"
pad $H $VIEW; sleep 0.1; pad $H 0; sleep 1.5
check "tap View: the client has the host's ping"             test "$(val $C 'coop_ping_state(0)')" = "1"

# 6. the hints and voice follow the last input: pad -> pad buttons and open mic, a key -> keys and push to talk
li=$(val $H 'coop.PadLayerButtons("list")')
pad $H $A; sleep 0.3; pad $H 0; sleep 0.5
gl=$(val $H 'coop_guide_state(6)')
check "  the guide's player list line shows the pad ($gl)"     test "$gl" = "$li"
check "  voice: automatic, the mic is open"                  test "$(val $H 'qa_voice()')" = "2 1"
lua $H 'coop_press(30, 150)'; sleep 0.8
check "after a key: the last input is the keys"              test "$(val $H 'coop_pad_state()')" = "0"
check "  the guide's player list line shows Hold Tab"        test "$(val $H 'coop_guide_state(6)')" = "Hold Tab"
check "  voice: push to talk again"                          test "$(val $H 'qa_voice()')" = "2 0"
lua $H 'coop_pad_fake()'

check "no Lua errors"                                        bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
