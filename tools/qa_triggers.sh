#!/usr/bin/env bash
# Script triggers that look at "the player", visited on purpose (tools/script_census.py --triggers): a boss that
# starts when the local player comes within 40 units of a point runs for the player of whichever game is running
# that script, so a client reaching it can start the fight on its own game only. For each level in LEVELS the host
# starts it with a client, then each game's player in turn is sent to every place the level's inline handlers
# look at (the handler's gob and the names in its body); afterwards tools/script_diff.py lists the handlers that ran
# on one game and not the other. A difference that tools/script_known.txt does not list fails the test if its event is synced or
# partly synced (OnDie, OnPickedUp) or a handler ran on a client and never on the host; host-only differences in other
# events are listed for a look (the first runs found timing-dependent ones).
#   LEVELS="1 3 7" tools/qa_triggers.sh      (default: 1 7)      MAX=<n> only the first n places per level
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about a minute per level of 10 places.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1
LEVELS="${LEVELS:-1 7}"
MAX="${MAX:-1000}"

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-triggers"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
has() { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
trap 'mdk_kill' EXIT
archive="$(mdk_wsl "$(mdk_dir 1)")/mdk2.tar"
[ -f "$archive" ] || { echo "no mdk2.tar in the game copy ($archive)"; exit 1; }

# game <n>'s own player to the gob or point called $2 (levels name them as globals)
visit() {
    lua "$1" "local p = mdkGetPlayerGob() local x = p.position.x local y = p.position.y local z = p.position.z mdkGobSetPosition(p, \"$2\") if (p.position.x == x and p.position.y == y and p.position.z == z) then coop_log(\"trigger $2: not moved\") else coop_log(\"trigger $2: moved\") end"
}
# this level's lines of a game's log: from the level start on (the logs run across levels)
since() { tail -n +"$(( $2 + 1 ))" "$(mdk_log "$1")"; }
reached() { since "$1" "$2" | grep -ac "^lua: trigger .*: moved"; }

mdk_boot
[ "${NOREMOTE:-}" = 1 ] && for i in 1 2; do lua "$i" 'coop_test_noremote(1)'; done     # compare with the handlers for other players' events off
for L in $LEVELS; do
    mapfile -t places < <(python3 "$here/script_census.py" --archive "$archive" --triggers "$L" | head -n "$MAX")
    echo "level $L: ${#places[@]} places" | tee -a "$out/places.txt"
    h0=$(wc -l < "$(mdk_log 1)"); c0=$(wc -l < "$(mdk_log 2)")
    lua 1 "mdkNewGame($L, 1)"
    check "level $L: the host started it"           wait_log 1 "^level $L checkpoint 1 started" 40
    check "level $L: the client followed"           wait_log 2 "^level $L checkpoint 1 started" 40
    sleep 8
    for who in 2 1; do                   # the client first: what only it starts is the bug this looks for
        for p in "${places[@]}"; do
            visit "$who" "$p"; sleep 2.5
            all_alive || break 2
        done
    done
    check "level $L: both games survived the visits" all_alive
    echo "level $L: the host reached $(reached 1 $h0) and the client $(reached 2 $c0) of ${#places[@]} places" | tee -a "$out/places.txt"
    check "level $L: the visits went somewhere (at least half of the places)" test "$(reached 1 $h0)" -ge "$(( ${#places[@]} / 2 ))" -a "$(reached 2 $c0)" -ge "$(( ${#places[@]} / 2 ))"
    sleep 3
    cp "$(mdk_log 1)" "$out/game1.log"; cp "$(mdk_log 2)" "$out/game2.log"
    python3 "$here/script_diff.py" "$out/game1.log" "$out/game2.log" --levels "$L" --client-only > "$out/diff-$L.txt" 2>&1
    rc=$?
    cat "$out/diff-$L.txt"
    check "level $L: no new client-only or promised script difference" test $rc = 0
    # a teleported player meets rooms in states the script never expects, so a Lua error here is a lead, not a verdict: named, not failed
    for i in 1 2; do since "$i" "$([ $i = 1 ] && echo $h0 || echo $c0)" | grep -a -B3 "LUA ERROR" > "$out/lua-errors-$L-game$i.txt"; done
    echo "level $L: Lua errors: host $(grep -ac 'LUA ERROR' "$out/lua-errors-$L-game1.txt"), client $(grep -ac 'LUA ERROR' "$out/lua-errors-$L-game2.txt") (see lua-errors-$L-game*.txt)" | tee -a "$out/places.txt"
done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs and diffs: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
