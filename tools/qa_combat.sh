#!/usr/bin/env bash
# T2/T3: three background games, real damage hook, all-process state assertions.
# MUTANT=deaf deliberately drops confirmations on client 2; this suite must fail.
# Requires configured disposable copies and the normal harness prerequisites.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3 MDK2_BACKGROUND=1
source "$here/mdk.sh"
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-combat"
mkdir -p "$out"
trap mdk_kill EXIT
MDK2_START='1 1' "$here/install.sh" >/dev/null
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
# coop_state atomically renames process-produced dumps. No log/screenshot oracles.
snapshot() {
    local tag="$1" i step path
    for i in 1 2 3; do
        path="$(mdk_wsl "$(mdk_dir "$i")")/coop/state_$tag.txt"
        rm -f "$path"
        lua "$i" "coop_state(\"$tag\")"
        for step in $(seq 1 50); do [ -f "$path" ] && break; sleep 0.1; done
        test -f "$path"
        cp "$path" "$out/${tag}_$i.txt"
    done
}
compare() {
    local before="$1" after="$2"; shift 2
    python3 "$here/combatcheck.py" --before "$out/${before}_1.txt" "$out/${before}_2.txt" "$out/${before}_3.txt" \
        --after "$out/${after}_1.txt" "$out/${after}_2.txt" "$out/${after}_3.txt" "$@"
}
mdk_boot
for i in 1 2 3; do lua "$i" 'God(1) coop_combat_reset()'; done
if [ "${MUTANT:-}" = deaf ]; then lua 2 'coop_combat_test(1)'; fi
snapshot before
lua 1 'mdkDealDamage(mdkGetPlayerGob(),r2_dog01,20,DAMAGE_SNIPER,-1)'
sleep 2
snapshot hosthit
compare before hosthit --attacker 1 --target r2_dog01 --expected 20
lua 2 'mdkDealDamage(mdkGetPlayerGob(),r2_dog01,30,DAMAGE_SNIPER,-1)'
sleep 2
snapshot clienthit
compare hosthit clienthit --attacker 2 --target r2_dog01 --expected 30
sleep 9
snapshot idle
compare clienthit idle --idle
# Local keyboard edges, exercised on every process; controls must not repeat while held.
for i in 1 2 3; do lua "$i" 'coop_press(29,500) coop_press(50,250)'; done
sleep 1
snapshot hidden
compare idle hidden --meter 0
for i in 1 2 3; do lua "$i" 'coop_press(29,500) coop_press(50,250)'; done
sleep 1
snapshot shown
compare hidden shown --meter 1
for i in 1 2 3; do lua "$i" 'coop_combat_reset()'; done
snapshot reset
compare shown reset --reset
printf '7 passed, 0 failed\n'
echo "state evidence: $out"
