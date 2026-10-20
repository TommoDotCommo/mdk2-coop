#!/usr/bin/env bash
# Checkpoint sweep: visit every checkpoint (the debug menu's list, same as F6) with two
# instances, do a few things at each and check both games. Restarts both after a crash
# and carries on from the next checkpoint.
#   tools/sweep.sh                 every checkpoint
#   tools/sweep.sh 2-1 3-4         from 2-1 up to and including 3-4
# Results: local/sweeps/<date-time>/results.md, plus per checkpoint both log slices,
# both screenshots and any crash dumps. Takes roughly 30 s per checkpoint.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"

ALL="1-1 1-2 1-3 1-4 1-5 1-6 1-7 1-8 2-1 2-2 2-3 2-4 2-5 2-6 2-7 2-8 2-9 2-10
3-1 3-2 3-3 3-4 3-5 3-6 3-7 3-8 3-9 3-10 3-11 4-1 4-2 4-3 4-4 4-5 4-6 4-7 4-8 4-9 4-10 4-11 4-14
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

out="$here/../local/sweeps/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
results="$out/results.md"
{
    echo "# Checkpoint sweep $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "| checkpoint | followed | puppets | enemies host/client | orphans | lua errors | enemies hit | hp mismatches | alive | notes |"
    echo "|---|---|---|---|---|---|---|---|---|---|"
} > "$results"

LOG1="$(mdk_log 1)"; LOG2="$(mdk_log 2)"
lines() { wc -l < "$1" 2>/dev/null || echo 0; }
since() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; }     # since <log> <line count before>
wait_for() {   # wait_for <log> <offset> <regex> <seconds>
    local i
    for i in $(seq 1 "$4"); do since "$1" "$2" | grep -qE "$3" && return 0; sleep 1; done
    return 1
}
alive_both() { read -r A B < "$MDK2_PIDS"; mdk_alive "$A" && mdk_alive "$B"; }
collect_dumps() {   # move new crash dumps into the checkpoint's folder
    local i d f
    for i in 1 2; do
        d="$(mdk_wsl "$(mdk_dir $i)")"
        for f in "$d"/mdk2hd_crash_*.mdmp; do
            [ -e "$f" ] && mv "$f" "$1/$([ $i = 1 ] && echo host || echo client)-$(basename "$f")"
        done
    done
}
start_at() {   # start_at <checkpoint>: fresh pair with the host autostarting there
    MDK2_START="${1%-*} ${1#*-}" "$here/install.sh" >/dev/null
    mdk_boot
}

collect_dumps "$out"        # old dumps out of the way
first=1
for c in $list; do
    l="${c%-*}"; cp="${c#*-}"
    dir="$out/$c"; mkdir -p "$dir"
    notes=""
    o1=$(lines "$LOG1"); o2=$(lines "$LOG2")

    if [ $first = 1 ] || ! alive_both; then
        start_at "$c"
        o1=0; o2=0
    else
        read -r A B < "$MDK2_PIDS"
        mdk_key "$A" 64                                   # F6: next checkpoint
    fi
    first=0
    read -r A B < "$MDK2_PIDS"

    followed=no
    if wait_for "$LOG1" "$o1" "^level $l checkpoint $cp started" 40 &&
       wait_for "$LOG2" "$o2" "^level $l checkpoint $cp started" 40; then
        followed=yes
    else
        notes="$notes host at: $(since "$LOG1" "$o1" | grep -E '^level' | tail -1 | tr -d '\r');"
    fi
    sleep 6                                               # let the level settle, enemies announce

    # actions: both walk, both fire, the client hits something through its gun path
    mdk_key "$A" 17 1 1500; mdk_click "$A" 1200 1
    mdk_key "$B" 17 1 1000; mdk_click "$B" 1200 1
    mdk_key "$B" 59                                       # client F1: hits coop_dog1 if there is one
    mdk_key "$A" 32 1 800; mdk_key "$B" 30 1 800          # strafe apart
    sleep 3
    mdk_shot "$A" "sweep_host" >/dev/null 2>&1; mdk_shot "$B" "sweep_client" >/dev/null 2>&1
    mv "$(mdk_wsl "$MDK2_TOOLS")/sweep_host.png" "$dir/host.png" 2>/dev/null
    mv "$(mdk_wsl "$MDK2_TOOLS")/sweep_client.png" "$dir/client.png" 2>/dev/null

    since "$LOG1" "$o1" > "$dir/host.log"; since "$LOG2" "$o2" > "$dir/client.log"
    collect_dumps "$dir"

    puppets="$(grep -c '^lua: puppet spawned' "$dir/host.log")/$(grep -c '^lua: puppet spawned' "$dir/client.log")"
    spawned=$(grep -c '^ent spawn' "$dir/host.log"); bound=$(grep -c '^ent bind' "$dir/client.log")
    orphans=$(grep -c '^ent orphan' "$dir/client.log")
    luaerr=$(( $(grep -c 'LUA ERROR' "$dir/host.log") + $(grep -c 'LUA ERROR' "$dir/client.log") ))
    # final hp per enemy: host logs "hp <name> <hp>" after each hit, the client "event 2 on <name>: ... -> hp <hp>"
    grep -E '^hp ' "$dir/host.log" | awk '{h[$2]=$3} END {for (n in h) print n, h[n]}' | sort > "$dir/hp_host.txt"
    grep -E '^event 2 on ' "$dir/client.log" | sed -E 's/^event 2 on ([^:]+):.* hp (-?[0-9]+)$/\1 \2/' |
        awk '{h[$1]=$2} END {for (n in h) print n, h[n]}' | sort > "$dir/hp_client.txt"
    hits=$(wc -l < "$dir/hp_host.txt")
    hpbad=$(join "$dir/hp_host.txt" "$dir/hp_client.txt" | awk '$2 != $3' | wc -l)
    if alive_both; then alive=yes; else alive=NO; notes="$notes crashed;"; fi
    ls "$dir"/*.mdmp >/dev/null 2>&1 && notes="$notes dump saved;"
    [ "$spawned" -ne "$bound" ] && notes="$notes binding mismatch;"
    [ "$luaerr" -gt 0 ] && notes="$notes $(grep -h 'LUA ERROR' "$dir"/*.log | sort | uniq -c | head -2 | tr -s ' ' | tr '\n' ';')"
    grep -q '^down: ' "$dir/host.log" "$dir/client.log" && notes="$notes went down;"

    [ "$hpbad" -gt 0 ] && notes="$notes hp differs: $(join "$dir/hp_host.txt" "$dir/hp_client.txt" | awk '$2 != $3' | head -2 | tr '\n' ';')"
    echo "| $c | $followed | $puppets | $spawned/$bound | $orphans | $luaerr | $hits | $hpbad | $alive | ${notes:-} |" >> "$results"
    echo "$c: followed $followed, alive $alive ${notes:-}"
done
mdk_kill
echo
echo "results: $results"
