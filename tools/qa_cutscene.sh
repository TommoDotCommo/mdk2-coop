#!/usr/bin/env bash
# Two real games through the opening sequences (1-9 lounge cutscene, 1-12 skydive): both start the movie on the
# host's go, the skip vote needs a majority (Backspace), the minigame cannot be skipped, and the level
# change waits until both players have seen the end. Needs a lab lease (tools/qa_all.py gives one).
#   tools/qa_cutscene.sh      (about 8 minutes)
#   MUTANT=nobarrier|onevote tools/qa_cutscene.sh   a deliberately broken coop.lua: passes only if the check
#                             that guards the broken part fails
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 9" "$here/install.sh" >/dev/null || exit 1
# mutants: the installed coop.lua with one protection removed
want=""
if [ -n "${MUTANT:-}" ]; then
    case "$MUTANT" in
        nobarrier) from='if (coop.mv ~= m or m.skipping or not coop.MovieOthers()) then'; to='if (1) then'
                   want="host does not load the next level alone" ;;
        onevote)   from='local needed = floor(parts / 2) + 1'; to='local needed = 1'
                   want="one of two votes does not skip" ;;
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
mdk_boot
# ---- 1-9: the lounge cutscene
check "host holds the movie for the client, then goes" wait_since 1 'movie 1: go' 60
check "client plays on the host's go" wait_since 2 'movie 1: (go|the host had already said go)' 60
vote 1
check "host's vote is counted" wait_since 1 'movie 1: my skip vote 1' 5
sleep 3
check "one of two votes does not skip" not_since 1 'skipped by vote'
mark_all
vote 2
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
check "host sees the client fall beside it" wait_since 1 'minigame: player 2 plays beside you \(level 1\)' 10
check "client sees the host fall beside it" wait_since 2 'minigame: player 1 plays beside you \(level 1\)' 10
lua 2 'ktgame_kurt.x = ktgame_kurt.x + 1.6'; sleep 0.8       # the client steers aside: its ghost leaves the host's Kurt
mdk_shot "$(mdk_pid 1)" "$out/skydive_host.png" || true      # evidence: the friend's ghost in the sky
mark_all
vote 1
check "the minigame cannot be skipped" not_since 1 'my skip vote'
# the host lands first; the level change waits for the client
lua 1 'ktgame_timer.OnCustomKey(ktgame_timer, 0, CUSTKEY_USER1)'
check "host reaches the end and waits" wait_since 1 'movie 1: ended here, waiting for the others' 60
sleep 6
check "host does not load the next level alone" not_since 1 '^level 1 checkpoint 1 started'
lua 2 'ktgame_timer.OnCustomKey(ktgame_timer, 0, CUSTKEY_USER1)'
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
# a trigger on the client's own side is the client's alone: no announcement, no waiting
mark_all
lua 2 'Level.PlayBossMovie()'
sleep 6
check "a client's own trigger movie is not kept together" not_since 1 'movie'
for i in $(mdk_seq); do check "game $i still running" mdk_alive "$(mdk_pid "$i")"; done
check "no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs: $out"
if [ -n "$want" ]; then
    printf '%s\n' "${results[@]}" | grep -qF "FAIL  $want" && { echo "mutant $MUTANT caught by: $want"; exit 0; }
    echo "mutant $MUTANT NOT caught by: $want"; exit 1
fi
[ "$fail" = 0 ]
