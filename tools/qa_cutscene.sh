#!/usr/bin/env bash
# Two real games through the opening sequences (1-9 lounge cutscene, 1-12 skydive): both start the movie on the
# host's go, the skip vote needs a majority (Backspace), the minigame cannot be skipped, and the level
# change waits until both players have seen the end. Needs isolated copies (tools/qa_all.py configures them).
#   tools/qa_cutscene.sh      (about 3 minutes)
#   tools/qa_cutscene.sh --early-go   only the early-GO playback clock regression
#   MUTANT=nobarrier|onevote|upright|sideways|escskip|earlygo tools/qa_cutscene.sh   a deliberately broken coop.lua: passes only if the check
#                             that guards the broken part fails
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
clock_only=0
case "$*" in
    "") start="1 9" ;;
    --early-go) start="1 2"; clock_only=1 ;;
    *) echo 'usage: qa_cutscene.sh [--early-go]' >&2; exit 2 ;;
esac
MDK2_BACKGROUND=1 MDK2_START="$start" "$here/install.sh" >/dev/null || exit 1
# mutants: the installed coop.lua with one protection removed
want=""
if [ -n "${MUTANT:-}" ]; then
    case "$MUTANT" in
        nobarrier) from='if (coop.mv ~= m or m.skipping or not coop.MovieOthers()) then'; to='if (1) then'
                   want="host does not load the next level alone" ;;
        onevote)   from='local needed = floor(parts / 2) + 1'; to='local needed = 1'
                   want="one of two votes does not skip" ;;
        upright)   from='if (not qw) then qw, qx, qy, qz = 1, 0, 0, 0 end'; to='qw, qx, qy, qz = 1, 0, 0, 0'
                   want="client's ghost Kurt faces like its own" ;;
        sideways)  from='if (vz < -0.9999) then coop_set_quat(g, 0, 1, 0, 0) else coop_set_quat(g, 1 + vz, -vy, vx, 0) end'
                   to='mdkGobPointToXYZ(g, vx, vy, vz)'
                   want="the missile ghost flies nose first" ;;
        escskip)   from='if (code and ago < 1000) then'; to='if (nil) then'
                   want="Esc on the client is a vote, not its own skip" ;;
        earlygo)   from='coop.MovieGo(m)     -- an early GO needs the same playback clock as one received while waiting'
                   to='m.state = "play"'
                   want="early host GO starts the playback clock" ;;
        *) echo "unknown MUTANT $MUTANT"; exit 2 ;;
    esac
    for i in $(mdk_seq); do
        python3 - "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.lua" "$from" "$to" <<'PY'
import sys
path, old, new = sys.argv[1:4]
data = open(path, newline='').read()
if old not in data:
    sys.exit('mutant: text not found')
open(path, 'w', newline='').write(data.replace(old, new))
PY
    done
    f="$(mdk_wsl "$(mdk_dir 1)")/coop/coop.lua"
    grep -qF "$to" "$f" || { echo "mutant $MUTANT did not apply"; exit 2; }
fi
out="$here/../local/qa-cutscene/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
pass=0; fail=0; results=()
cleanup() {
    local i
    for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log" 2>/dev/null || true; done
    mdk_kill
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
check() {
    local name=$1; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
mark() { eval "off$1=\$(wc -l < \"\$(mdk_log $1)\")"; }
mark_all() { mark 1; mark 2; }
# wait_since <game> <pattern> <seconds>: a log line since the last mark_all
wait_since() {
    local i=$1 pattern=$2 deadline=$((SECONDS + $3)) offset
    eval "offset=\$off$i"
    while [ "$SECONDS" -le "$deadline" ]; do
        tail -n +"$((offset + 1))" "$(mdk_log "$i")" | grep -E "$pattern" >/dev/null && return 0
        sleep 0.2
    done
    return 1
}
not_since() {   # not_since <game> <pattern>: no such line since the last mark_all
    local offset; eval "offset=\$off$1"
    ! tail -n +"$((offset + 1))" "$(mdk_log "$1")" | grep -E "$2"
}
vote() { mdk_press "$(mdk_pid "$1")" 14 1 100 200; }
# the line of a log since the mark, for the report
off1=0; off2=0         # the games' logs start empty
early_go_checks() {
# Force GO to arrive before this client's next movie is created, the race seen at 3-10/3-11.
# Use the real event handler and StartMovie path; the timer must advance without arithmetic errors.
mark_all
lua 2 'coop.MovieEvent("", 0, coop.mvcount + 1, MV_GO, 0, 0, 0, 0, 0, 0)
       coop.remote_run = 1
       StartMovie(mdkGetCameraGob(), { { Wait, { mdkGetCameraGob(), 3 } } })
       coop.remote_run = nil'
check "early host GO is exercised" wait_since 2 'the host had already said go' 5
sleep 0.5
lua 2 'local m = coop.mv
       if (m and m.state == "play" and m.go_at and m.tick_at and m.played > 0) then
           coop_log("T early GO clock advances") end'
check "early host GO starts the playback clock" wait_since 2 '^lua: T early GO clock advances' 2
check "early GO movie ends on the client" wait_since 2 'movie [0-9]+: over' 10
check "early GO runs without Lua errors" not_since 2 'LUA ERROR'
}
report() {
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs: $out"
if [ -n "$want" ]; then
    printf '%s\n' "${results[@]}" | grep -qF "FAIL  $want" && { echo "mutant $MUTANT caught by: $want"; exit 0; }
    echo "mutant $MUTANT NOT caught by: $want"; exit 1
fi
[ "$fail" = 0 ]
}
mdk_boot
if [ "$clock_only" = 1 ]; then early_go_checks; report; exit $?; fi
# ---- 1-9: the lounge cutscene
check "host holds the movie for the client, then goes" wait_since 1 'movie 1: go' 60
check "client plays on the host's go" wait_since 2 'movie 1: (go|the host had already said go)' 60
# the cutscene box (bottom right) stays out of the picture until a player presses a key other than the skip key
sleep 2
mark_all
lua 2 'coop_hud_dump()'; sleep 1
check "no cutscene box while just watching" not_since 2 'hud moviebox: Cutscene'
mdk_press "$(mdk_pid 2)" 23 1 100 200                      # I: no use in a cutscene
lua 2 'coop_hud_dump()'
check "a key press shows the time left and the skip key" \
    wait_since 2 'hud moviebox: Cutscene  [0-9]+:[0-9][0-9] left \| Backspace: vote to skip \| [[:space:]]*$' 5
mdk_shot "$(mdk_pid 2)" "$out/cutscene_box.png" || true     # evidence: the box at the bottom right
vote 1
check "host's vote is counted" wait_since 1 'movie 1: my skip vote 1' 5
sleep 3
check "one of two votes does not skip" not_since 1 'skipped by vote'
# a vote has no time limit: still counted well past the old 15 s
sleep 15
lua 2 'coop_hud_dump()'; lua 1 'coop_hud_dump()'
# with a vote open the box shows without a key press, and a measured movie (coop.mvlen) counts down the
# first time it plays
check "the vote stays until the movie ends (client's box)" \
    wait_since 2 'hud moviebox: Cutscene  [0-9]+:[0-9][0-9] left \| Backspace: vote to skip \| Skip votes: 1 of 2' 5
# Options > Co-op > Interface > Cutscene timer off: the box keeps the skip key and the votes, not the time
lua 2 'coop_set("hud", "movietimer", 0) coop.movietimer = nil coop_hud_dump()'
check "cutscene timer switched off: no time in the box" \
    wait_since 2 'hud moviebox:  \| Backspace: vote to skip \| Skip votes: 1 of 2' 5
lua 2 'coop_set("hud", "movietimer", 1) coop.movietimer = 1'
check "the host's box says it voted and how to take it back" \
    wait_since 1 'hud moviebox: Cutscene  [0-9]+:[0-9][0-9] left \| You voted to skip  \(Backspace again: take it back\) \| Skip votes: 1 of 2' 5
mark_all
# the second vote is the game's own skip key, Esc: the engine stops the movie script itself, which used to end the
# movie for that player alone
mdk_press "$(mdk_pid 2)" 1 1 100 200
check "Esc on the client is a vote, not its own skip" wait_since 2 'the game.s skip \(key 1\) counts as a vote' 5
check "second vote skips on the host" wait_since 1 'movie 1: skipped by vote' 8
check "second vote skips on the client" wait_since 2 'movie 1: skipped by vote' 8
# the lounge scene's Done starts the next movie (the landing in the corridor): it is kept together too
check "the next movie starts together" wait_since 2 'movie 2: go' 30
mark_all
vote 1; sleep 1; vote 2
check "that movie is skipped by both" wait_since 1 'movie 2: skipped by vote' 8
# ---- 1-12: the skydive
check "host loads the skydive" wait_since 1 '^level 1 checkpoint 12 started' 40
check "client loads the skydive" wait_since 2 '^level 1 checkpoint 12 started' 40
check "skydive movie goes on both" wait_since 2 'movie 1: (go|the host had already said go)' 40
mark_all
vote 1; sleep 1; vote 2
check "skipping runs up to the minigame" wait_since 1 'movie 1: skipped by vote' 8
inmini() { for _ in $(seq 1 30); do lua "$1" 'if Save.inminigame then coop_log("T mini".."game") end'; sleep 1
           tail -n +"$((${2}+1))" "$(mdk_log $3)" | grep -q "lua: T minigame" && return 0; done; return 1; }
check "host is in the minigame" inmini 1 "$off1" 1
check "client is in the minigame" inmini 2 "$off2" 2
# nobody is hit while the checks run (a hit starts the game's death camera and the skydive over): the lasers and the
# homing missile still fly, but never hit
nohit='Level.MissileOnUpdate = function(gob)
  if (gob.laser and not gob.exploding and kurtgame.ready) then local dt = chGetDeltaT()
    gob.z = gob.z + kurtgame.missilespeed * dt; gob.x = gob.x + gob.vx * dt; gob.y = gob.y + gob.vy * dt
    mdkGobSetPositionXYZ(gob, gob.x, gob.y, gob.z) end end'
lua 1 "$nohit"; lua 2 "$nohit"
# the skydive lands by itself when its timer runs out (ktgame_timer's custom key): the client lands only when the
# test says, so the end barrier can be held for longer than the old 20 s
lua 2 'T_ock = ktgame_timer.OnCustomKey
       ktgame_timer.OnCustomKey = function(g, part, key) if (T_land) then T_ock(g, part, key) end end'
check "host sees the client fall beside it" wait_since 1 'minigame: player 2 plays beside you \(level 1\)' 10
check "client sees the host fall beside it" wait_since 2 'minigame: player 1 plays beside you \(level 1\)' 10
# the ghost is the skydiving Kurt as the game tilts it, not upright (|dot| of the two orientations near 1)
facing() {
    lua 2 'local g = coop.fallghost[0]; local _,_,_,a,b,c,d = coop_pose(ktgame_kurt); local _,_,_,e,f,h,i = coop_pose(g)
           coop_log(format("T facing %.3f", abs(a*e + b*f + c*h + d*i)))'
    sleep 0.5
    local v; v=$(tail -n +"$((off2 + 1))" "$(mdk_log 2)" | grep -oE 'T facing [0-9.]+' | tail -1 | cut -d' ' -f3)
    echo "dot $v"; awk -v v="${v:-0}" 'BEGIN { exit !(v > 0.99) }'
}
check "client's ghost Kurt faces like its own" facing
# the homing missile the host's game fires at its Kurt flies in the client's sky too (every few seconds)
check "client sees the host's homing missile" wait_since 2 "minigame: player 1's missile flies beside you" 20
# its nose (the model's +z, turned by the ghost's orientation) points the way it flies; a missile sideways was seen
nose() {
    local i v
    for i in $(seq 1 20); do
        lua 2 'local s, p = next(coop.fallmis, nil)
               if (p and p.gob and coop_alive(p.gob) and (p.vx ~= 0 or p.vy ~= 0 or p.vz ~= 0)) then
                 local _,_,_,w,x,y,z = coop_pose(p.gob); local n = sqrt(p.vx*p.vx + p.vy*p.vy + p.vz*p.vz)
                 coop_log(format("T nose %.3f", (2*(x*z + w*y)*p.vx + 2*(y*z - w*x)*p.vy + (1 - 2*(x*x + y*y))*p.vz) / n)) end'
        v=$(tail -n +"$((off2 + 1))" "$(mdk_log 2)" | grep -oE 'T nose -?[0-9.]+' | tail -1 | cut -d' ' -f3)
        [ -n "$v" ] && break
        sleep 0.5
    done
    echo "nose . heading = $v"; awk -v v="${v:-0}" 'BEGIN { exit !(v > 0.9) }'
}
check "the missile ghost flies nose first" nose
# Options > Co-op > Interface > Friends in minigames off: no ghosts drawn; on again: back
ghosts() { lua 2 'local n, s = 0, next(coop.fallghost, nil) while (s) do n = n + 1 s = next(coop.fallghost, s) end
                  coop_log(format("T ghosts %d", n))'; sleep 0.5
           tail -n +"$((off2 + 1))" "$(mdk_log 2)" | grep -oE 'T ghosts [0-9]+' | tail -1 | cut -d' ' -f3; }
lua 2 'coop_set("features", "ghosts", 0)'; sleep 1
n=$(ghosts); check "friends in minigames switched off: no ghost (got $n)" test "$n" = 0
lua 2 'coop_set("features", "ghosts", 1)'; sleep 1
n=$(ghosts); check "switched on again: the ghost is back (got $n)" test "$n" = 1
lua 2 'ktgame_kurt.x = ktgame_kurt.x + 1.6'; sleep 0.8       # the client steers aside: its ghost leaves the host's Kurt
mdk_shot "$(mdk_pid 1)" "$out/skydive_host.png" || true      # evidence: the friend's ghost in the sky
mark_all
vote 1
check "the minigame cannot be skipped" not_since 1 'my skip vote'
# the host lands first; the level change waits for the client
lua 1 'ktgame_timer.OnCustomKey(ktgame_timer, 0, CUSTKEY_USER1)'
check "host reaches the end and waits" wait_since 1 'movie 1: ended here, waiting for the others' 60
sleep 25                                                    # past the 20 s the wait used to give up after
check "host does not load the next level alone" not_since 1 '^level 1 checkpoint 1 started'
lua 2 'T_land = 1 ktgame_timer.OnCustomKey(ktgame_timer, 0, CUSTKEY_USER1)'
check "client reaches the end" wait_since 2 'movie 1: ended here, waiting for the others' 60
check "host finishes once the client has" wait_since 1 'movie 1: everyone has seen the end' 10
check "client finishes with the host" wait_since 2 'movie 1: everyone has seen the end' 10
check "host loads 1-1" wait_since 1 '^level 1 checkpoint 1 started' 40
check "client loads 1-1" wait_since 2 '^level 1 checkpoint 1 started' 40
# ---- a movie the host's trigger starts in a level: the client plays it too. A short stand-in for the boss
# movie (level 1 has none) is defined in both games and hooked like the game's own.
mark_all
sleep 20                                                    # past the load window: this is a trigger, not a load movie
stand_in='Level.PlayBossMovie = function() StartMovie(mdkGetCameraGob(), { { Wait, { mdkGetCameraGob(), 3 } } }) end; coop.HookMovies()'
lua 1 "$stand_in"; lua 2 "$stand_in"
lua 1 'Level.PlayBossMovie()'
check "host starts the trigger movie and waits briefly" wait_since 1 'movie 1: waiting for the others' 8
check "client is told to play it" wait_since 2 'movie Level.PlayBossMovie: the host started it' 10
check "trigger movie goes on the host" wait_since 1 'movie 1: go' 10
check "trigger movie goes on the client" wait_since 2 'movie 1: go' 10
check "trigger movie ends on the host" wait_since 1 'movie 1: over' 20
check "trigger movie ends on the client" wait_since 2 'movie 1: over' 20
# played to its end, so its length is known; the next time the banner counts it down
check "host learns the movie's length" wait_since 1 'movie 1: length [^ ]+ (2[5-9][0-9][0-9]|3[0-4][0-9][0-9])\b' 1
mark_all
sleep 16                                                    # a game plays the same trigger movie once in 15 s
lua 1 'Level.PlayBossMovie()'
check "second showing goes" wait_since 1 'movie 2: go' 10
mdk_press "$(mdk_pid 1)" 23 1 100 100
lua 1 'coop_hud_dump()'
check "the box counts down the time the movie has left" wait_since 1 'hud moviebox: Cutscene  0:0[1-3] left' 5
check "second showing ends" wait_since 2 'movie 2: over' 20
# a trigger on the client's own side is the client's alone: no announcement, no waiting
mark_all
lua 2 'Level.PlayBossMovie()'
sleep 6
check "a client's own trigger movie is not kept together" not_since 1 'movie'
early_go_checks
for i in $(mdk_seq); do check "game $i still running" mdk_alive "$(mdk_pid "$i")"; done
check "no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
report
