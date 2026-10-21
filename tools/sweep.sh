#!/usr/bin/env bash
# Checkpoint sweep: visit every checkpoint (the debug menu's list, same as F6) with MDK2_N
# instances (default 2: host and one client), do a few things at each and check every game.
# Restarts all after a crash and carries on from the next checkpoint.
#   tools/sweep.sh                 every checkpoint
#   tools/sweep.sh 2-1 3-4         from 2-1 up to and including 3-4
#   MDK2_N=4 tools/sweep.sh        host and three clients (game copies: tools/mdk.sh, tools/copies.sh)
#   MDK2_SHARDS=4 tools/sweep.sh   four sets of MDK2_N games side by side, each on its own run of
#                                  checkpoints (copies 1..4*MDK2_N, ports MDK2_PORT + 10 per set)
#   MDK2_SHOTS=1 tools/sweep.sh    screenshots at every checkpoint (default: only where a check fails)
# Results: local/sweeps/<date-time>/results.md, plus per checkpoint every game's log slice, state
# dump with statecheck.py's report (state.txt), screenshots where something failed and any crash
# dumps. Waits are on conditions, not fixed sleeps, and the games act at the same time: about
# 8-10 s per checkpoint, divided by the number of shards.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"

ALL="1-1 1-2 1-3 1-4 1-5 1-6 1-7 1-8 2-1 2-2 2-3 2-4 2-5 2-6 2-7 2-8 2-9 2-10
3-1 3-2 3-3 3-4 3-5 3-6 3-16 3-7 3-8 3-9 3-10 3-11 4-1 4-2 4-3 4-4 4-5 4-6 4-7 4-8 4-9 4-10 4-11 4-14
5-1 5-2 5-3 5-4 5-5 5-6 5-7 5-8 5-9 5-10 6-1 6-2 6-3 6-4 6-5 6-6 6-7 7-1 7-2 7-3 7-4 7-5 7-6 7-7 7-8
8-1 8-2 8-3 8-4 8-5 8-6 8-7 9-1 9-3 9-4 9-5 9-6 9-7 9-8 9-11 9-12 10-1 10-2 10-3 10-4 10-5 10-6 10-11
11-1 11-6 11-11 12-1 12-6 12-11"
HEADER="| checkpoint | followed | puppets host/clients | enemies host/bound | orphans | lua errors | enemies hit | hp mismatches | state fail/warn | alive | notes |"

shards="${MDK2_SHARDS:-1}"
child="${MDK2_SWEEP_CHILD:-}"

if [ -n "$child" ]; then
    list="$MDK2_SWEEP_LIST"; out="$MDK2_SWEEP_OUT"
else
    from="${1:-1-1}"; to="${2:-12-11}"
    list=""; on=0
    for c in $ALL; do
        [ "$c" = "$from" ] && on=1
        [ $on = 1 ] && list="$list $c"
        [ "$c" = "$to" ] && break
    done
    [ -n "$list" ] || { echo "no checkpoints between $from and $to"; exit 1; }
    out="$here/../local/sweeps/$(date +%Y%m%d-%H%M%S)$([ "$MDK2_N" -gt 2 ] && echo "-n$MDK2_N")$([ "$shards" -gt 1 ] && echo "-x$shards")"
    mkdir -p "$out"
fi

# ---- parent of shards: split the list, start one sweep per set of copies, merge their rows ----
if [ -z "$child" ] && [ "$shards" -gt 1 ]; then
    make -s -C "$here/../bridge" || exit 1
    set -- $list; total=$#; per=$(( (total + shards - 1) / shards ))
    start=$(date +%s); pids=()
    for j in $(seq 0 $((shards - 1))); do
        part=$(echo $list | tr ' ' '\n' | sed -n "$((j * per + 1)),$(((j + 1) * per))p" | tr '\n' ' ')
        [ -n "${part// /}" ] || continue
        envs=(MDK2_SWEEP_CHILD=$((j + 1)) MDK2_SWEEP_LIST="$part" MDK2_SWEEP_OUT="$out" MDK2_SHARDS=1 MDK2_NO_BUILD=1
              MDK2_PORT=$((MDK2_PORT + 10 * j)) MDK2_PIDS="$MDK2_PIDS.shard$((j + 1))")
        for i in $(mdk_seq); do envs+=("MDK2_DIR$i=$(mdk_dir $((j * MDK2_N + i)))"); done
        env "${envs[@]}" "$0" > "$out/shard$((j + 1)).out" 2>&1 &
        pids+=($!)
        echo "shard $((j + 1)): $part($(env "${envs[@]}" bash -c 'source "'"$here"'/mdk.sh"; for i in $(mdk_seq); do printf "%s " "$(mdk_dir $i)"; done'))"
    done
    wait "${pids[@]}"
    {
        echo "# Checkpoint sweep $(date '+%Y-%m-%d %H:%M'), $MDK2_N games x $shards sets, $(( $(date +%s) - start )) s"
        echo; echo "Per-client columns list clients 2..$MDK2_N separated by /."; echo
        echo "$HEADER"; echo "|---|---|---|---|---|---|---|---|---|---|---|"
        for c in $list; do grep -h "^| $c |" "$out"/rows-*.md 2>/dev/null || echo "| $c | not run |  |  |  |  |  |  |  |  | shard stopped |"; done
    } > "$out/results.md"
    grep -h '^[0-9]*-[0-9]*: ' "$out"/shard*.out | grep -v 'followed yes, alive yes *$' || echo "all clean"
    echo; echo "results: $out/results.md ($(( $(date +%s) - start )) s)"
    exit 0
fi

# ---- one set of games ----
rows="$out/rows-${child:-1}.md"
: > "$rows"
name() { case $1 in 1) echo host;; 2) echo client;; *) echo "client$1";; esac; }   # as mdk_state names them
lines() { { wc -l < "$1"; } 2>/dev/null || echo 0; }
since() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; }     # since <log> <line count before>
count_since() { since "$1" "$2" | grep -cE "$3"; }         # no -q anywhere: SIGPIPE under pipefail
wait_for() {   # wait_for <log> <offset> <regex> <seconds>: polls every 0.25 s
    local i
    for i in $(seq 1 $(( $4 * 4 ))); do since "$1" "$2" | grep -E "$3" >/dev/null && return 0; sleep 0.25; done
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
# settled: every game has its puppets and every client has bound every enemy the host announced,
# with no new announcement for a second (instead of a fixed 6 s)
settled() {
    local s b i last=-1 stable=0 n
    for n in $(seq 1 32); do           # 8 s at most
        s=$(count_since "$(mdk_log 1)" "${off[1]}" '^ent spawn')
        local ok=1
        for i in $(mdk_seq); do
            [ "$(count_since "$(mdk_log "$i")" "${off[$i]}" '^lua: puppet spawned')" -ge $((MDK2_N - 1)) ] || ok=0
            [ "$i" = 1 ] && continue
            b=$(count_since "$(mdk_log "$i")" "${off[$i]}" '^ent bind')
            [ "$b" -ge "$s" ] || ok=0
        done
        if [ $ok = 1 ] && [ "$s" = "$last" ]; then stable=$((stable + 1)); else stable=0; fi
        last=$s
        [ $stable -ge 4 ] && return 0
        sleep 0.25
    done
    return 1
}
# quiet: no new hit, hp or shot line in any game for 0.5 s (instead of a fixed 3 s), 4 s at most
quiet() {
    local before now i n stable=0
    before=-1
    for n in $(seq 1 16); do
        # the logs keep growing a little (smoothing stats every 2 s), so count only the effects
        now=0
        for i in $(mdk_seq); do now=$((now + $(count_since "$(mdk_log "$i")" "${off[$i]}" '^(hp |event [0-9]+ on |replay fire|damage |down: )'))); done
        if [ "$now" = "$before" ]; then stable=$((stable + 1)); else stable=0; fi
        before=$now
        [ $stable -ge 2 ] && return 0
        sleep 0.25
    done
    return 1
}
# act <instance>: that game's part, run in the background for all games at once: walk, fire,
# (clients) hit something through the gun path, strafe (alternating directions)
act() {
    local i=$1 p; p=$(mdk_pid "$i")
    if [ "$i" = 1 ]; then mdk_cmd "$p" "coop_press(17, 1500)"; sleep 1.5
    else mdk_cmd "$p" "coop_press(17, 1000)"; sleep 1.0; fi
    mdk_cmd "$p" "coop_press(256, 1200)"; sleep 1.2
    [ "$i" != 1 ] && { mdk_cmd "$p" "coop_press(59, 80)"; sleep 0.15; }     # F1: hits coop_dog1 or the nearest enemy
    mdk_cmd "$p" "coop_press($([ $((i % 2)) = 1 ] && echo 32 || echo 30), 800)"; sleep 0.8
}

collect_dumps "$out"        # old dumps out of the way
first=1
declare -A off
for c in $list; do
    l="${c%-*}"; cp="${c#*-}"
    dir="$out/$c"; mkdir -p "$dir"
    notes=""
    t0=$(date +%s%N)
    for i in $(mdk_seq); do off[$i]=$(lines "$(mdk_log "$i")"); done

    if [ $first = 1 ] || ! alive_all; then
        start_at "$c"
        for i in $(mdk_seq); do off[$i]=0; done
    else
        mdk_key "$(mdk_pid 1)" 64                         # F6: next checkpoint
    fi
    first=0

    followed=yes
    for i in $(mdk_seq); do
        wait_for "$(mdk_log "$i")" "${off[$i]}" "^level $l checkpoint $cp started" 40 && continue
        followed=no
        notes="$notes $(name "$i") at: $(since "$(mdk_log "$i")" "${off[$i]}" | grep -E '^level' | tail -1 | tr -d '\r');"
    done
    settled || notes="$notes not settled after 8 s;"

    for i in $(mdk_seq); do act "$i" & done
    wait
    quiet || true                                         # enemies may keep shooting: go on after 4 s
    mdk_state "$c" "$dir" > "$dir/state.txt" 2>&1
    # a difference can be momentary (a revive's teleport while the other game still draws the
    # puppet 100 ms behind): check again 1.5 s later; only what is still there counts as a failure
    if grep -q '^FAIL' "$dir/state.txt"; then
        first=$(grep -m1 '^FAIL' "$dir/state.txt" | sed 's/^FAIL  //')
        mv "$dir/state.txt" "$dir/state_first.txt"
        for f in "$dir"/state_"$c"_*.txt; do mv "$f" "${f%.txt}.first.txt"; done
        sleep 1.5
        mdk_state "$c" "$dir" > "$dir/state.txt" 2>&1
        grep -q '^FAIL' "$dir/state.txt" || notes="$notes transient (gone 1.5 s later): $first;"
    fi
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
    # screenshots where something looks wrong ("went down" alone is a normal death), or always with MDK2_SHOTS=1
    if [ "${MDK2_SHOTS:-0}" = 1 ] || [ -n "$(echo "$notes" | sed 's/ went down;//; s/ transient (gone 1.5 s later): [^;]*;//; s/ //g')" ]; then
        for i in $(mdk_seq); do mdk_shot "$(mdk_pid "$i")" "$dir/$(name "$i").png" >/dev/null 2>&1; done
    fi
    secs=$(( ($(date +%s%N) - t0) / 1000000 ))
    echo "| $c | $followed | $(join_by / "${pup[@]}") | $spawned/$(join_by / "${bound[@]}") | $orphans | $luaerr | $hits | $hpbad | $state | $alive | ${notes:-} |" >> "$rows"
    echo "$c: followed $followed, alive $alive ${notes:-} ($((secs / 1000)).$((secs % 1000 / 100)) s)"
done
mdk_kill

if [ -z "$child" ]; then
    {
        echo "# Checkpoint sweep $(date '+%Y-%m-%d %H:%M'), $MDK2_N games"
        echo; echo "Per-client columns list clients 2..$MDK2_N separated by /."; echo
        echo "$HEADER"; echo "|---|---|---|---|---|---|---|---|---|---|---|"
        cat "$rows"
    } > "$out/results.md"
    rm -f "$rows"
    echo; echo "results: $out/results.md"
fi
