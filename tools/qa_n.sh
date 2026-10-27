#!/usr/bin/env bash
# Co-op checks with MDK2_N (default 3) real game instances on this PC: one host and N-1 clients
# over localhost UDP, in background test mode. What tools/qa.sh cannot see with two: clients
# seeing each other through the host (relayed states, shots, pickups), damage reaching the
# right client, and a client leaving and joining again mid-level.
#   MDK2_N=8 tools/qa_n.sh
# Game copies: see tools/mdk.sh (tools/copies.sh makes more). About 3-4 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_n.sh needs MDK2_N >= 3 (tools/qa.sh covers two)"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-n$MDK2_N"; mkdir -p "$out"
state() { mdk_state "$1" "$out" > "$out/report_$1.txt" 2>&1; }
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
every_client() {   # every_client <regex>: every client's log has it
    local i; for i in $(seq 2 "$MDK2_N"); do has "$i" "$1" || return 1; done; }
wait_log() {   # wait_log <instance> <regex> <seconds>
    local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }

mdk_boot
H=1; L=$MDK2_N; HP=$(mdk_pid 1); LP=$(mdk_pid "$L"); P2=$(mdk_pid 2)
others=$((MDK2_N - 1))

# 1. everyone in, everyone in level 1-2
check "all $MDK2_N games running"                 all_alive
check "host saw $others players join ($(count $H '^player [0-9]+ joined'))" test "$(count $H '^player [0-9]+ joined')" -eq "$others"
check "every client got a player number"          every_client "^we are player [0-9]+"
check "host waited for everyone before starting"  has $H "autostart: level 1 checkpoint 2, $others others here"
check "every client followed into level 1"        every_client "^level 1 checkpoint 2 started"
sleep 4
# each game draws a puppet for each of the others
for i in $(mdk_seq); do
    n=$(count "$i" "^lua: puppet spawned for player")
    check "game $i has puppets for all $others others ($n)" test "$n" -eq "$others"
done

# 2. enemies on every client
spawned=$(count $H "^ent spawn")
for i in $(seq 2 "$MDK2_N"); do
    b=$(count "$i" "^ent bind")
    check "client $i bound the host's enemies ($b of $spawned)" test "$b" -eq "$spawned"
done
check "state matches in every game after loading" state load

# 3. the last client's sniper rounds reach the host and, relayed, the other clients
mdk_rclick "$LP"; sleep 2; mdk_click "$LP" 100 3; sleep 2; mdk_rclick "$LP"; sleep 2
check "last client's sniper rounds replayed on host"     has $H "replay fire type 406"
check "and relayed to client 2"                          has 2 "replay fire type 406"

# 4. host-spawned Doganboy, hit by the last client: hp identical everywhere
mdk_key "$HP" 63; sleep 3                                     # F5: host spawns coop_dog1
check "every client bound coop_dog1"                     every_client "ent bind .* -> coop_dog1"
mdk_key "$LP" 79; sleep 3                                     # Num 1: 10 x 2 gun damage on coop_dog1
host_hp=$(grep -E "^event 1 on coop_dog1" "$(mdk_log $H)" | sed 's/.*-> hp //' | tr '\n' ' ')
check "last client's hits reached the host"              test -n "$host_hp"
for i in $(seq 2 "$MDK2_N"); do
    c_hp=$(grep -E "^event 2 on coop_dog1" "$(mdk_log "$i")" | sed 's/.*-> hp //' | tr '\n' ' ')
    check "coop_dog1 hp on client $i matches the host"     test "$host_hp" = "$c_hp"
done
check "state matches after hits"                         state hits

# 5. damage on a client's puppet in the host's world goes to that client only (HURT)
lastnum=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log $L)" | tail -1)
p2num=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log 2)" | tail -1)
h3=$(count $L "^event 3 "); h2=$(count 2 "^event 3 ")
mdk_cmd "$HP" "mdkDealDamage(nil, coop_gob(\"@p$lastnum\"), 5, 2, -1)"; sleep 2
check "hit on player $lastnum's puppet hurt player $lastnum" test "$(count $L '^event 3 ')" -gt "$h3"
check "and nobody else"                                   test "$(count 2 '^event 3 ')" -eq "$h2"

# 6. pickup and door by the last client, seen by the host and client 2
mdk_key "$LP" 62; sleep 3                                     # F4: onto the decoy
check "last client picked up the decoy"                  has $L "^pickup l1_r3dummy"
check "host removed its decoy"                           has $H "peer picked up l1_r3dummy: removed here"
check "client 2 removed its decoy (relayed)"             has 2 "peer picked up l1_r3dummy: removed here"
mdk_key "$LP" 62; sleep 3                                     # F4: into door dr1_03
check "host opened dr1_03 for player $lastnum"           has $H "door dr1_03 opening \(nearer player: puppet $lastnum\)"
check "client 2 opened dr1_03 for player $lastnum"       has 2 "door dr1_03 opening \(nearer player: puppet $lastnum\)"
check "state matches after pickup and door"              state pickup

# 7. client 2 leaves: everyone notices, its puppet goes; it comes back and joins mid-level
taskkill.exe /PID "$P2" /F >/dev/null 2>&1
check "host saw player $p2num leave"                     wait_log $H "^player $p2num left" 20
check "last client removed player $p2num's puppet"       wait_log $L "player $p2num left: puppet removed" 10
log2_before=$(wc -l < "$(mdk_log 2)" 2>/dev/null || echo 0)
P2=$(mdk_start 2); sed -i "s/^\(\s*[0-9]*\) [0-9]*/\1 $P2/" "$MDK2_PIDS"
check "host saw a player join again"                     wait_log $H "^player [0-9]+ asked to join level 1 checkpoint 2" 40
check "rejoined client loaded level 1-2"                 wait_log 2 "^level 1 checkpoint 2 started" 40
sleep 8
check "rejoined client bound the enemies"                test "$(count 2 '^ent bind')" -ge "$(( spawned - 2 ))"
p2num=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log 2)" | tail -1)
check "the others drew the rejoined player again"        wait_log $L "puppet spawned for player $p2num" 10
check "state matches after the rejoin"                   state rejoin

# 8. checkpoint keys and level 2 for everyone
mdk_key "$HP" 65                                              # F7: previous checkpoint (1-1)
for i in $(seq 1 40); do sleep 1; every_client "^level 1 checkpoint 1 started" && break; done
check "F7 took everyone to checkpoint 1-1"               every_client "^level 1 checkpoint 1 started"
mdk_key "$HP" 66                                              # F8: next level (2-1)
for i in $(seq 1 40); do sleep 1; every_client "^level 2 checkpoint 1 started" && break; done
check "F8 took everyone to level 2"                      every_client "^level 2 checkpoint 1 started"
sleep 20
check "all $MDK2_N games survive 20 s of level 2"         all_alive
check "state matches in level 2"                         state level2
check "no Lua errors anywhere"                           bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
grep -h '^FAIL' "$out"/report_*.txt 2>/dev/null | sort | uniq -c | sed 's/^/  state: /'
echo "state dumps and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
