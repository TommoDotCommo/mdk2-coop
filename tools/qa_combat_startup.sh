#!/usr/bin/env bash
# T1: one background game; exercise registration/dumps/reset without remote players.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1 MDK2_BACKGROUND=1
source "$here/mdk.sh"
trap mdk_kill EXIT
MDK2_START='1 1' "$here/install.sh" >/dev/null
mdk_boot
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-combat-startup"
mkdir -p "$out"
path="$(mdk_wsl "$(mdk_dir 1)")/coop/state_combat_startup.txt"
rm -f "$path"
mdk_cmd "$(mdk_pid 1)" 'coop_combat_reset() coop_combat_dump() coop_state("combat_startup")'
for step in $(seq 1 50); do [ -f "$path" ] && break; sleep 0.1; done
cp "$path" "$out/state.txt"
python3 - "$here" "$out/state.txt" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from combatcheck import load
s = load(sys.argv[2])
assert s['size'] == 0 and s['meter'] == 0  # the meter starts collapsed
assert len(s['players']) == 16 and all(v == (0, 0, 0., 0.) for v in s['players'].values())
print('PASS one-process combat registration, dump and reset state')
PY
printf '1 passed, 0 failed\n'
