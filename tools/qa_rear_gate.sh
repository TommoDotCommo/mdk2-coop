#!/usr/bin/env bash
# Level 2: r9 entrance must not lock the r8 rear gate until the group crosses.
# LEADER=2 exercises a client leading instead of the host.
# Uses the native door animation callback and reads the actual engine lock bit.
# MUTANT=earlylock restores the original callback: the trailing-player checks must fail.
# Needs two isolated background game copies; the QA runner supplies their lease.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="2 7" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-rear-gate"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
count() { grep -acE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
q() { local tag="Q$RANDOM$RANDOM"
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    wait_log "$1" "^lua: $tag " 10 >/dev/null
    grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'; }
wait_q() { local t; for t in $(seq 1 "$4"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }
to() { lua "$1" "mdkGobSetPosition(mdkGetPlayerGob(), \"$2\")"; }
trap 'mdk_kill' EXIT

leader="${LEADER:-1}"
[[ "$leader" = 1 || "$leader" = 2 ]] || exit 2
trailer=$((3 - leader))
mdk_boot
sleep 2
if [ "${MUTANT:-}" = earlylock ]; then
    for i in 1 2; do
        lua "$i" 'if coop.rear_gate_original then l2_r9entrdoor.OnAnimLoop=coop.rear_gate_original end; coop.RearGateTick=function() end'
    done
fi
check "both peers loaded level 2" bash -c "grep -q '^level 2 checkpoint 7 started' '$(mdk_log 1)' && grep -q '^level 2 checkpoint 7 started' '$(mdk_log 2)'"
check "rear gate initially unlocked on host" wait_q 1 'coop_door_locked(l2_r8entrdoor06)' 0 5
check "rear gate initially unlocked on client" wait_q 2 'coop_door_locked(l2_r8entrdoor06)' 0 5
# Reproduce a leader beyond r8 and a trailing player still inside it.
lua "$trailer" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1198,951,235)'
lua "$leader" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1281,902,235)'
check "native r9 opening callback fired on host" wait_log 1 'script event L2: OnAnimLoop l2_r9entrdoor' 15
check "native r9 opening callback fired on client" wait_log 2 'script event L2: OnAnimLoop l2_r9entrdoor' 15
sleep 2
check "host leaves rear gate unlocked for trailing player" wait_q 1 'coop_door_locked(l2_r8entrdoor06)' 0 3
check "client leaves rear gate unlocked for trailing player" wait_q 2 'coop_door_locked(l2_r8entrdoor06)' 0 3
check "trailing player stays in r8 without catch-up teleport" wait_q "$trailer" 'floor(mdkGetPlayerGob().position.x)' 1198 3
# A client cannot commit the authoritative result.
lua 2 'coop_puzzle(4)'
sleep 1
check "client request cannot lock the host gate" wait_q 1 'coop_door_locked(l2_r8entrdoor06)' 0 3
# Returning must remain possible; this used to leave the trailing player trapped.
lua "$leader" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1234,946,235)'
sleep 2
check "leader can return without locking the group out" wait_q 2 'coop_door_locked(l2_r8entrdoor06)' 0 3
# Only when both players cross may the original one-way lock take effect.
lua "$leader" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1281,902,235)'
lua "$trailer" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1234,946,235)'
sleep 2
check "rear gate stays unlocked until the last crossing" wait_q 1 'coop_door_locked(l2_r8entrdoor06)' 0 3
lua "$trailer" 'mdkGobSetPositionXYZ(mdkGetPlayerGob(),1261,930,235)'
check "host commits native rear lock after both cross" wait_q 1 'coop_door_locked(l2_r8entrdoor06)' 1 10
check "client receives authoritative rear lock" wait_q 2 'coop_door_locked(l2_r8entrdoor06)' 1 10
# Replay the host's retained result as on reconnect, after removing only the test copy's lock.
lua 2 'mdkProxDoorLock(l2_r8entrdoor06,0); coop_ready()'
check "reconnect history restores completed rear lock" wait_q 2 'coop_door_locked(l2_r8entrdoor06)' 1 15
check "no Lua errors" bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
