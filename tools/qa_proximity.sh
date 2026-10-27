#!/usr/bin/env bash
# Proximity triggers: a handler that starts something when "the player" comes near looked at the local player only, so
# level 7's boss (r8.OnUpdate: the player within 40 of r8_center) started only on the game whose player went there.
# The handlers that only look at the player (tools/script_census.py --proximity-lua) are given the nearest player on
# every game. With a host and a client on level 7: the client goes to the boss and the host starts the fight too; on a
# fresh level the host goes and the client starts it too; and with nobody near, nothing starts. (The first half does not
# tell the old behaviour from the new one: the host's player falls into level 7's death zone at the start and the revive
# logic brings it back next to the living client, near the boss. The second half does, and is what the mutant fails.)
#   MUTANT=nonearest tools/qa_proximity.sh     the handlers are not wrapped (the old behaviour): must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 1" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-proximity"; mkdir -p "$out"
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

# level 7 on both games; the handlers are wrapped a second after the level is up
level7() {
    local n1 n2 t
    n1=$(count 1 '^level 7 checkpoint 1 started'); n2=$(count 2 '^level 7 checkpoint 1 started')
    lua 1 'mdkNewGame(7, 1)'
    for t in $(seq 1 60); do
        [ "$(count 1 '^level 7 checkpoint 1 started')" -gt "$n1" ] && [ "$(count 2 '^level 7 checkpoint 1 started')" -gt "$n2" ] && break; sleep 1; done
    sleep 9
}

mdk_boot
[ "${MUTANT:-}" = nonearest ] && for i in 1 2; do lua "$i" 'coop.no_nearest = 1'; done
level7
check "both games are on level 7"                       bash -c "grep -q '^level 7 checkpoint 1 started' '$(mdk_log 1)' && grep -q '^level 7 checkpoint 1 started' '$(mdk_log 2)'"
check "r8's handler sees the nearest player on the host"   wait_log 1 'proximity: r8.OnUpdate sees the nearest player' 15
check "and on the client"                               wait_log 2 'proximity: r8.OnUpdate sees the nearest player' 15
check "the boss has not started anywhere yet"           bash -c "[ \"$(q 1 'tostring(Save.railnum)')\" = nil ] && [ \"$(q 2 'tostring(Save.railnum)')\" = nil ]"

# the client walks to the boss. r8 locks a door behind whoever enters, which would bring the host to the client (and
# its own proximity would start the boss): the host is marked busy so it stays where it is
lua 1 'Save.inminigame = 1'
to 2 r8_center
check "the client's own game started the boss"          wait_q 2 'tostring(Save.railnum)' 1 15
check "the host started it too"                         wait_q 1 'tostring(Save.railnum)' 1 15
check "the boss's rails run on the host"                wait_log 1 'script event L7: OnCustomKey zizrail01' 25
check "and on the client"                               wait_log 2 'script event L7: OnCustomKey zizrail01' 25
lua 1 'Save.inminigame = nil'

# a fresh level 7: the host walks to the boss
level7
lua 2 'Save.inminigame = 1'
check "nothing started on the fresh level"              bash -c "[ \"$(q 1 'tostring(Save.railnum)')\" = nil ] && [ \"$(q 2 'tostring(Save.railnum)')\" = nil ]"
to 1 r8_center
check "the host started the boss"                       wait_q 1 'tostring(Save.railnum)' 1 15
check "the client started it too"                       wait_q 2 'tostring(Save.railnum)' 1 15
lua 2 'Save.inminigame = nil'
check "no Lua errors"                                   bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
check "both games still running"                        bash -c "for p in $(mdk_pids | tr '\n' ' '); do powershell.exe -NoProfile -Command \"if (-not (Get-Process -Id \$p -ErrorAction SilentlyContinue)) { exit 1 }\" || exit 1; done"
for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
