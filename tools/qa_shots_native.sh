#!/usr/bin/env bash
# Real projectile flight with host + two clients, including Doganboy's held grenade.
# Background mode only. MDK2_SIM="80 30 3" also exercises reliable delivery under loss.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_shots.sh needs MDK2_N >= 3"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1
out="$here/../local/qa-shots/$(date +%Y%m%d-%H%M%S)-$$"; mkdir -p "$out"
pass=0; fail=0
check() {
    local name="$1"; shift
    if "$@"; then echo "PASS $name"; pass=$((pass + 1))
    else echo "FAIL $name"; fail=$((fail + 1)); fi
}
has() { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() {
    local n; for n in $(seq 1 $(( $3 * 4 ))); do has "$1" "$2" && return 0; sleep 0.25; done; return 1
}
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
dump() {
    local start; start=$(wc -l < "$(mdk_log "$1")")
    lua "$1" 'coop_shots_dump()' || return 1
    tail -n +$((start + 1)) "$(mdk_log "$1")" > "$out/$2_$1.txt"
}
cleanup() {
    local i; for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
    mdk_kill
}
trap cleanup EXIT
mdk_boot
for i in $(mdk_seq); do
    check "game $i installed the launch hook" has "$i" '^launch_shot hook installed'
    lua "$i" 'mdkAddHitpoints(mdkGetPlayerGob(), 20000)'
done
check "host has both clients" has 1 'player 3 connected'

# The native throwing function follows the same hand attachment and animation release
# used by Doganboy AI. Wait for a live attack first: inactive enemies ignore a throw.
lua 1 'make("shot_dog", OBJ_DOGANBOY)'
check "Doganboy really fired" wait_log 1 '^fire type 403 .*launched velocity' 12
for n in $(seq 1 12); do
    lua 1 'mdkWalkerThrowObject(shot_dog, OBJ_DOGANBOYGRENADE)'
    wait_log 1 '^fire type 417 .*launched velocity' 1 && break
done
check "Doganboy released its grenade" has 1 '^fire type 417 .*launched velocity'
# Capture each client's pair concurrently, before another client's shell round trips can
# consume the grenade's lifetime. Both samples still require a live, moving projectile.
flight_pids=()
for i in $(seq 2 "$MDK2_N"); do
    (
        wait_log "$i" '^replay fire type 417 .*created' 5 || exit 1
        dump "$i" flight1 || exit 1
        sleep 0.1
        dump "$i" flight2
    ) &
    flight_pids+=($!)
done
# Missing/failed snapshots fail the active-projectile and position checks below.
for flight_pid in "${flight_pids[@]}"; do wait "$flight_pid" || true; done
for i in $(seq 2 "$MDK2_N"); do
    check "client $i replayed the grenade" has "$i" '^replay fire type 417 .*created'
    check "client $i grenade is active and foreign" grep -qE '^shot type 417 active 1 foreign 1 own 0 ' "$out/flight1_$i.txt"
done
lua 1 'mdkDealDamage(mdkGetPlayerGob(), shot_dog, 9999, 1, -1)'

for i in $(seq 2 "$MDK2_N"); do
    check "client $i grenade moves" python3 - "$out/flight1_$i.txt" "$out/flight2_$i.txt" <<'PY'
import re, sys
def pos(path):
    m = re.search(r'^shot type 417 .* at (\([^)]*\))', open(path).read(), re.M)
    return m[1] if m else None
a, b = map(pos, sys.argv[1:])
sys.exit(0 if a and b and a != b else 1)
PY
done

# A client's native sniper launch must reach the host and the other client, with no
# creation-time duplicate (406 is another create-now, launch-later projectile).
sleep 6
lua 2 'coop_test_sniper()'
for i in $(mdk_seq); do
    if [ "$i" = 2 ]; then pattern='^fire type 406 .*launched velocity'
    else pattern='^replay fire type 406 .*created'; fi
    check "game $i sees client sniper fire" wait_log "$i" "$pattern" 5
done
for i in $(mdk_seq); do
    dump "$i" expired
    check "game $i grenade expires" bash -c '! grep -q "^shot type 417 " "$1"' _ "$out/expired_$i.txt"
    check "game $i survived" mdk_alive "$(mdk_pid "$i")"
    check "game $i has no Lua errors" bash -c '! grep -q "LUA ERROR" "$1"' _ "$(mdk_log "$i")"
done
logs=(); for i in $(mdk_seq); do logs+=("$(mdk_log "$i")"); done
check "launch transforms match on every client" python3 - "${logs[@]}" <<'PY'
import collections, re, sys
def launches(path, replay):
    pattern = (r'^replay launch type (417) at (.*?) velocity (.*?) owner' if replay else
               r'^fire type (417) at (.*?) target .* launched velocity (.*?) owner')
    return collections.Counter(re.findall(pattern, open(path).read(), re.M))
host = launches(sys.argv[1], False)
sys.exit(0 if host and all(launches(p, True) == host for p in sys.argv[2:]) else 1)
PY
check "client sniper fire is relayed exactly once" python3 - "${logs[@]}" <<'PY'
import re, sys
logs = [open(p).read() for p in sys.argv[1:]]
sent = len(re.findall(r'^fire type 406 ', logs[1], re.M))
sys.exit(0 if sent > 0 and all(len(re.findall(r'^replay fire type 406 ', log, re.M)) == sent
                             for i, log in enumerate(logs) if i != 1) else 1)
PY
echo "$pass passed, $fail failed; logs: $out"
[ "$fail" -eq 0 ]
