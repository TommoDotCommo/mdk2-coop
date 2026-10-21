#!/usr/bin/env bash
# Checkpoint sweep: visit every checkpoint (the debug menu's list, same as F6) with MDK2_N
# instances (default 2: host and one client), do a few things at each and check every game.
# Restarts all after a crash and carries on from the next checkpoint.
#   tools/sweep.sh                 every checkpoint
#   tools/sweep.sh 2-1 3-4         from 2-1 up to and including 3-4
#   MDK2_N=4 tools/sweep.sh        host and three clients (game copies: tools/mdk.sh, tools/copies.sh)
# Results: local/sweeps/<date-time>/results.md, plus per checkpoint every game's log slice,
# screenshot and state dump with statecheck.py's report (state.txt) and any crash dumps.
# Takes roughly 30 s per checkpoint (more with more clients).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"

ALL="1-1 1-2 1-3 1-4 1-5 1-6 1-7 1-8 2-1 2-2 2-3 2-4 2-5 2-6 2-7 2-8 2-9 2-10
3-1 3-2 3-3 3-4 3-5 3-6 3-16 3-7 3-8 3-9 3-10 3-11 4-1 4-2 4-3 4-4 4-5 4-6 4-7 4-8 4-9 4-10 4-11 4-14
5-1 5-2 5-3 5-4 5-5 5-6 5-7 5-8 5-9 5-10 6-1 6-2 6-3 6-4 6-5 6-6 6-7 7-1 7-2 7-3 7-4 7-5 7-6 7-7 7-8
8-1 8-2 8-3 8-4 8-5 8-6 8-7 9-1 9-3 9-4 9-5 9-6 9-7 9-8 9-11 9-12 10-1 10-2 10-3 10-4 10-5 10-6 10-11
11-1 11-6 11-11 12-1 12-6 12-11"

from="${1:-1-1}"; to="${2:-12-11}"
list=""; on=0
for c in $ALL; do
    [ "$c" = "$from" ] && on=1
    [ $on = 1 ] && list="$list $c"
    [ "$c" = "$to" ] && break
done
[ -n "$list" ] || { echo "no checkpoints between $from and $to"; exit 1; }

out="$here/../local/sweeps/$(date +%Y%m%d-%H%M%S)$([ "$MDK2_N" -gt 2 ] && echo "-n$MDK2_N")"
mkdir -p "$out"
results="$out/results.md"
{
    echo "# Checkpoint sweep $(date '+%Y-%m-%d %H:%M'), $MDK2_N games"
    echo
    echo "Per-client columns list clients 2..$MDK2_N separated by /."
    echo
    echo "| checkpoint | followed | puppets host/clients | enemies host/bound | orphans | lua errors | enemies hit | hp mismatches | state fail/warn | alive | notes |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|"
} > "$results"

name() { case $1 in 1) echo host;; 2) echo client;; *) echo "client$1";; esac; }   # as mdk_state names them
lines() { { wc -l < "$1"; } 2>/dev/null || echo 0; }
since() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; }     # since <log> <line count before>
wait_for() {   # wait_for <log> <offset> <regex> <seconds>
    local i
    # grep reads to the end (no -q): with pipefail, tail killed by SIGPIPE would count as no match
    for i in $(seq 1 "$4"); do since "$1" "$2" | grep -E "$3" >/dev/null && return 0; sleep 1; done
    return 1
}
alive_all() { local p; [ -s "$MDK2_PIDS" ] || return 1; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
collect_dumps() {   # move new crash dumps into the checkpoint's folder
    local i d f
    for i in $(mdk_seq); do
        d="$(mdk_wsl "$(mdk_dir "$i")")"
        for f in "$d"/mdk2hd_crash_*.mdmp; do
            [ -e "$f" ] && mv "$f" "$1/$(name "$i")-$(basename "$f")"
        done
    done
}
start_at() {   # start_at <checkpoint>: fresh set with the host autostarting there
    MDK2_BACKGROUND=1 MDK2_START="${1%-*} ${1#*-}" "$here/install.sh" >/dev/null
    mdk_boot
}
join_by() { local IFS="$1"; shift; echo "$*"; }

collect_dumps "$out"        # old dumps out of the way
first=1
declare -A off
for c in $list; do
    l="${c%-*}"; cp="${c#*-}"
    dir="$out/$c"; mkdir -p "$dir"
    notes=""
    for i in $(mdk_seq); do off[$i]=$(lines "$(mdk_log "$i")"); done

    if [ $first = 1 ] || ! alive_all; then
        start_at "$c"
        for i in $(mdk_seq); do off[$i]=0; done
    else
        mdk_key "$(mdk_pid 1)" 64                         # F6: next checkpoint
    fi
    first=0
    H=$(mdk_pid 1)

    followed=yes
    for i in $(mdk_seq); do
        wait_for "$(mdk_log "$i")" "${off[$i]}" "^level $l checkpoint $cp started" 40 && continue
        followed=no
        notes="$notes $(name "$i") at: $(since "$(mdk_log "$i")" "${off[$i]}" | grep -E '^level' | tail -1 | tr -d '\r');"
    done
    sleep 6                                               # let the level settle, enemies announce

    # actions: everyone walks and fires, every client hits something through its gun path, then
    # everyone strafes (alternate directions)
    mdk_key "$H" 17 1 1500; mdk_click "$H" 1200 1
    for i in $(seq 2 "$MDK2_N"); do
        P=$(mdk_pid "$i")
        mdk_key "$P" 17 1 1000; mdk_click "$P" 1200 1
        mdk_key "$P" 59                                   # client F1: hits coop_dog1 or the nearest enemy
    done
    for i in $(mdk_seq); do mdk_key "$(mdk_pid "$i")" $([ $((i % 2)) = 1 ] && echo 32 || echo 30) 1 800; done
    sleep 3
    for i in $(mdk_seq); do mdk_shot "$(mdk_pid "$i")" "$dir/$(name "$i").png" >/dev/null 2>&1; done
    mdk_state "$c" "$dir" > "$dir/state.txt" 2>&1
    state=$(sed -nE 's/^state: ([0-9]+) fail, ([0-9]+) warn.*/\1\/\2/p' "$dir/state.txt")
    [ -n "$state" ] || state="no dump"

    for i in $(mdk_seq); do since "$(mdk_log "$i")" "${off[$i]}" > "$dir/$(name "$i").log"; done
    collect_dumps "$dir"

    hl="$dir/host.log"
    spawned=$(grep -c '^ent spawn' "$hl")
    # final hp per enemy: host logs "hp <name> <hp>" after each hit, a client "event 2 on <name>: ... -> hp <hp>"
    grep -E '^hp ' "$hl" | awk '{h[$2]=$3} END {for (n in h) print n, h[n]}' | sort > "$dir/hp_host.txt"
    hits=$(wc -l < "$dir/hp_host.txt")
    pup=("$(grep -c '^lua: puppet spawned' "$hl")"); bound=(); orphans=0; hpbad=0; luaerr=$(grep -c 'LUA ERROR' "$hl")
    for i in $(seq 2 "$MDK2_N"); do
        cl="$dir/$(name "$i").log"
        pup+=("$(grep -c '^lua: puppet spawned' "$cl")")
        b=$(grep -c '^ent bind' "$cl"); bound+=("$b")
        [ "$b" -ne "$spawned" ] && notes="$notes binding mismatch on $(name "$i");"
        orphans=$((orphans + $(grep -c '^ent orphan' "$cl")))
        luaerr=$((luaerr + $(grep -c 'LUA ERROR' "$cl")))
        grep -E '^event 2 on ' "$cl" | sed -E 's/^event 2 on ([^:]+):.* hp (-?[0-9]+)$/\1 \2/' |
            awk '{h[$1]=$2} END {for (n in h) print n, h[n]}' | sort > "$dir/hp_$(name "$i").txt"
        bad=$(join "$dir/hp_host.txt" "$dir/hp_$(name "$i").txt" | awk '$2 != $3')
        if [ -n "$bad" ]; then
            hpbad=$((hpbad + $(echo "$bad" | wc -l)))
            notes="$notes hp differs on $(name "$i"): $(echo "$bad" | head -2 | tr '\n' ';')"
        fi
    done
    if alive_all; then alive=yes; else alive=NO; notes="$notes crashed;"; fi
    ls "$dir"/*.mdmp >/dev/null 2>&1 && notes="$notes dump saved;"
    [ "$luaerr" -gt 0 ] && notes="$notes $(grep -h 'LUA ERROR' "$dir"/*.log | sort | uniq -c | head -2 | tr -s ' ' | tr '\n' ';')"
    grep -q '^down: ' "$dir"/*.log && notes="$notes went down;"
    grep -q '^FAIL' "$dir/state.txt" && notes="$notes $(grep -m2 '^FAIL' "$dir/state.txt" | sed 's/^FAIL  //' | tr '\n' ';')"
    echo "| $c | $followed | $(join_by / "${pup[@]}") | $spawned/$(join_by / "${bound[@]}") | $orphans | $luaerr | $hits | $hpbad | $state | $alive | ${notes:-} |" >> "$results"
    echo "$c: followed $followed, alive $alive ${notes:-}"
done
mdk_kill
echo
echo "results: $results"
