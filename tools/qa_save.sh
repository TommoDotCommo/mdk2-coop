#!/usr/bin/env bash
# Drop-out checks with MDK2_N (default 3) games in background test mode: co-op saves in every
# game, a client that crashes and comes back with its hp and position, a client whose network
# drops for 15 s, and a host that crashes and resumes the co-op save with everyone.
#   MDK2_N=3 tools/qa_save.sh
# Game copies: see tools/mdk.sh. About 5 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_save.sh needs MDK2_N >= 3"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" MDK2_SAVE_INTERVAL=600 "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-save$MDK2_N"; mkdir -p "$out"
state() { mdk_state "$1" "$out" > "$out/report_$1.txt" 2>&1; }
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
every_game() { local i; for i in $(mdk_seq); do has "$i" "$1" || return 1; done; }
every_client() { local i; for i in $(seq 2 "$MDK2_N"); do has "$i" "$1" || return 1; done; }
wait_log() {   # wait_log <instance> <regex> <seconds>
    local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
wait_clients() {   # wait_clients <regex> <seconds>
    local i; for i in $(seq 1 "$2"); do every_client "$1" && return 0; sleep 1; done; return 1; }
set_pid() { local i=0 p n=""; for p in $(mdk_pids); do i=$((i + 1)); [ $i = "$1" ] && p=$2; n="$n $p"; done; echo "$n" > "$MDK2_PIDS"; }
# player line of a state dump: type hp x y z
player_of() { sed -nE 's/^player ([0-9]+) (-?[0-9]+) (.*)$/\1 \2 \3/p' "$1"; }
dist() { python3 -c "import math,sys; a=list(map(float,sys.argv[1:4])); b=list(map(float,sys.argv[4:7])); print(math.dist(a,b))" "$@"; }
dump_name() { case $1 in 1) echo host;; 2) echo client;; *) echo "client$1";; esac; }
latest_save() { sed -nE 's/^co-op save ([0-9]+) written.*/\1/p' "$(mdk_log 1)" | tail -1; }

mdk_boot
L=$MDK2_N
check "all $MDK2_N games running"                       all_alive
check "every client followed into level 1-2"            every_client "^level 1 checkpoint 2 started"

# 1. the first co-op save comes by itself 10 s into the level, in every game
check "first co-op save made in every game"             bash -c "for i in \$(seq 1 30); do ok=1; for g in $(mdk_seq | tr '\n' ' '); do grep -q '^co-op save [0-9]* written' \"\$(source $here/mdk.sh; mdk_log \$g)\" || ok=0; done; [ \$ok = 1 ] && exit 0; sleep 1; done; exit 1"

# 2. the last client takes 30 damage and moves; a save catches that; then its game crashes
mdk_cmd "$(mdk_pid $L)" "mdkDealDamage(nil, mdkGetPlayerGob(), 30, 2, -1)"
mdk_key "$(mdk_pid $L)" 17 1 1200; sleep 2
mdk_cmd "$(mdk_pid 1)" "coop.SaveNow()"; sleep 3
save=$(latest_save)
check "save $save written in every game"                every_game "^co-op save $save written"
state saved
before=$(player_of "$out/state_saved_$(dump_name $L).txt")
lastnum=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log $L)" | tail -1)
mdk_key "$(mdk_pid $L)" 31 1 1500; sleep 2                    # walk away from the saved spot
taskkill.exe /PID "$(mdk_pid $L)" /F >/dev/null 2>&1; sleep 2
set_pid "$L" "$(mdk_start $L)"
check "restarted client loaded its co-op save $save"    wait_log $L "loading our co-op save $save" 60
check "and got player number $lastnum back"             wait_log $L "^we are player $lastnum\s*\$" 5
check "host knew it was the same player"                has 1 "^player $lastnum (joined .*back again|restarted its game)"
check "host sent the world again after the load"        wait_log 1 "^player $lastnum loaded a save: sending the world again" 30
sleep 8
state rejoin
after=$(player_of "$out/state_rejoin_$(dump_name $L).txt")
read -r _ hp_b xb yb zb <<< "$before"; read -r _ hp_a xa ya za <<< "$after"
check "rejoined client has its saved hp ($hp_b -> $hp_a)" test "$hp_a" = "$hp_b"
d=$(dist "$xb" "$yb" "$zb" "$xa" "$ya" "$za" 2>/dev/null || echo 999)
check "rejoined client is where it saved (${d%.*} away)" python3 -c "import sys; sys.exit(0 if float('$d') < 3 else 1)"
check "state matches after the rejoin"                  grep -q "^state: 0 fail" "$out/report_rejoin.txt"

# 3. client 2's network drops for 15 s: it waits, the host lets it go, then it plays on
p2num=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log 2)" | tail -1)
mdk_cmd "$(mdk_pid 2)" "coop_netblock(15000)"
check "cut-off client waits for the host"              wait_log 2 "waiting: Connection to the host lost" 6
check "host noticed player $p2num leave"                wait_log 1 "^player $p2num left" 15
check "client plays again once back"                    wait_log 2 "host is back: playing again" 20
check "host took it back as player $p2num"              wait_log 1 "^player $p2num joined .*back again" 5
check "client kept its game (no reload)"                wait_log 2 "we are still there" 5
sleep 5
check "state matches after the network drop"            state netdrop

# 4. the host crashes; everyone waits; the host comes back and resumes the co-op save
mdk_cmd "$(mdk_pid 1)" "coop.SaveNow()"; sleep 3
save=$(latest_save)
state hostsave
taskkill.exe /PID "$(mdk_pid 1)" /F >/dev/null 2>&1
check "clients wait when the host is gone"              wait_clients "waiting: Connection to the host lost" 8
sed -i '/^coop.autostart/d' "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
set_pid 1 "$(mdk_start 1)"
check "clients see the host restarted"                  wait_clients "the host restarted its game" 40
check "host back on the title screen"                   wait_log 1 "^level 0 checkpoint 0 started" 40
sleep 5
mdk_cmd "$(mdk_pid 1)" "coop.ResumeLatest()"
check "host resumed co-op save $save"                   wait_log 1 "co-op save $save loaded" 40
check "every client loaded its copy of save $save"      wait_clients "loading our co-op save $save" 30
check "every client plays again"                        wait_clients "host is back: playing again" 30
for i in $(seq 2 "$MDK2_N"); do
    check "client $i kept its player number through the host restart" \
        bash -c "[ \"\$(grep -E '^we are player' '$(mdk_log $i)' | sort -u | wc -l)\" = 1 ]"
done
sleep 8
state resumed
for i in $(seq 2 "$MDK2_N"); do
    b=$(player_of "$out/state_hostsave_$(dump_name $i).txt"); a=$(player_of "$out/state_resumed_$(dump_name $i).txt")
    read -r _ hb x1 y1 z1 <<< "$b"; read -r _ ha x2 y2 z2 <<< "$a"
    check "client $i hp kept through the host crash ($hb -> $ha)" test "$ha" = "$hb"
done
check "state matches after the host resumed"            grep -q "^state: 0 fail" "$out/report_resumed.txt"

# 5. and the session carries on: checkpoint keys
mdk_key "$(mdk_pid 1)" 66                                     # F8: next level
check "F8 takes everyone to level 2 after the resume"   wait_clients "^level 2 checkpoint 1 started" 40
sleep 10
check "all $MDK2_N games still running"                 all_alive
check "no Lua errors"                                   bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
grep -h '^FAIL' "$out"/report_*.txt 2>/dev/null | sort | uniq -c | sed 's/^/  state: /'
echo "state dumps and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
