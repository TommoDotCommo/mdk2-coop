#!/usr/bin/env bash
# Going down, reviving, dying and spectating (bridge/revive.c) with MDK2_N (default 3) games in
# background test mode at 1-2: a lethal hit puts a client down (pose in every game, damage
# ignored, HUD), the host revives it by holding R, it goes down again and bleeds out (30 s) to
# dead and spectating (puppet parked elsewhere), a checkpoint brings it back, lava kills
# straight to dead, and everyone down reloads the checkpoint for all. About 3 minutes.
#   MDK2_N=3 tools/qa_revive.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_revive.sh needs MDK2_N >= 3"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
wait_log() {   # wait_log <instance> <regex> <seconds>
    local i; for i in $(seq 1 $(( $3 * 4 ))); do has "$1" "$2" && return 0; sleep 0.25; done; return 1; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
# ask <instance> <lua expression> <regex>: log the expression's value there and match it
ask() { local tag="ask$RANDOM"; lua "$1" "coop_log(\"$tag \" .. tostring($2))"
        wait_log "$1" "^lua: $tag " 3 && grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //" | grep -qE "$3"; }
hit()  { lua "$1" "mdkDealDamage(nil, mdkGetPlayerGob(), ${3:-999}, ${2:-DAMAGE_BADGUY}, -1)"; }

mdk_boot
C=2; O=3                                    # the client that goes down, and an onlooker
sleep 3

# 1. down
hit $C
check "a lethal hit puts the client down"               wait_log $C "life: alive -> downed \(lethal hit\)" 3
check "the host sees it down"                           ask 1 "coop_life(1)" "^1$"
check "the other client sees it down"                   ask $O "coop_life(1)" "^1$"
sleep 1.5
lua 1 "coop_animdump(coop.puppets[1])"; sleep 0.5
check "its puppet lies down in the host's game"         bash -c "tail -5 '$(mdk_log 1)' | grep -qE '^animdump coop_p2 .*\(anim 17\)'"
lua $O "coop_animdump(coop.puppets[1])"; sleep 0.5
check "  and in the other client's game"                bash -c "tail -5 '$(mdk_log $O)' | grep -qE '^animdump coop_p2 .*\(anim 17\)'"
before=$(grep -cE '^down: ' "$(mdk_log $C)")
hit $C DAMAGE_BADGUY 30; sleep 0.5
check "more damage while down is ignored"               ask $C "coop_hp(mdkGetPlayerGob())" "^[1-9]"
lua $C "coop_hud_dump()"; sleep 0.5
check "the downed client's HUD counts down"             wait_log $C "hud status: You're down: [0-9]+ s" 2
lua 1 "coop_hud_dump()"; sleep 0.5
check "the host's HUD offers the revive"                wait_log 1 "hud status: .* is down \| Hold R to revive" 2

# 2. revive: the host holds R (everyone starts at the same spot, 3 units apart)
lua 1 "coop_press(19, 3600)"; sleep 1.5
lua $C "coop_hud_dump()"; sleep 0.3
check "the downed client sees who is reviving it"       wait_log $C "hud status: .* \| .* is reviving you" 2
check "holding R for 3 s revives the client"            wait_log $C "life: downed -> alive \(revived by player 1\)" 5
sleep 1
check "  with half its health"                          ask $C "coop_hp(mdkGetPlayerGob())" "^5[0-9]$"
check "  and the others see it alive"                   ask $O "coop_life(1)" "^0$"
lua $C "coop_press(17, 600)"; sleep 1
check "  and it can walk again"                         bash -c "grep -q 'revive action: up' '$(mdk_log $C)'"

# 3. down again, bleed out, spectate, back at a checkpoint
hit $C
check "down again"                                      wait_log $C "life: alive -> downed" 3
check "after 30 s it bleeds out"                        wait_log $C "life: downed -> dead \(bled out\)" 35
check "the host knows where its body is"                wait_log 1 "revive: player 2 died at" 3
check "then it spectates a teammate"                    wait_log $C "revive: spectating player" 5
check "  the others see it spectating"                  ask $O "coop_life(1)" "^3$"
watching() { grep -E '^hud status: Dead: watching ' "$(mdk_log $C)" | tail -1 | sed -E 's/^hud status: Dead: watching ([^|]*) \|.*/\1/'; }
lua $C "coop_hud_dump()"; sleep 0.5
check "  its HUD says whom it watches"                  wait_log $C "hud status: Dead: watching " 2
first=$(watching)
lua $C "coop_press(256, 150)"; sleep 0.6; lua $C "coop_hud_dump()"; sleep 0.5
check "  a click switches to the other teammate ($first -> $(watching))" bash -c "test -n '$first' && test '$first' != '$(watching)'"
if has 1 "zone is clear: back"; then
    check "its zone was clear: it came back"            wait_log $C "life: (dead|spectating) -> alive \(our zone is clear\)|life: dead -> alive" 3
else
    lua 1 "coop_revive_checkpoint()"
    check "a checkpoint brings it back"                 wait_log $C "life: dead -> alive \(a checkpoint was reached\)" 3
fi
sleep 1
check "  next to a teammate, visible again"             ask $O "coop_life(1)" "^0$"

# a checkpoint also stands up whoever is down, where they lie
hit $C
check "down once more"                                  wait_log $C "life: alive -> downed" 3
lua 1 "coop_revive_checkpoint()"
check "  a checkpoint stands it up"                     bash -c "for i in \$(seq 1 12); do test \$(grep -c 'life: downed -> alive (a checkpoint was reached)' '$(mdk_log $C)') -ge 1 && exit 0; sleep 0.25; done; exit 1"

# 4. lava: no body to reach, straight to dead; the checkpoint brings it back
hit $O DAMAGE_LAVAFALL 500
check "lava kills straight to dead"                     wait_log $O "life: alive -> dead \(no body to reach\)" 3
lua 1 "coop_revive_checkpoint()"
check "  and a checkpoint brings it back"               wait_log $O "life: dead -> alive" 4

# 5. everyone down: the host reloads the checkpoint for all
loads=$(count 1 "^level 1 checkpoint 2 started")
for i in $(mdk_seq); do hit "$i"; done
check "everyone down reloads the checkpoint"            wait_log 1 "revive: everyone is down: reloading the checkpoint" 6
check "  the host loads it again"                       bash -c "for i in \$(seq 1 40); do test \$(grep -c '^level 1 checkpoint 2 started' '$(mdk_log 1)') -gt $loads && exit 0; sleep 0.5; done; exit 1"
sleep 8
for i in $(mdk_seq); do check "  game $i stands up in the new start" ask "$i" "coop_life(coop_slot())" "^0$"; done

all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
check "no game crashed"                                 all_alive
check "no Lua errors"                                   bash -c "! grep -h 'LUA ERROR' $(for i in $(mdk_seq); do printf "'%s' " "$(mdk_log $i)"; done)"
mdk_kill

printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
