#!/usr/bin/env bash
# Chat and pings with MDK2_N (default 3) games in background test mode: a message from a client
# reaches the host and the other client (with the sender's name), the host's reaches everyone, a
# ping shows up for all with its sender and place, and both are in the frame (pixels).
#   MDK2_N=3 tools/qa_chat.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-chat$MDK2_N"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()  { grep -qE "$2" "$(mdk_log "$1")"; }
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
L=$MDK2_N

mdk_boot
lua 2 'coop_set_name("Kurt Fan")'; lua 1 'coop_set_name("Host Hank")'; sleep 2
lua 2 'coop_say("hello from the client")'
check "host got the client's message"       wait_log 1 "^chat: Kurt Fan: hello from the client" 10
check "other client got it, named"          wait_log $L "^chat: Kurt Fan: hello from the client" 10
lua 1 'coop_say("a message from the host that is long enough to be split across two event fields ok")'
check "client got the host's message (cut at 62)" wait_log 2 "^chat: Host Hank: a message from the host that is long enough to be split across[[:space:]]*$" 10
lua 2 'coop_ping(10, 20, 30)'
check "host got the ping, with sender"      wait_log 1 "^ping: Kurt Fan at \(10\.0, 20\.0, 30\.0\)" 10
check "other client got the ping"           wait_log $L "^ping: Kurt Fan at \(10\.0, 20\.0, 30\.0\)" 10
lua 2 'coop_typing(1)'; sleep 1; lua 1 'coop_chat_dump()'; sleep 1
check "ping live on the host"               has 1 "^ping live: 2 Kurt Fan"
check "host sees the client typing"          has 1 "^typing: 2 Kurt Fan"
lua 1 'mdkGobSetPositionXYZ(mdkGetPlayerGob(), 0, 0, 0)' ; sleep 1
mdk_shot "$(mdk_pid 1)" "$out/chat_host.png"
check "no Lua errors"                       bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
