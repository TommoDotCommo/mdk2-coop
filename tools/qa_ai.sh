#!/usr/bin/env bash
# Enemy AI against players who are not the host: a live Doganboy placed beside one client only
# must hurt that client (the host's AI picks it, HURT reaches it, its hp falls) and nobody else.
# Needs MDK2_N >= 3: host, a client that stays with the host, and a client that walks away.
#   tools/qa_ai.sh        (about 2 minutes; isolated copies and MDK2_BACKGROUND as in qa_n.sh)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_ai.sh needs MDK2_N >= 3"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
wait_log() {   # wait_log <instance> <regex> <seconds>
    local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
hp_of() {   # hp_of <instance>: its player's hp, from a log line of its own
    local tag="HP$RANDOM$RANDOM"
    mdk_cmd "$(mdk_pid "$1")" "coop_log(format(\"$tag %d\", coop_hp(mdkGetPlayerGob()) or -1))"
    wait_log "$1" "^lua: $tag " 10 >/dev/null
    grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/.* //' | tr -d '\r'; }
hurts()  { count "$1" '^event 3 '; }              # HURT events a game applied to its own player
bites()  { count 1 "^damage coop_dog1 -> $1 "; }  # the host's dog hitting a body (host log)

beside() {   # beside <n>: the dog still stands within 6 units (z too) of player n's puppet on the host
    local tag="NEAR$RANDOM$RANDOM"
    mdk_cmd "$HP" "local p = coop_gob(\"@p$1\") coop_log(format(\"$tag %d\", (abs(coop_dog1.position.x - p.position.x) < 6 and abs(coop_dog1.position.y - p.position.y) < 6 and abs(coop_dog1.position.z - p.position.z) < 6) and 1 or 0))"
    wait_log $H "^lua: $tag 1" 10; }
place_dog() {   # place_dog <n>: put the dog beside player n's puppet; a teleport sometimes drops it, so retry
    local t
    for t in 1 2 3; do
        mdk_cmd "$HP" "local p = coop_gob(\"@p$1\") mdkGobSetPositionXYZ(coop_dog1, p.position.x + 1.5, p.position.y, p.position.z + 1)"
        sleep 1
        beside "$1" && return 0
    done
    return 1; }

mdk_boot
H=1; C2=2; L=$MDK2_N; HP=$(mdk_pid 1); LP=$(mdk_pid "$L")
check "all $MDK2_N games running"                       all_alive
sleep 4
lastnum=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log $L)" | tail -1)
c2num=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log $C2)" | tail -1)
# gentle enemies: the dog must bite, not wipe the team
mdk_cmd "$HP" 'coop_set("game", "dmg_taken", 25)'

# 1. the last client walks into the next room, away from the host and client 2
mdk_key "$LP" 62; sleep 3                                     # F4: onto the decoy in the next room
check "the last client moved away (pickup of the decoy)" has $L "^pickup l1_r3dummy"

# 2. control: a dog beside the host and client 2 bites them and never the faraway client
mdk_key "$HP" 63; sleep 3                                     # F5: coop_dog1, 6 units ahead of the host
dog_bound() { has $C2 'ent bind .* -> coop_dog1' && has $L 'ent bind .* -> coop_dog1'; }
check "dog bound on every client"                       dog_bound
h_l=$(hurts $L); h_2=$(hurts $C2)
sleep 8
check "control: the dog bit someone near the host ($(bites bob) host, $(bites coop_p$c2num) client 2)" test $(( $(bites bob) + $(bites coop_p$c2num) )) -gt 0
check "control: the faraway client was not bitten"      test "$(bites coop_p$lastnum)" -eq 0
check "control: and felt nothing"                       test "$(hurts $L)" -eq "$h_l"

# 3. the dog is put beside the faraway client only
lp_hp=$(hp_of $L); hl0=$(hurts $L)        # before the move: its first bites land while we check where the dog is
check "the dog stands beside client $lastnum"           place_dog $lastnum
b2=$(bites coop_p$c2num); bh=$(bites bob); h_2=$(hurts $C2)
check "the dog's AI bit the faraway client"             wait_log $H "^damage coop_dog1 -> coop_p$lastnum " 20
sleep 3
check "client $lastnum applied HURT events from it"     test "$(hurts $L)" -gt "$hl0"
check "and its hp fell ($lp_hp -> $(hp_of $L))"         test "$(hp_of $L)" -lt "$lp_hp"
check "client $c2num was not bitten meanwhile"          test "$(bites coop_p$c2num)" -eq "$b2"
check "and felt nothing"                                test "$(hurts $C2)" -eq "$h_2"
check "the host was not bitten meanwhile"               test "$(bites bob)" -eq "$bh"

# 4. and the other client too: the dog moves to client 2
c2_hp=$(hp_of $C2); hc0=$(hurts $C2)
check "the dog stands beside client $c2num"             place_dog $c2num
check "the dog's AI bit client $c2num"                  wait_log $H "^damage coop_dog1 -> coop_p$c2num " 20
sleep 3
check "client $c2num applied HURT events from it"       test "$(hurts $C2)" -gt "$hc0"
check "and its hp fell ($c2_hp -> $(hp_of $C2))"        test "$(hp_of $C2)" -lt "$c2_hp"

check "no Lua errors anywhere"                          bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-ai"; mkdir -p "$out"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
