#!/usr/bin/env bash
# The first enemy of level 1, played for real: in room 2 a Doganboy waves a sign behind glass. Sniping the
# sign (r2_dog01target) shatters the glass, knocks the dog down and sets it on the players; when it dies the
# door dr1_02 unlocks. The sniper round is a real one (coop_test_sniper, the native launch path), not a spawned
# enemy: this is what a group meets in its first minutes. Checks, with the host and two clients in the room:
#   - the sign is there in every game, the sniper's shot reaches the host and the glass sequence runs everywhere
#   - the dog goes after each player in turn (host or client) and hurts only that player
#   - every player's hits (host and both clients) lower the one hp the host keeps, and every game shows it
#   - the kill happens in every game and dr1_02 opens for each player
#   - the script trace (bridge/scripttrace.c): the dog's OnDie ran in every game, and tools/script_diff.py finds no
#     handler of a synced or partly synced event that ran on the host only
#   SNIPER=<n> tools/qa_sign.sh    which player fires the sniper round (default 2, a client; 1 is the host)
#   MUTANT=noondie tools/qa_sign.sh   clients skip the dog's OnDie (the bug this test was written for): must fail
#   Targeting geometry (docs/TESTING.md): while the dog works on player n every other player waits in another room of the
#   level, more than a thousand units away, and the distance is asserted from every game's own state dump; the dog's
#   native target (the host's target query) must be player n.
#   MUTANT=hostaoe tools/qa_sign.sh   the host does not route the dog's direct hits to remote players: must fail
#   MUTANT=near tools/qa_sign.sh      the other players stand beside player n as the old test had it: the geometry check must fail
# Needs MDK2_N >= 3, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 3 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_sign.sh needs MDK2_N >= 3"; exit 1; }
SNIPER="${SNIPER:-2}"
MDK2_BACKGROUND=1 MDK2_START="1 1" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-sign"; mkdir -p "$out"
check() {
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
lua()   { mdk_cmd "$(mdk_pid "$1")" "$2"; }
# q <game> <lua value expression>: that game's value, read back from its own log
q() {
    local tag="Q$RANDOM$RANDOM"
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    wait_log "$1" "^lua: $tag " 10 >/dev/null
    grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'
}
wait_q() {   # wait_q <game> <lua expr> <expected> <seconds>
    local t; for t in $(seq 1 "$4"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }
every() {   # every <lua expr> <expected>: the value in every game
    local i; for i in $(mdk_seq); do [ "$(q "$i" "$1")" = "$2" ] || return 1; done; }
every_eventually() {   # every_eventually <seconds> <lua expr> <expected>
    local t; for t in $(seq 1 "$1"); do every "$2" "$3" && return 0; sleep 1; done; return 1; }
place() {   # place <game> <x> <y>: that game's own player there, in room 2
    lua "$1" "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $2, $3, -30)"; }
ondie_everywhere() { local i; for i in $(mdk_seq); do has "$i" '^script event L1: OnDie r2_dog01' || return 1; done; }
hostile() { q 1 'coop_hp(r2_dog01)'; }

mdk_boot
H=1
# The test games start invulnerable; the dog's hits are judged on each player's own hp, so turn that off.
for i in $(mdk_seq); do lua "$i" 'God(0)'; done
if [ "${MUTANT:-}" = noondie ]; then for i in $(seq 2 "$MDK2_N"); do lua "$i" 'coop_test_noondie(1)'; done; fi
case "${MUTANT:-}" in ''|noondie|near) : ;; hostaoe) lua 1 'coop_presence_test(0,0,1,0)' ;; *) echo "Unknown sign mutant"; exit 2 ;; esac
# everyone into room 2, in front of the glass (the dog is behind it at y 152, the players come in at y 172)
for i in $(mdk_seq); do place "$i" "$((70 + i * 4))" 172; done
sleep 4
check "all games running"                               all_alive
check "the sign is in every game"                       every 'r2_dog01target ~= nil and 1 or 0' 1
check "the dog waves behind the glass in every game (ai 0)" every 'r2_dog01.ai' 0
check "every game bound the host's dog"                 bash -c "grep -q 'ent bind .* -> r2_dog01' '$(mdk_log 2)' && grep -q 'ent bind .* -> r2_dog01' '$(mdk_log 3)'"
hp0=$(hostile)

# 1. the sign is sniped by one player, for real: aimed at it, one native sniper round
aim='local t = r2_dog01target local p = mdkGetPlayerGob() mdkGobPointToXYZ(p, t.position.x - p.position.x, t.position.y - p.position.y, t.position.z - p.position.z) coop_test_sniper()'
# the round has the engine's random aim spread (from 8 units away the board in front of the sign takes the hits): fire
# from where the players come in until one lands on the sign itself
place "$SNIPER" 78 172; sleep 2
for t in $(seq 1 40); do
    has "$SNIPER" 'damage .* -> r2_dog01target' && break
    lua "$SNIPER" "$aim"; sleep 2
done
check "player $SNIPER's round hit the sign"             has "$SNIPER" 'damage .* -> r2_dog01target'
check "the host saw the hit on the sign"                bash -c "[ $SNIPER = 1 ] || grep -qE '^event 1 on r2_dog01target' '$(mdk_log 1)'"
check "the glass sequence ran in every game (sign gone)" every_eventually 20 'r2_dog01target == nil and 1 or 0' 1
# the host's dog is loose (its KNOCKDOWN animation looped and the script set ai 1). A client's mirror takes its animation
# from the host's snapshots and never runs that loop event, so its own ai stays 0; nothing there depends on it
check "the host's dog is loose (ai 1)"                  wait_q 1 'r2_dog01.ai' 1 20
sleep 4

# 2. it picks on one player at a time; place it beside that player, the others wait in another room.
# What counts is read from each game's own process (state dump): that player's own hp went down, the hit arrived
# there as a direct hit from the dog, and nobody else lost hp. The host's log line "damage r2_dog01 -> ..." is
# written even when the host does not forward the hit to the client, so it proves nothing about the client.
beside() {   # beside <n>: the dog next to player n (on the host: its own player, or n's puppet)
    if [ "$1" = 1 ]; then lua 1 'local p = mdkGetPlayerGob() mdkGobSetPositionXYZ(r2_dog01, p.position.x + 1.5, p.position.y, p.position.z + 1)'
    else lua 1 "local p = coop_gob(\"@p$1\") mdkGobSetPositionXYZ(r2_dog01, p.position.x + 1.5, p.position.y, p.position.z + 1)"; fi; }
geo_check() { mdk_assert_far "$@" | tee -a "$out/geometry.txt"; return "${PIPESTATUS[0]}"; }   # keep the reason: check hides output
dump_of() {   # dump_of <game>: path of a fresh state dump written by that game's own process
    local tag="own$RANDOM" f t; f="$(mdk_wsl "$(mdk_dir "$1")")/coop/state_${tag}_$1.txt"; rm -f "$f"
    lua "$1" "coop_state(\"${tag}_$1\")"; for t in $(seq 1 150); do [ -f "$f" ] && break; sleep 0.1; done; echo "$f"; }
field() {   # field <game> <row> <column>: one value from that game's own state dump
    awk -v row="$2" -v col="$3" '$1 == row { print $col; exit }' "$(dump_of "$1")" | tr -d '\r'; }
own_hp()   { field "$1" player 3; }
receipts() { field "$1" direct_receipts 2; }
dog_id=$(awk '{ sub(/\r$/, "") } $1 == "ent" && $NF == "r2_dog01" { print $2 }' "$(dump_of 1)" | tr -d '\r')
host_target() {   # the host's native target query for the dog, from its own state dump
    awk '$1 == "ai" && $2 == "r2_dog01" { print $3 }' "$(dump_of 1)" | tr -d '\r'; }
for n in $(mdk_seq); do
    for i in $(mdk_seq); do
        if [ "$i" = "$n" ]; then place "$i" 78 156
        elif [ "${MUTANT:-}" = near ]; then place "$i" "$((56 + i * 10))" 191
        else lua "$i" 'mdkGobSetPosition(mdkGetPlayerGob(), "l1_r8doganland1")'; fi   # another room, 1000+ units away
    done
    declare -A hp0=() rc0=(); for i in $(mdk_seq); do hp0[$i]=$(own_hp "$i"); rc0[$i]=$(receipts "$i"); done
    check "the dog works on player $n; every other player is outside every splash and strike radius" geo_check "$n" "dog on player $n"
    for t in 1 2 3 4 5 6 7 8; do beside "$n"; sleep 1; [ "$(own_hp "$n")" -lt "${hp0[$n]}" ] && break; done
    sleep 1
    hp1=$(own_hp "$n")
    check "the dog hurt player $n: that game's own hp ${hp0[$n]} -> $hp1" test "$hp1" -lt "${hp0[$n]}"
    check "the hit reached player $n's game as a direct hit from the dog" bash -c "[ '$(receipts "$n")' -gt '${rc0[$n]}' ] && case '$(field "$n" direct_source 2)' in r2_dog01|n$dog_id) true ;; *) false ;; esac"
    check "the host's native target for the dog was player $n" test "$(host_target)" = "@p$n"
    for i in $(mdk_seq); do
        [ "$i" = "$n" ] && continue
        check "and not player $i while it was on player $n (that game's own hp)" test "$(own_hp "$i")" -ge "${hp0[$i]}"
    done
done

# 3. every player's hits lower the host's hp for the dog, and every game shows the same number
for n in $(mdk_seq); do
    for i in $(mdk_seq); do place "$i" "$((60 + i * 6))" 188; done
    h0=$(hostile)
    lua "$n" 'mdkDealDamage(mdkGetPlayerGob(), r2_dog01, 20, 1, -1)'; sleep 1
    lua "$n" 'mdkDealDamage(mdkGetPlayerGob(), r2_dog01, 20, 1, -1)'; sleep 2
    h1=$(hostile)
    check "player $n's hits lowered the dog's hp ($h0 -> $h1)" test "$h1" -lt "$h0"
    check "every game shows the host's hp ($h1) after player $n's hits" every_eventually 10 'coop_hp(r2_dog01)' "$h1"
done

# 4. the kill, by the last client; the door unlocks for everyone
lua "$MDK2_N" 'mdkDealDamage(mdkGetPlayerGob(), r2_dog01, 2000, 1, -1)'
check "the dog is gone in every game"                   every_eventually 15 'coop_alive(r2_dog01)' nil
check "the clients ran the dog's OnDie"                 bash -c "grep -q 'ent despawn .* died: running its OnDie' '$(mdk_log 2)' && grep -q 'ent despawn .* died: running its OnDie' '$(mdk_log 3)'"
for n in $(mdk_seq); do
    for i in $(mdk_seq); do [ "$i" = "$n" ] && place "$i" 78 121 || place "$i" "$((60 + i * 6))" 188; done
    sleep 3
    check "dr1_02 opened for player $n on the host"     wait_log 1 "$([ "$n" = 1 ] && echo '^door dr1_02 opening \(nearer player: ours\)' || echo "^door dr1_02 opening \\(nearer player: puppet $n\\)")" 10
    for i in $(seq 2 "$MDK2_N"); do
        check "dr1_02 opened for player $n in game $i"  wait_log "$i" "^door dr1_02 opening" 10
    done
    for i in $(mdk_seq); do place "$i" "$((60 + i * 6))" 188; done
    sleep 3
done
check "all games still running"                         all_alive
check "the dog's OnDie ran in every game (script trace)" ondie_everywhere
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
check "no promised script event ran on the host only"   python3 "$here/script_diff.py" "$out/game1.log" $(for i in $(seq 2 "$MDK2_N"); do printf '%s ' "$out/game$i.log"; done)
check "no Lua errors anywhere"                          bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
