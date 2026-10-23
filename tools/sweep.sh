#!/usr/bin/env bash
# Checkpoint sweep: visit every checkpoint (the debug menu's list, same as F6) with MDK2_N
# instances (default 2: host and one client), do a few things at each and check every game.
# Restarts all after a crash and carries on from the next checkpoint.
#   tools/sweep.sh                 every checkpoint
#   tools/sweep.sh 2-1 3-4         from 2-1 up to and including 3-4
#   tools/sweep.sh --levels        first checkpoint of each of the 12 levels (F8 between them)
#   MDK2_N=4 tools/sweep.sh        host and three clients (game copies: tools/mdk.sh, tools/copies.sh)
#   MDK2_SHARDS=4 tools/sweep.sh   four sets of MDK2_N games side by side, each on its own run of
#                                  checkpoints (copies 1..4*MDK2_N, ports from MDK2_PORTS or spaced by 16)
#   MDK2_SHOTS=1 tools/sweep.sh    screenshots at every checkpoint (default: only where a check fails)
# Results: local/sweeps/<date-time>/results.md, plus per checkpoint every game's log slice, state
# dump with statecheck.py's report (state.txt), screenshots where something failed and any crash
# dumps. Waits are on conditions, not fixed sleeps, and the games act at the same time: about
# 8-10 s per checkpoint, divided by the number of shards.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
[[ "$MDK2_N" =~ ^[0-9]+$ ]] && [ "$MDK2_N" -ge 2 ] && [ "$MDK2_N" -le 16 ] || {
    echo 'sweep requires 2..16 instances' >&2; exit 1;
}

ALL="$(tr '\n' ' ' < "$here/checkpoints.txt")"
HEADER="| checkpoint | followed | puppets host/clients | enemies host/bound | orphans | lua errors | enemies hit | hp mismatches | state fail/warn | alive | notes |"

shards="${MDK2_SHARDS:-1}"
child="${MDK2_SWEEP_CHILD:-}"
levels="${MDK2_SWEEP_LEVELS:-0}"
[[ "$shards" =~ ^[0-9]+$ ]] && [ "$shards" -ge 1 ] || { echo 'invalid shard count' >&2; exit 1; }
if [ -z "$child" ] && [ -n "${MDK2_PORTS:-}" ]; then
    IFS=, read -r -a copyports <<< "$MDK2_PORTS"
    [ "$((MDK2_N * shards))" -le "${#copyports[@]}" ] || {
        echo 'sweep needs a reserved port for each configured copy' >&2; exit 1;
    }
    for copyport in "${copyports[@]}"; do
        [[ "$copyport" =~ ^[0-9]+$ ]] && [ "$copyport" -ge 1024 ] && [ "$copyport" -le 65535 ] || {
            echo 'invalid MDK2_PORTS entry' >&2; exit 1;
        }
    done
fi

if [ -n "$child" ]; then
    list="$MDK2_SWEEP_LIST"; out="$MDK2_SWEEP_OUT"
else
    if [ "${1:-}" = --levels ]; then
        [ "$#" = 1 ] || { echo 'usage: sweep.sh --levels | [from] [to]' >&2; exit 1; }
        levels=1
        list=""; previous=""
        for c in $ALL; do
            if [ "${c%-*}" != "$previous" ]; then list="$list $c"; previous="${c%-*}"; fi
        done
    else
        from="${1:-1-1}"; to="${2:-12-11}"
        [[ " $ALL " == *" $from "* ]] || { echo "unknown checkpoint: $from" >&2; exit 1; }
        [[ " $ALL " == *" $to "* ]] || { echo "unknown checkpoint: $to" >&2; exit 1; }
        list=""; on=0; reached=0
        for c in $ALL; do
            [ "$c" = "$from" ] && on=1
            [ $on = 1 ] && list="$list $c"
            if [ "$c" = "$to" ] && [ "$on" = 1 ]; then reached=1; break; fi
        done
        [ -n "$list" ] && [ "$reached" = 1 ] || { echo "no checkpoints between $from and $to"; exit 1; }
    fi
    out="$here/../local/sweeps/$(date +%Y%m%d-%H%M%S)-$$$([ "$MDK2_N" -gt 2 ] && echo "-n$MDK2_N")$([ "$shards" -gt 1 ] && echo "-x$shards")"
    mkdir -p "$out"
fi

# ---- parent of shards: split the list, start one sweep per set of copies, merge their rows ----
if [ -z "$child" ] && [ "$shards" -gt 1 ]; then
    make -s -C "$here/../bridge" || exit 1
    set -- $list; total=$#; per=$(( (total + shards - 1) / shards ))
    start=$(date +%s); pids=()
    stop_shards() {
        local pid
        for pid in $(jobs -pr); do kill -TERM "$pid" 2>/dev/null || true; done
        for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    }
    trap stop_shards EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    for j in $(seq 0 $((shards - 1))); do
        part=$(echo $list | tr ' ' '\n' | sed -n "$((j * per + 1)),$(((j + 1) * per))p" | tr '\n' ' ')
        [ -n "${part// /}" ] || continue
        shard_port=$((MDK2_PORT + 16 * j))
        if [ -n "${MDK2_PORTS:-}" ]; then
            shard_port=${copyports[j * MDK2_N]}
        fi
        envs=(MDK2_SWEEP_CHILD=$((j + 1)) MDK2_SWEEP_LIST="$part" MDK2_SWEEP_OUT="$out" MDK2_SHARDS=1 MDK2_NO_BUILD=1
              MDK2_SWEEP_LEVELS="$levels"
              MDK2_PORT="$shard_port" MDK2_PIDS="$MDK2_PIDS.shard$((j + 1))")
        for i in $(mdk_seq); do envs+=("MDK2_DIR$i=$(mdk_dir $((j * MDK2_N + i)))"); done
        env "${envs[@]}" "$0" > "$out/shard$((j + 1)).out" 2>&1 &
        pids+=($!)
        echo "shard $((j + 1)): $part($(env "${envs[@]}" bash -c 'source "'"$here"'/mdk.sh"; for i in $(mdk_seq); do printf "%s " "$(mdk_dir $i)"; done'))"
    done
    shard_failed=0
    for pid in "${pids[@]}"; do wait "$pid" || shard_failed=1; done
    trap - EXIT INT TERM
    {
        echo "# Checkpoint sweep $(date '+%Y-%m-%d %H:%M'), $MDK2_N games x $shards sets, $(( $(date +%s) - start )) s"
        echo; echo "Per-client columns list clients 2..$MDK2_N separated by /."; echo
        echo "$HEADER"; echo "|---|---|---|---|---|---|---|---|---|---|---|"
        for c in $list; do grep -h "^| $c |" "$out"/rows-*.md 2>/dev/null || echo "| $c | not run |  |  |  |  |  |  |  |  | shard stopped |"; done
    } > "$out/results.md"
    grep -h '^[0-9]*-[0-9]*: ' "$out"/shard*.out || true
    echo; echo "results: $out/results.md ($(( $(date +%s) - start )) s)"
    python3 "$here/sweep_check.py" "$out/results.md" --players "$MDK2_N" --expected $list > "$out/summary.txt"
    score=$?; cat "$out/summary.txt"
    [ "$score" = 0 ] && [ "$shard_failed" = 0 ]
    exit $?
fi

# ---- one set of games ----
trap mdk_kill EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
rows="$out/rows-${child:-1}.md"
: > "$rows"
name() { case $1 in 1) echo host;; 2) echo client;; *) echo "client$1";; esac; }   # as mdk_state names them
lines() { { wc -l < "$1"; } 2>/dev/null || echo 0; }
since() { tail -n +"$(( $2 + 1 ))" "$1" 2>/dev/null; }     # since <log> <line count before>
# a team wipe reloads the checkpoint and the puppets are made again: only the last load counts
last_load_puppets() { awk '/^level [0-9]+ checkpoint .* started/ {n = 0} /^lua: puppet spawned/ {n++} END {print n + 0}' "$1"; }
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
    MDK2_BACKGROUND=1 MDK2_START="${1%-*} ${1#*-}" "$here/install.sh" >/dev/null || return 1
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
        start_at "$c" || { echo "fixture installation failed for $c" >&2; exit 1; }
        for i in $(mdk_seq); do off[$i]=0; done
    else
        if [ "$levels" = 1 ]; then
            mdk_key "$(mdk_pid 1)" 66                     # F8: first checkpoint of next level
        else
            mdk_key "$(mdk_pid 1)" 64                     # F6: next checkpoint
        fi
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
        first_failure=$(grep -m1 '^FAIL' "$dir/state.txt" | sed 's/^FAIL  //')
        mv "$dir/state.txt" "$dir/state_first.txt"
        for f in "$dir"/state_"$c"_*.txt; do mv "$f" "${f%.txt}.first.txt"; done
        sleep 1.5
        mdk_state "$c" "$dir" > "$dir/state.txt" 2>&1
        grep -q '^FAIL' "$dir/state.txt" || notes="$notes transient (gone 1.5 s later): $first_failure;"
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
    pup=("$(last_load_puppets "$hl")"); bound=(); orphans=0; hpbad=0; luaerr=$(grep -c 'LUA ERROR' "$hl")
    for i in $(seq 2 "$MDK2_N"); do
        cl="$dir/$(name "$i").log"
        pup+=("$(last_load_puppets "$cl")")
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

report="$rows"
[ -n "$child" ] || report="$out/results.md"
python3 "$here/sweep_check.py" "$report" --players "$MDK2_N" --expected $list | tee "$out/summary-${child:-1}.txt"
