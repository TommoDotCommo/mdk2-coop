#!/usr/bin/env bash
# Automated co-op checks with two real game instances on this PC (host + client, UDP
# on localhost). Installs the current build (tools/install.sh), drives both games with key
# and mouse input, then checks bridge.log on each side. Needs the two game copies, windowed.
# Takes about 2 minutes. Background test mode: the games sit muted behind other windows on the
# second monitor and get scripted input only, so the PC stays usable. Leaves the settings.lua line in.
#   tools/qa.sh            run every check
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null   # always test what is in the repo; host autostarts 1-2

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"   # state dumps and reports
# state <tag>: both games dump their state; passes when statecheck.py finds no FAIL
state() { mdk_state "$1" "$out" > "$out/report_$1.txt" 2>&1; }
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }

mdk_boot
read -r A B < "$MDK2_PIDS"
H=1; C=2

# 1. sessions and level following
check "both games running"                      bash -c "$(declare -f mdk_alive); mdk_alive $A && mdk_alive $B"
check "client got the host's packets"           has $C "first packet from peer"
check "client followed the host into level 1"   has $C "host started level 1 checkpoint 2: following"

# 2. enemies
sleep 2
spawned=$(count $H "^ent spawn"); bound=$(count $C "^ent bind")
check "host announced level enemies ($spawned)" test "$spawned" -gt 0
check "client bound all of them ($bound)"       test "$bound" -eq "$spawned"
check "state matches after loading"             state load

# 2b. client sniper rounds (right mouse toggles sniper mode; fire before any enemy is attacking the
#     client: hit reactions interrupt the switch)
mdk_rclick "$B"; sleep 2; mdk_click "$B" 100 3; sleep 2; mdk_rclick "$B"; sleep 1
check "client sniper rounds replayed on host"   has $H "replay fire type 406"

# 3. host-spawned enemy, client damage through the host
mdk_key "$A" 63; sleep 3                                    # F5: host spawns coop_dog1
check "client bound the host's new Doganboy"    has $C "ent bind .* -> coop_dog1"
# the client hits it: F1 on the client deals 10 x 2 gun damage from its Kurt (aiming with the
# mouse depends on where the Doganboy walks); the HIT path is the same as for real shots
mdk_key "$B" 59; sleep 3
host_hp=$(grep -E "^event 1 on coop_dog1" "$(mdk_log $H)" | sed 's/.*-> hp //' | tr '\n' ' ')
client_hp=$(grep -E "^event 2 on coop_dog1" "$(mdk_log $C)" | sed 's/.*-> hp //' | tr '\n' ' ')
check "client hits reached the host"            test -n "$host_hp"
check "enemy hp identical on both sides"        test "$host_hp" = "$client_hp"
# the client's own copy of the Doganboy hits it locally (copied attack animation); those hits
# must be dropped: every damage the client's Kurt took has to have come as HURT from the host
local_hits=$(grep -cE "^damage coop_dog1 -> bob" "$(mdk_log $C)"); hurts=$(count $C "^event 3 ")
host_hits=$(grep -cE "^damage coop_dog1 -> coop_p2" "$(mdk_log $H)")
check "client hurt only via host ($hurts HURT for $host_hits host hits; $local_hits local dropped)" test "$hurts" -eq "$host_hits"

# 3b. same-named spawns (spawners name every spawn alike) and Poopsy damage: the client must
#     create separate copies and survive hitting them (the Poopsy's damage handler reads the
#     attacker; a NULL attacker crashed the client in the first manual session)
mdk_key "$A" 59; sleep 1; mdk_key "$A" 59; sleep 3            # F1 twice: two "coop_spawn"
# the first is unique when announced and goes by name; the second must go as n<id>
check "first coop_spawn announced by name"      has $H "^ent spawn [0-9a-f]+ coop_spawn type 217"
check "duplicate announced as n<id>"            bash -c "grep -A50 -E '^ent spawn [0-9a-f]+ coop_spawn' '$(mdk_log $H)' | grep -qE '^ent spawn ([0-9a-f]+) n\\1 type 217'"
check "client made a separate copy for it"      has $C "^ent bind ([0-9a-f]+) -> n\\1 \\(local [0-9a-f]+, type 217\\)"
mdk_click "$B" 2500 1; sleep 2
check "client survived hitting the Poopsies"    bash -c "$(declare -f mdk_alive); mdk_alive $B"
check "state matches after spawns and hits"     state hits

# 4. the host's enemy shots on the client (the Doganboy fires when the host backs off)
mdk_key "$A" 31 1 2000; sleep 4
# the Doganboy keeps firing: count the host's shots, give them 2 s to arrive, then compare
fired=$(count $H "^fire type [0-9]+ at "); sleep 2; replayed=$(count $C "^replay fire")   # sent ones log a position
check "host shots replayed on client ($fired fired, $replayed replayed)"  test "$replayed" -ge "$fired"

# 5. pickup and rooms/doors (client teleports to the decoy item, then into door dr1_03)
mdk_key "$B" 62; sleep 3
check "client picked up the decoy"              has $C "^pickup l1_r3dummy"
check "host removed its decoy"                  has $H "peer picked up l1_r3dummy: removed here"
mdk_key "$B" 62; sleep 3
check "host opened dr1_03 for the puppet"       has $H "door dr1_03 opening \(nearer player: puppet 2\)"
check "state matches after pickup and door"     state pickup

# 6. going down
mdk_key "$B" 61; sleep 2; mdk_key "$A" 61; sleep 2         # F3: 200 damage to self
check "client revived at the host"              has $C "^down: "
check "host revived at the client"              has $H "^down: "
sleep 6     # a lava death that slipped through would reload the checkpoint by now
reload_after_down() { sed -n '/^down: /,$p' "$1" | grep -q '^level '; }
# falling out of the world (F4 cycles decoy -> door -> void; the client used two already)
mdk_key "$B" 62; sleep 3
check "client fell out of the world and came back" has $C "^fell out of the world"
check "no checkpoint reload after going down"   bash -c "$(declare -f reload_after_down); ! reload_after_down '$(mdk_log $H)' && ! reload_after_down '$(mdk_log $C)'"

check "both games still running"                bash -c "$(declare -f mdk_alive); mdk_alive $A && mdk_alive $B"

# 7. checkpoint keys and level 2 (Max: his anim-loop handler deleted a held gob the puppet
#    does not have, killing either game within ~16 s in 2 of 3 runs)
mdk_key "$A" 65                                             # F7: previous checkpoint (1-1)
for i in $(seq 1 30); do sleep 1; grep -q "^level 1 checkpoint 1 started" "$(mdk_log $C)" && break; done
check "F7 took both games to checkpoint 1-1"    has $C "^level 1 checkpoint 1 started"
mdk_key "$A" 66                                             # F8: next level (2-1)
for i in $(seq 1 30); do sleep 1; grep -q "^level 2 checkpoint 1 started" "$(mdk_log $C)" && break; done
check "F8 took both games to level 2"           has $C "^level 2 checkpoint 1 started"
sleep 20
check "both games survive 20 s of level 2 (Max)" bash -c "$(declare -f mdk_alive); mdk_alive $A && mdk_alive $B"
check "state matches in level 2"                state level2
mdk_kill

printf '%s\n' "${results[@]}"
grep -h '^FAIL' "$out"/report_*.txt 2>/dev/null | sed 's/^/  state: /'
echo "state dumps: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
