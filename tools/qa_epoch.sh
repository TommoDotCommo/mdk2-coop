#!/usr/bin/env bash
# Two real games: a new world at the same checkpoint must reload the client, while a
# same-epoch reconnect must retain its world. All commands run in background mode.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1
out="$here/../local/qa-epoch/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
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
wait_since() {
    local i=$1 offset=$2 pattern=$3 seconds=$4 deadline=$((SECONDS + $4))
    while [ "$SECONDS" -le "$deadline" ]; do
        tail -n +"$((offset + 1))" "$(mdk_log "$i")" | grep -E "$pattern" >/dev/null && return 0
        sleep 0.2
    done
    return 1
}
world() {
    last_tag=$1
    mdk_state "$last_tag" "$out" > "$out/report_$last_tag.txt" 2>&1 && return 0
    sleep 1.5
    last_tag="${1}_retry"
    mdk_state "$last_tag" "$out" > "$out/report_$last_tag.txt" 2>&1
}
fixture() {
    python3 -B - "$here" "$out" "$last_tag" "$1" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
import statecheck
out, tag, phase = Path(sys.argv[2]), sys.argv[3], sys.argv[4]
for name in ('host', 'client'):
    state = statecheck.load(out / f'state_{tag}_{name}.txt')
    dogs = sum(e[3] == 'coop_dog1' for e in state['ent'].values())
    decoys = state['item'].get('l1_r3dummy', 0)
    expected = (1, 0) if phase == 'changed' else (0, 1)
    if (dogs, decoys) != expected:
        raise SystemExit(f'FAIL {name}: {dogs} test dogs, {decoys} decoys; expected {expected}')
print('PASS world fixture: ' + phase)
PY
}
mdk_boot
check "initial worlds match" world initial
check "initial fixture has its pickup and no test spawn" fixture fresh
h0=$(wc -l < "$(mdk_log 1)"); c0=$(wc -l < "$(mdk_log 2)")
lua 1 'coop_press(63, 80)'
check "client sees the host test spawn" wait_since 2 "$c0" '^ent bind .* -> coop_dog1' 5
lua 2 'coop.tpnext = 1'
lua 2 'coop_press(62, 80)'
check "client actually picks up the decoy" wait_since 2 "$c0" '^pickup l1_r3dummy' 5
check "host receives the pickup" wait_since 1 "$h0" 'peer picked up l1_r3dummy: removed here' 5
check "changed worlds match" world changed
check "both worlds changed before reset" fixture changed

# Reloading the very same checkpoint is a new epoch, not a continuation of this world.
h0=$(wc -l < "$(mdk_log 1)"); c0=$(wc -l < "$(mdk_log 2)")
lua 1 'mdkNewGame(1, 2)'
check "host actually reloads the same checkpoint" wait_since 1 "$h0" '^level 1 checkpoint 2 started' 15
check "client actually reloads the same checkpoint" wait_since 2 "$c0" '^level 1 checkpoint 2 started' 15
check "worlds match after same-checkpoint reset" world reset
check "old test spawn is gone and pickup restored on both" fixture fresh

# A transient link outage in that epoch is different: do not discard the restored world.
h0=$(wc -l < "$(mdk_log 1)"); c0=$(wc -l < "$(mdk_log 2)")
lua 2 'coop_netblock(12000)'
check "host observes the outage" wait_since 1 "$h0" 'player 2 left' 15
check "host observes the same client rejoin" wait_since 1 "$h0" 'player 2 joined' 15
check "client continues its existing epoch" wait_since 2 "$c0" 'we are still there' 5
check "reconnect does not reload the client" bash -c "! tail -n +$((c0 + 1)) '$(mdk_log 2)' | grep -q '^level '"
check "worlds match after same-epoch reconnect" world rejoined
check "restored fixture survives reconnect" fixture fresh
for i in $(mdk_seq); do check "game $i still running" mdk_alive "$(mdk_pid "$i")"; done
check "no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs and states: $out"
[ "$fail" = 0 ]
