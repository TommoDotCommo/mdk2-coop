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

for i in $(mdk_seq); do rm -f "$(mdk_wsl "$(mdk_dir "$i")")/coop/saves.ini"; done     # sessions of earlier runs

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
# counts <regex>: that line's count in each client's log; more_than <counts> <regex> <secs>: wait
# until every client has more of it than then
counts() { local i; for i in $(seq 2 "$MDK2_N"); do printf '%s ' "$(count "$i" "$1")"; done; }
more_than() {
    local base=($1) i k ok
    for k in $(seq 1 "$3"); do ok=1
        for i in $(seq 2 "$MDK2_N"); do [ "$(count "$i" "$2")" -gt "${base[$((i - 2))]}" ] || ok=0; done
        [ $ok = 1 ] && return 0; sleep 1; done
    return 1; }
set_pid() { local i=0 p n=""; for p in $(mdk_pids); do i=$((i + 1)); [ $i = "$1" ] && p=$2; n="$n $p"; done; echo "$n" > "$MDK2_PIDS"; }
# player line of a state dump: type hp x y z
player_of() { sed -nE 's/^player ([0-9]+) (-?[0-9]+) (.*)$/\1 \2 \3/p' "$1"; }
dist() { python3 -c "import math,sys; a=list(map(float,sys.argv[1:4])); b=list(map(float,sys.argv[4:7])); print(math.dist(a,b))" "$@"; }
dump_name() { case $1 in 1) echo host;; 2) echo client;; *) echo "client$1";; esac; }
saves_ini() { echo "$(mdk_wsl "$(mdk_dir "$1")")/coop/saves.ini"; }
sessions_in() { grep -c '^\[h' "$(saves_ini "$1")"; }
# the first section of a copy's saves.ini that has at least $2 players recorded: its name
session_of() { awk -v n="$2" '/^\[/ {sect=$0} /^m_n=/ {split($0, a, "="); if (a[2] + 0 >= n) print sect}' "$(saves_ini "$1")" | tr -d '\r' | head -1; }
latest_save() { sed -nE 's/^co-op save ([0-9]+) written.*/\1/p' "$(mdk_log 1)" | tail -1; }

mdk_boot
L=$MDK2_N
check "all $MDK2_N games running"                       all_alive
check "every client followed into level 1-2"            every_client "^level 1 checkpoint 2 started"

# 1. the first co-op save comes by itself 10 s into the level, in every game
check "first co-op save made in every game"             bash -c "for i in \$(seq 1 30); do ok=1; for g in $(mdk_seq | tr '\n' ' '); do grep -q '^co-op save [0-9]* written' \"\$(source $here/mdk.sh; mdk_log \$g)\" || ok=0; done; [ \$ok = 1 ] && exit 0; sleep 1; done; exit 1"

# 1b. every game recorded the session: the same one, with everybody who is playing in it
sleep 2
s1=$(session_of 1 "$MDK2_N")
check "host recorded a session with all $MDK2_N players" test -n "$s1"
for i in $(seq 2 "$MDK2_N"); do
    check "game $i recorded the same session ($s1)"       test "$(session_of "$i" "$MDK2_N")" = "$s1"
done
check "host saved as session in saves.ini (peak $MDK2_N)" bash -c "grep -q '^peak=$MDK2_N' '$(saves_ini 1)' && grep -q '^updated=[0-9]' '$(saves_ini 1)' && grep -q '^host_name=' '$(saves_ini 1)'"
check "one session so far"                              test "$(sessions_in 1)" = 1
mdk_cmd "$(mdk_pid 1)" "coop_log(coop.ShortNames('A, B, C, D, E'))"
check "long player lists are shortened (A, B, C +2)"    wait_log 1 "^lua: A, B, C \+2\s*\$" 5

# 1c. checkpoints are numbered in play order (3-16 sits between 3-6 and 3-7); levels have captions,
#     and 11 and 12 a placeholder
mdk_cmd "$(mdk_pid 1)" "coop_log(coop.CpText(3, 7))"
check "checkpoint 3-7 is the 8th of 12 in level 3"      wait_log 1 "^lua: checkpoint 8 of 12\s*\$" 5
mdk_cmd "$(mdk_pid 1)" "local p, t, h = coop_level_info(10) coop_log(p .. '|' .. t .. '|' .. h)"
check "level 10 has a place, a time and a character"    wait_log 1 "^lua: .+[|][0-9]+:[0-9][0-9] PM[|]Kurt\s*\$" 5
mdk_cmd "$(mdk_pid 1)" "local p, t, h = coop_level_info(11) coop_log(p .. '|' .. t .. '|' .. h)"
check "level 11 has the placeholder"                    wait_log 1 "^lua: Location to be named[|]time unknown[|]Max\s*\$" 5

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
# the host's own enemies against what was saved (clients follow the host, so agreeing with it proves
# nothing): the level 1 dogans wait on their platform and must not have fallen or lost hp
ent_of() { tr -d '\r' < "$1" | grep -a "^ent .* $2$" | head -1; }   # ent <handle> <type> <hp> x y z name
for d in l1_r7dogan1 l1_r7dogan2; do
    read -r _ _ _ hs _ _ zs _ <<< "$(ent_of "$out/state_hostsave_host.txt" $d)"
    read -r _ _ _ hr _ _ zr _ <<< "$(ent_of "$out/state_resumed_host.txt" $d)"
    check "host $d kept its saved height after the resume ($zs -> $zr)" \
        python3 -c "import sys; z1, z2 = float(sys.argv[1]), float(sys.argv[2]); sys.exit(0 if abs(z1 - z2) < 2 else 1)" "${zs:-x}" "${zr:-y}"
    check "host $d did not lose hp across the resume ($hs -> $hr)" \
        python3 -c "import sys; sys.exit(0 if float(sys.argv[2]) >= float(sys.argv[1]) > 0 else 1)" "${hs:-0}" "${hr:-0}"
done

# 4b. resuming went on with the same session: no second one
check "resume kept the one session"                     test "$(sessions_in 1)" = 1
check "no new session was started by the resume"        bash -c "! grep -q 'new co-op session' '$(mdk_log 1)'"

# 5. and the session carries on: checkpoint keys
mdk_key "$(mdk_pid 1)" 66                                     # F8: next level
check "F8 takes everyone to level 2 after the resume"   wait_clients "^level 2 checkpoint 1 started" 40
sleep 10

# 6. level 2 starts with Max's tutorial, where the game cannot save: a host crash there must
#    resume at 2-1 (the checkpoint it got to), not at the last save back in level 1
check "no co-op save possible in Max's tutorial"        bash -c "! grep -q 'co-op save [0-9]* written: level 2' '$(mdk_log 1)'"
w0=$(counts "waiting: Connection to the host lost"); f0=$(counts "host started level 2 checkpoint 1: following")
b0=$(counts "host is back: playing again")
taskkill.exe /PID "$(mdk_pid 1)" /F >/dev/null 2>&1
check "clients wait again"                              more_than "$w0" "waiting: Connection to the host lost" 8
set_pid 1 "$(mdk_start 1)"
check "host back on the title screen again"             wait_log 1 "^level 0 checkpoint 0 started" 40
sleep 5
mdk_cmd "$(mdk_pid 1)" "coop.ResumeLatest()"
check "host resumes at level 2 checkpoint 1"            wait_log 1 "resuming co-op at level 2 checkpoint 1" 10
check "every client follows into level 2"               more_than "$f0" "host started level 2 checkpoint 1: following" 40
check "every client plays again after the second crash" more_than "$b0" "host is back: playing again" 40
sleep 8
check "state matches after resuming at the checkpoint"  state resumed2
check "all $MDK2_N games still running"                 all_alive
# 7. a new game from the level picker is a new session; the old one is kept
mdk_cmd "$(mdk_pid 1)" "coop.BuildStart()"
mdk_cmd "$(mdk_pid 1)" "coop.StartAt(1, 3)"
check "host started a new session"                      wait_log 1 "new co-op session" 30
check "every client followed to level 1 checkpoint 3"   wait_clients "host started level 1 checkpoint 3: following" 40
sleep 12
mdk_cmd "$(mdk_pid 1)" "coop.SaveNow()"; sleep 4
check "two sessions are kept"                           test "$(sessions_in 1)" = 2
check "no Lua errors"                                   bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
grep -h '^FAIL' "$out"/report_*.txt 2>/dev/null | sort | uniq -c | sed 's/^/  state: /'
echo "state dumps and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
