#!/usr/bin/env bash
# Automated co-op checks with two real game instances on this PC (host + client, UDP
# on localhost). Installs the current build (tools/install.sh), drives both games with key
# and mouse input, then checks bridge.log on each side. Needs the two game copies, windowed.
# Takes about 2 minutes. Background test mode: the games sit muted behind other windows on the
# second monitor and get scripted input only, so the PC stays usable. Leaves the settings.lua line in.
#   tools/qa.sh            run every check
#   MUTANT=overlap|magnet tools/qa.sh   the partner's character kept too close (0.5: inside the collision) or
#                          too far (3, as before): passes only if the standing check catches it
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1   # always test what is in the repo; host autostarts 1-2
case "${MUTANT:-}" in
    "") ;;
    overlap) apart=0.5 ;;
    magnet) apart=3 ;;
    *) echo "unknown MUTANT $MUTANT"; exit 2 ;;
esac
if [ -n "${MUTANT:-}" ]; then
    for i in 1 2; do sed -i "s/^coop.apart = 1\r\?\$/coop.apart = $apart\r/" "$(mdk_wsl "$(mdk_dir $i)")/coop/coop.lua"; done
    grep -q "^coop.apart = $apart" "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua" || { echo "mutant $MUTANT did not apply"; exit 2; }
fi

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

trap mdk_kill EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mdk_boot
read -r A B < "$MDK2_PIDS"
H=1; C=2

# 1. sessions and level following
check "both games running"                      bash -c "$(declare -f mdk_alive); mdk_alive $A && mdk_alive $B"
check "client got the host's packets"           has $C "first packet from peer"
check "client followed the host into level 1"   has $C "host started level 1 checkpoint 2: following"
# both start on the same spot: the host, standing still, is not pushed, and sees its partner right beside it
# (the characters collide at about 0.88 units; the partner used to be kept 3 away and slid off as you came near)
mdk_cmd "$A" 'T_sx, T_sy = coop_pose(mdkGetPlayerGob())'
sleep 5
n0=$(wc -l < "$(mdk_log $H)")
mdk_cmd "$A" 'local x, y = coop_pose(mdkGetPlayerGob()); local a, b = coop_pose(coop.puppets[1])
              coop_log(format("T stand moved %.2f beside %.2f", sqrt((x-T_sx)*(x-T_sx) + (y-T_sy)*(y-T_sy)), sqrt((a-x)*(a-x) + (b-y)*(b-y))))'
sleep 0.5
stand=$(tail -n +$((n0 + 1)) "$(mdk_log $H)" | grep -oE 'T stand moved [0-9.]+ beside [0-9.]+' | tail -1)
echo "$stand" > "$out/stand.txt"
check "standing together: the host is not pushed ($stand)" \
    awk -v m="$(echo "$stand" | awk '{print $4}')" 'BEGIN { exit !(m != "" && m < 0.2) }'
check "standing together: the partner is right beside, not kept away ($stand)" \
    awk -v d="$(echo "$stand" | awk '{print $6}')" 'BEGIN { exit !(d != "" && d >= 0.85 && d < 1.5) }'

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

# 6. Death tests get a fresh checkpoint: enemies spawned during combat above must not
# decide whether the survivor is still standing. Read native life state and wait for
# replication before asking the host to respawn a peer; fixed sleeps raced those updates.
lua_in() { mdk_cmd "$1" "$2"; }
wait_since() {
    local instance=$1 offset=$2 pattern=$3 seconds=$4 n
    for n in $(seq 1 $((seconds * 4))); do
        tail -n +"$((offset + 1))" "$(mdk_log "$instance")" | grep -E "$pattern" >/dev/null && return 0
        sleep 0.25
    done
    return 1
}
# life <game instance> <zero-based player slot> <allowed values regex> <timeout seconds>
# coop_life reads the bridge state, including the life byte actually received from a peer.
life() {
    local instance=$1 slot=$2 expected=$3 deadline=$((SECONDS + $4)) tag value n
    while [ "$SECONDS" -le "$deadline" ]; do
        tag="life_${BASHPID}_${RANDOM}"
        lua_in "$(mdk_pid "$instance")" "coop_log(\"$tag \" .. tostring(coop_life($slot)))" || return 1
        for n in $(seq 1 20); do
            value=$(sed -n "s/^lua: $tag //p" "$(mdk_log "$instance")" | tail -1 | tr -d '\r')
            [ -z "$value" ] || break
            sleep 0.05
        done
        [[ "$value" =~ $expected ]] && return 0
        sleep 0.1
    done
    echo "life: game $instance slot $slot expected $expected, got ${value:-no reply}" >&2
    return 1
}
# A same-checkpoint host reload is intentionally treated as "already here" by clients.
# Hop through 1-1 first so both sides actually discard their previous combat world.
for fixture_cp in 1 2; do
    hload=$(wc -l < "$(mdk_log $H)"); cload=$(wc -l < "$(mdk_log $C)")
    lua_in "$A" "mdkNewGame(1, $fixture_cp)"
    check "death fixture 1-$fixture_cp loaded on host"   wait_since $H "$hload" "^level 1 checkpoint $fixture_cp started" 40
    check "death fixture 1-$fixture_cp loaded on client" wait_since $C "$cload" "^level 1 checkpoint $fixture_cp started" 40
done
check "host starts the fixture alive"          life $H 0 '^0$' 5
check "client starts the fixture alive"        life $C 1 '^0$' 5
check "both worlds agree before death tests"   state death_start
h0=$(wc -l < "$(mdk_log $H)"); c0=$(wc -l < "$(mdk_log $C)")

mdk_key "$B" 61                                           # F3: lava to self, ordinary damage hook
check "client in lava: dead, no body to reach"  wait_since $C "$c0" 'life: alive -> dead \(no body to reach\)' 4
check "host sees its dead client"              life $H 1 '^[23]$' 4
check "host remains alive for its client"      life $H 0 '^0$' 3
check "  and spectating the host, or back"      life $C 1 '^[03]$' 6
lua_in "$A" "coop_revive_checkpoint()"
check "  a checkpoint (or its cleared zone) brings it back" life $C 1 '^0$' 6
check "host sees the client alive again"       life $H 1 '^0$' 4

# Select the void test spot explicitly; this phase no longer depends on earlier F4 presses.
cvoid=$(wc -l < "$(mdk_log $C)")
lua_in "$B" "coop.tpnext = 3"
mdk_key "$B" 62
check "client fell out of the world: dead"      wait_since $C "$cvoid" 'life: alive -> dead \(fell out of the world\)' 4
check "host sees its client fall"              life $H 1 '^[23]$' 4
check "host still stands after the fall"       life $H 0 '^0$' 3
lua_in "$A" "coop_revive_checkpoint()"
check "  and back"                             life $C 1 '^0$' 6
check "host sees the client back after falling" life $H 1 '^0$' 4

hdeath=$(wc -l < "$(mdk_log $H)")
mdk_key "$A" 61
check "host in lava: dead too"                  wait_since $H "$hdeath" 'life: alive -> dead \(no body to reach\)' 4
check "client sees its dead host"              life $C 0 '^[23]$' 4
check "client remains alive for its host"      life $C 1 '^0$' 3
lua_in "$A" "coop_revive_checkpoint()"
check "  and back"                             life $H 0 '^0$' 6
check "client sees its host alive again"       life $C 0 '^0$' 4
check "no checkpoint reload: one always stood" bash -c "! tail -n +$((h0 + 1)) '$(mdk_log $H)' | grep -q '^level ' && ! tail -n +$((c0 + 1)) '$(mdk_log $C)' | grep -q '^level '"

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
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
if [ -n "${MUTANT:-}" ]; then
    printf '%s\n' "${results[@]}" | grep -q "^FAIL  standing together" && { echo "mutant $MUTANT caught"; exit 0; }
    echo "mutant $MUTANT NOT caught"; exit 1
fi
grep -h '^FAIL' "$out"/report_*.txt 2>/dev/null | sed 's/^/  state: /'
echo "state dumps: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
