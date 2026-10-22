#!/usr/bin/env bash
# Recorded controls, replayed inside the bridge. See routes/README.md.
#   tools/replay.sh record <name> [instance]  start in an existing session
#   tools/replay.sh stop <name> [instance]    save/export to local/routes/<name>.route
#   tools/replay.sh run <file.route>          fresh background host + clients, progression + state checks
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"
export MDK2_N
action="${1:-}"; route="${2:-}"; instance="${3:-1}"
case "$action" in
record|stop)
    [[ "$route" =~ ^[A-Za-z0-9_-]{1,63}$ ]] || { echo 'invalid route name' >&2; exit 1; }
    [[ "$instance" =~ ^[0-9]+$ ]] && [ "$instance" -ge 1 ] && [ "$instance" -le "$MDK2_N" ] || exit 1
    mdk_cmd "$(mdk_pid "$instance")" "coop_route(\"$action\", \"$route\")"
    d="$(mdk_wsl "$(mdk_dir "$instance")")/coop"
    # command.lua is removed before its Lua executes; wait for the result file as well.
    for attempt in $(seq 1 100); do
        if grep -qx "route $route" "$d/route_status.txt" 2>/dev/null; then
            if [ "$action" = record ]; then
                grep -Eq '^status (recording|fail)$' "$d/route_status.txt" && break
            else
                grep -Eq '^status (idle|fail)$' "$d/route_status.txt" && break
            fi
        fi
        sleep 0.1
    done
    cat "$d/route_status.txt"
    if [ "$action" = stop ]; then
        [ -s "$d/$route.route" ] && grep -qx "route $route" "$d/route_status.txt" &&
            grep -q '^reason recording saved$' "$d/route_status.txt" || exit 1
        mkdir -p "$here/local/routes"
        cp "$d/$route.route" "$here/local/routes/$route.route"
        echo "recording: $here/local/routes/$route.route"
    else
        grep -q '^status recording$' "$d/route_status.txt"
    fi
    exit
    ;;
run) [ -f "$route" ] || { echo "no route: $route" >&2; exit 1; } ;;
*) echo "usage: tools/replay.sh record|stop <name> [instance] | run <file.route>" >&2; exit 2 ;;
esac
[[ "$MDK2_N" =~ ^[0-9]+$ ]] && [ "$MDK2_N" -ge 2 ] && [ "$MDK2_N" -le 16 ] || {
    echo 'replay QA requires 2..16 real game instances' >&2; exit 1;
}
# Validate with the game's parser before changing or starting any copies.
mkdir -p "$here/local"
info=$(mktemp "$here/local/route-info.XXXXXX")
trap 'rm -f "$info"' EXIT
cc -std=c99 -O2 "$here/tools/route_info.c" "$here/bridge/route.c" -lm -o "$info"
read -r level cp duration frames < <("$info" "$route")
rm -f "$info"
trap - EXIT
out=$(mktemp -d "$here/local/replays/XXXXXXXX" 2>/dev/null) || {
    mkdir -p "$here/local/replays"
    out=$(mktemp -d "$here/local/replays/XXXXXXXX")
}
cp "$route" "$out/route.route"
export MDK2_BACKGROUND=1
result=FAIL
reason='runner interrupted or failed'
started_games=0
collect() {
    local code=$? i d p
    trap - EXIT INT TERM
    set +e
    if [ "$started_games" = 1 ]; then
        if [ "$result" != PASS ] && [ -f "$(mdk_wsl "$(mdk_dir 1)")/coop/route_status.txt" ]; then
            mdk_state failure "$out" > "$out/failure-state.txt" 2>&1
            for i in $(mdk_seq); do
                if [ -f "$(mdk_wsl "$(mdk_dir "$i")")/coop/route_status.txt" ] && mdk_alive "$(mdk_pid "$i")"; then
                    mdk_shot "$(mdk_pid "$i")" "$out/failure-$i.png"
                fi
            done
        fi
        for i in $(mdk_seq); do
            d="$(mdk_wsl "$(mdk_dir "$i")")"
            cp "$d/coop/bridge.log" "$out/game-$i.log"
            cp "$d/coop/route_status.txt" "$out/game-$i-status.txt"
            for p in "$d"/mdk2hd_crash_*.mdmp; do [ -e "$p" ] && cp "$p" "$out/game-$i-$(basename "$p")"; done
        done
        mdk_kill
    fi
    printf 'RESULT: %s\n%s\n' "$result" "$reason" | tee "$out/result.txt"
    echo "evidence: $out"
    [ "$result" = PASS ] && exit 0
    [ "$code" -ne 0 ] && exit "$code"
    exit 1
}
trap collect EXIT
trap 'reason="runner interrupted"; exit 130' INT TERM
MDK2_START="$level $cp" "$here/tools/install.sh" > "$out/install.txt" 2>&1
started_games=1
for i in $(mdk_seq); do rm -f "$(mdk_wsl "$(mdk_dir "$i")")/coop/route_status.txt"; done
: > "$MDK2_PIDS"
for i in $(mdk_seq); do mdk_start "$i" >> "$MDK2_PIDS"; sleep 2; done
ready=0
deadline=$((SECONDS + 90))
while [ "$SECONDS" -lt "$deadline" ]; do
    ready=1
    for i in $(mdk_seq); do
        status_file="$(mdk_wsl "$(mdk_dir "$i")")/coop/route_status.txt"
        if ! [ -f "$status_file" ] || ! grep -qx "level $level" "$status_file" ||
           ! grep -qx "players $((MDK2_N - 1))" "$status_file"; then ready=0; fi
    done
    [ "$ready" = 0 ] || break
    sleep 0.25
done
[ "$ready" = 1 ] || { reason='games did not load the bridge and join the starting level within 90 s'; exit 1; }
sleep 1
host="$(mdk_wsl "$(mdk_dir 1)")/coop"
cp "$out/route.route" "$host/replay_bot.route"
mdk_cmd "$(mdk_pid 1)" 'coop_route("replay", "replay_bot")'
for attempt in $(seq 1 100); do
    if grep -qx 'route replay_bot' "$host/route_status.txt" &&
       grep -Eq '^status (running|pass|fail)$' "$host/route_status.txt"; then break; fi
    sleep 0.1
done
# Periodic progress snapshots test sync after actual progression, while playback continues.
deadline=$((SECONDS + (duration + 999) / 1000 + 15))
previous="$level-$cp"; step=0
while [ "$SECONDS" -lt "$deadline" ]; do
    state=$(cat "$host/route_status.txt" 2>/dev/null || true)
    status=$(sed -n 's/^status //p' <<< "$state")
    current="$(sed -n 's/^level //p' <<< "$state")-$(sed -n 's/^checkpoint //p' <<< "$state")"
    for p in $(mdk_pids); do
        mdk_alive "$p" || { reason="game $p exited during replay"; exit 1; }
    done
    [ "$status" != fail ] || { reason=$(sed -n 's/^reason //p' <<< "$state"); exit 1; }
    [ "$status" = running ] || [ "$status" = pass ] || { reason="replay did not start ($status)"; exit 1; }
    if [ "$current" != "$previous" ] || [ "$status" = pass ]; then
        step=$((step + 1)); previous="$current"
        if ! mdk_state "route_$step" "$out" > "$out/state-$step.txt" 2>&1; then
            sleep 1.5
            mdk_state "retry_$step" "$out" > "$out/state-$step-retry.txt" 2>&1 || {
                reason="persistent world mismatch after progression to $current"; exit 1;
            }
        fi
    fi
    if [ "$status" = pass ]; then
        # Progression QA also requires living participants and clean logs.
        for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game-$i.log"; done
        python3 "$here/tools/replay_check.py" "$out" "$MDK2_N" | tee "$out/participants.txt" || {
            reason='client, life or log check failed'; exit 1;
        }
        result=PASS; reason="recorded progression and endpoint reached; $MDK2_N worlds checked"
        break
    fi
    sleep 0.25
done
[ "$result" = PASS ] || reason='replay timed out without a terminal result'
