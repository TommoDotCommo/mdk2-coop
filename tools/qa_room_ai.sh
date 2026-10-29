#!/usr/bin/env bash
# Native level-one generator, circular enemies, turret targeting and duplicate-name damage.
# Uses two disposable background copies, with no injected AI target, alert or enemy.
# After each stage's setup the host waits in another room and the geometry rule is asserted from process state.
# MUTANT=targeting or hostaoe must fail; MUTANT=near (host beside the client) must be rejected by the geometry check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='1 2'
source "$here/mdk.sh"
trap mdk_kill EXIT
"$here/install.sh" >/dev/null
mdk_boot
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-room-ai"
python3 "$here/check_room_ai.py" --dirs "$(mdk_wsl "$(mdk_dir 1)")" "$(mdk_wsl "$(mdk_dir 2)")" --output "$out"
