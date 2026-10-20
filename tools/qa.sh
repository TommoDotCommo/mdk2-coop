#!/usr/bin/env bash
# Automated co-op checks with two real game instances on this PC (host + client, UDP
# on localhost). Drives both with key and mouse input, then checks bridge.log on each
# side. Needs: both game copies with the bridge DLL and coop.ini (role=host / client),
# tools/inject.sh done, the game windowed. Takes about 2 minutes; the game windows will
# take focus.
#   tools/qa.sh            run every check
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
mdk_tools_install

pass=0; fail=0; results=()
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
check "client followed the host into level 1"   has $C "peer started level 1 checkpoint 2: following"

# 2. enemies
sleep 2
spawned=$(count $H "^ent spawn"); bound=$(count $C "^ent bind")
check "host announced level enemies ($spawned)" test "$spawned" -gt 0
check "client bound all of them ($bound)"       test "$bound" -eq "$spawned"

# 3. host-spawned enemy, client damage through the host
mdk_key "$A" 63; sleep 3                                    # F5: host spawns coop_dog1
check "client bound the host's new Doganboy"    has $C "ent bind .* -> coop_dog1"
mdk_click "$B" 2500 1; sleep 2                              # client fires at it
host_hp=$(grep -E "^event 1 on coop_dog1" "$(mdk_log $H)" | sed 's/.*-> hp //' | tr '\n' ' ')
client_hp=$(grep -E "^event 2 on coop_dog1" "$(mdk_log $C)" | sed 's/.*-> hp //' | tr '\n' ' ')
check "client hits reached the host"            test -n "$host_hp"
check "enemy hp identical on both sides"        test "$host_hp" = "$client_hp"
# the client's own copy of the Doganboy hits it locally (copied attack animation); those hits
# must be dropped: every damage the client's Kurt took has to have come as HURT from the host
local_hits=$(grep -cE "^damage coop_dog1 -> bob" "$(mdk_log $C)"); hurts=$(count $C "^event 3 ")
host_hits=$(grep -cE "^damage coop_dog1 -> coop_p2" "$(mdk_log $H)")
check "client hurt only via host ($hurts HURT for $host_hits host hits; $local_hits local dropped)" test "$hurts" -eq "$host_hits"

# 4. shots both ways (the Doganboy fires when the host backs off; the client snipes)
mdk_key "$A" 31 1 2000; sleep 4
fired=$(count $H "^fire type"); replayed=$(count $C "^replay fire")
check "host shots replayed on client ($fired/$replayed)"  bash -c "[ $fired -eq 0 ] || [ $replayed -ge $((fired - 1)) ]"
mdk_key "$B" 57; sleep 1; mdk_click "$B" 100 2; sleep 2; mdk_key "$B" 57; sleep 1
check "client sniper rounds replayed on host"   has $H "replay fire type 406"

# 5. pickup and rooms/doors (client teleports to the decoy item, then into door dr1_03)
mdk_key "$B" 62; sleep 3
check "client picked up the decoy"              has $C "^pickup l1_r3dummy"
check "host removed its decoy"                  has $H "peer picked up l1_r3dummy: removed here"
mdk_key "$B" 62; sleep 3
check "host opened dr1_03 for the puppet"       has $H "door dr1_03 opening \(nearer player: puppet\)"

# 6. going down
mdk_key "$B" 61; sleep 2; mdk_key "$A" 61; sleep 2         # F3: 200 damage to self
check "client revived at the host"              has $C "^down: "
check "host revived at the client"              has $H "^down: "

check "both games still running"                bash -c "$(declare -f mdk_alive); mdk_alive $A && mdk_alive $B"
mdk_kill

printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
