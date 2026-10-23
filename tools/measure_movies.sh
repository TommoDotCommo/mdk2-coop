#!/usr/bin/env bash
# Measure the cutscenes the co-op keeps together, for the countdown on the cutscene banner (coop.mvlen in
# lua/coop_dev.lua). Two real games watch each movie to its end, unskipped; a minigame inside a movie is won at
# once (its time does not count). Each length is the game's own "movie N: length <key> <ms>" line.
#   tools/measure_movies.sh                   every movie checkpoint, then the trigger movies (about 1.5 hours)
#   tools/measure_movies.sh 2-12 4-15         only these movie checkpoints
#   tools/measure_movies.sh t:4               only the trigger movies of level 4
# Writes local/movies/<date-time>/lengths.txt ("key ms" per line, host's measure) and coop.mvlen.lua (the table).
# Needs two isolated game copies.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
out="$here/../local/movies/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
trap 'mdk_kill' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# the checkpoints where a level's Level.Init plays a movie (level scripts, "cp == N")
loads="1-9 1-12 1-10 2-12 2-13 2-14 3-12 3-14 4-12 4-13 4-15 5-11 5-12 5-13 6-8 6-9 7-11 7-12 8-8 8-9 8-10
       9-13 9-14 10-7 10-8 10-9 10-10 11-2 11-3 11-4 12-2 12-3 12-4"
# the movies a trigger starts (coop.moviehooks), per level: called on the host, at each play checkpoint of the
# level in turn until one plays (they need their room's gobs)
triggers="1:l1_r6.StartCutScene 1:l1_r8.StartCutSceneOpening 1:Level.PlayDeathMovie 2:l2_r11.StartCutSceneBoss
          3:l3_bathroom03.StartCutScene 3:l3_w_hub00.StartCutScene 3:Level.PlayDeathMovie 4:Level.BridgeMovie
          4:Level.PlayDeathMovie 5:Level.PlayDeathMovie 6:Level.PlayDeathMovie 7:l7r5.SendInTheTroops
          11:Level.KillZizzy 12:Level.KillZizzy"
if [ $# -gt 0 ]; then
    loads=""; want_t=""
    for a in "$@"; do case $a in t:*) want_t="$want_t ${a#t:}" ;; *) loads="$loads $a" ;; esac; done
    t=""; for x in $triggers; do for l in $want_t; do [ "${x%%:*}" = "$l" ] && t="$t $x"; done; done
    triggers="$t"
fi

lua() { mdk_cmd "$(mdk_pid "$1")" "$2" 2>/dev/null; }
lines() { wc -l < "$(mdk_log "$1")"; }
since() { tail -n +"$(($2 + 1))" "$(mdk_log "$1")"; }     # since <game> <line count>
# a minigame inside a movie: win it once it runs (each game plays its own). Its start resets the result, so a
# win before that is lost: win whenever it runs undecided
win='if (Save and Save.inminigame) then local w
  if (coop.level == 1 and kurtgame.ready and kurtgame.result == 0 and not ktgame_kurt.dead) then
    ktgame_timer.OnCustomKey(ktgame_timer, 0, CUSTKEY_USER1); w = 1
  elseif (coop.level == 2 and torpedogame.ready and torpgob.result == 0 and not torpgob.dead) then
    tpgame_timer.OnCustomKey(tpgame_timer, 0, CUSTKEY_USER1); w = 1
  elseif (coop.level == 5 and podgob.result == 0 and not podgob.dead) then
    pd_gametimer.OnAnimLoop(pd_gametimer, ANIM_ACTION00); w = 1
  elseif (coop.level == 9 and jdgob.result == 0 and not jdgob.dead) then
    jdgame_timer.OnAnimLoop(jdgame_timer, ANIM_ACTION00); w = 1 end
  if (w) then coop_log("T won the minigame") end end'
# watch until no movie has run on the host for 25 s (a movie's Done may load a level that plays the next one),
# or 10 minutes
watch() {
    local deadline=$((SECONDS + 600)) quiet=$SECONDS state
    while [ "$SECONDS" -le "$deadline" ]; do
        lua 1 "$win"; lua 2 "$win"
        lua 1 'if (coop.mv) then coop_log("T mv on") else coop_log("T mv off") end'
        sleep 2
        state=$(tail -n 40 "$(mdk_log 1)" | grep -oE 'T mv (on|off)' | tail -1)
        [ "$state" = "T mv on" ] && quiet=$SECONDS
        [ $((SECONDS - quiet)) -ge 25 ] && return 0
    done
    echo "  still in a movie after 10 minutes"
}
report() {   # report <what> <host line count before>: the lengths measured since
    local got; got=$(since 1 "$2" | grep -oE 'movie [0-9]+: length [^ ]+ [0-9]+' | awk '{print $4, $5}')
    if [ -n "$got" ]; then echo "$got" >> "$out/lengths.txt"; echo "$got" | sed "s/^/  $1: /"
    else echo "  $1: nothing measured"; fi
    since 1 "$2" | grep -h 'LUA ERROR' | sort -u | sed "s/^/  $1 (host): /"
}
booted=""
boot_at() {
    [ -n "$booted" ] && save_logs
    booted=1
    MDK2_BACKGROUND=1 MDK2_START="$1 $2" "$here/install.sh" >/dev/null || return 1
    mdk_boot
}
for i in 1 2; do : > "$out/game$i.log"; done
save_logs() { for i in 1 2; do cat "$(mdk_log "$i")" >> "$out/game$i.log" 2>/dev/null; done; }

for lc in $loads; do
    l=${lc%-*}; c=${lc#*-}
    echo "== $lc"
    boot_at "$l" "$c" || { echo "  install failed"; continue; }
    watch
    report "$lc" 0
done

level_cps() { tr ' ' '\n' < "$here/checkpoints.txt" | grep -E "^$1-[0-9]+$" | cut -d- -f2; }
cur=""
for t in $triggers; do
    l=${t%%:*}; f=${t#*:}
    echo "== $f (level $l)"
    done_t=""
    for c in $(level_cps "$l"); do
        if [ "$cur" != "$l-$c" ]; then
            if [ "${cur%-*}" = "$l" ] && mdk_alive "$(mdk_pid 1)"; then
                n2=$(lines 2)
                lua 1 "mdkNewGame($l, $c)"
                for _ in $(seq 1 60); do since 2 "$n2" | grep -q "^level $l checkpoint $c started" && break; sleep 1; done
            else
                boot_at "$l" "$c" || continue
            fi
            cur="$l-$c"
            sleep 18                      # past the load window: this is a trigger, not a load movie
            watch                         # the checkpoint's own movie, if it has one
        fi
        n1=$(lines 1)
        arg=${f%.*}; [ "$arg" = Level ] && arg=""        # a room's function gets its room, as the game calls it
        lua 1 "if ($f) then $f($arg) else coop_log('T no such function') end"
        sleep 6
        if since 1 "$n1" | grep -q 'movie [0-9]*: waiting for the others'; then
            watch
            report "$f" "$n1"
            since 1 "$n1" | grep -q 'movie [0-9]*: length ' && { done_t=1; break; }
        fi
        # nothing played here, or it broke off: the next checkpoint (a fresh load, so its gobs are right)
        echo "  not at $l-$c"
        cur="$l-x"
    done
    [ -n "$done_t" ] || echo "  $f: nothing measured"
done
[ -n "$booted" ] && save_logs

# the table for coop.mvlen: one length per key (the longest, if a movie was measured twice)
sort -k1,1 -k2,2nr "$out/lengths.txt" 2>/dev/null | awk '!seen[$1]++' > "$out/lengths.sorted"
{ while read -r k ms; do printf '    ["%s"] = %s,\n' "$k" "$ms"; done < "$out/lengths.sorted"; } > "$out/coop.mvlen.lua"
echo "$(wc -l < "$out/lengths.sorted") movies measured; table: $out/coop.mvlen.lua"
