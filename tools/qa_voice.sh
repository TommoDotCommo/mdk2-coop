#!/usr/bin/env bash
# Proximity voice checks with 3 games in background test mode (no audio devices: a test tone
# stands in for the mic, the speakers are simulated): a client's voice reaches the host and, relayed,
# the other client at full volume up close; a far-away client is not sent it (the host filters by
# range) and its volume is zero; the host's own voice reaches both clients; range "everywhere"
# reaches the far client again; the speaker's name tag is marked while they talk. Screenshots
# of the tag go to the output folder.
#   tools/qa_voice.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-voice"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()  { grep -qE "$2" "$(mdk_log "$1")"; }
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
# info <instance> <slot>: "frames gain lost" of that speaker, as that instance sees it
info() { lua "$1" "local f, g, l = coop_voice_info($2) coop_log(format(\"VOICE %d %d %d\", f, g, l))"; sleep 1
         grep -E "^lua: VOICE " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: VOICE //' | tr -d '\r'; }
frames() { info "$1" "$2" | cut -d' ' -f1; }
gain()   { info "$1" "$2" | cut -d' ' -f2; }

mdk_boot
check "voice starts in test mode"                       has 1 "^voice: on, push to talk V"

# 1. client 2 (slot 1) talks, everyone close together
lua 2 'coop_voice_test(3500)'; sleep 1
lua 1 'coop_log(format("TALK %d", coop_voice_talking(1)))'; sleep 0.5
mdk_shot "$(mdk_pid 3)" "$out/talking_tag.png"
mdk_shot "$(mdk_pid 1)" "$out/talking_panel.png"
sleep 2.5
h1=$(frames 1 1); c1=$(frames 3 1)
check "host got client 2's voice ($h1 frames)"           test "${h1:-0}" -ge 20
check "client 3 got it relayed ($c1 frames)"             test "${c1:-0}" -ge 20
check "close up: full volume (gain $(gain 3 1))"         test "$(gain 3 1)" -ge 90
check "talking flag seen"                                has 1 "^lua: TALK 1"

# 2. client 3 walks far away (the game puts it back after a few seconds, so everything in this
# step happens right after): the host stops sending it client 2's and its own voice
f2a=$(frames 2 0); f3a=$(frames 3 0)
lua 3 'local p = mdkGetPlayerGob() mdkGobSetPositionXYZ(p, p.position.x + 4000, p.position.y, p.position.z)'; sleep 1
lua 2 'coop_voice_test(700)'; lua 1 'coop_voice_test(700)'; sleep 1.2
g=$(gain 3 1)
check "far client 3's volume is zero (gain $g)"          test "$g" -eq 0
sleep 1.5
h2=$(frames 1 1); c2=$(frames 3 1)
f2b=$(frames 2 0); f3b=$(frames 3 0)
check "host still hears client 2 ($h1 -> $h2)"           test "$h2" -gt "$h1"
check "far client 3 gets none of client 2 ($c1 -> $c2)"  test "$c2" = "$c1"
check "near client 2 hears the host ($f2a -> $f2b)"      test "$f2b" -gt "$f2a"
check "far client 3 does not ($f3a -> $f3b)"             test "$f3b" = "$f3a"
sleep 4

# 4. range everywhere (host setting): the far client hears again at full volume
lua 1 'coop_set("voice", "range", 0)'; sleep 1
lua 2 'coop_voice_test(1500)'; sleep 3
c3=$(frames 3 1)
check "range everywhere: far client hears ($c2 -> $c3)"  test "$c3" -gt "$c2"
check "and at full volume (gain $(gain 3 1))"            test "$(gain 3 1)" -ge 90

check "no Lua errors"                                    bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
