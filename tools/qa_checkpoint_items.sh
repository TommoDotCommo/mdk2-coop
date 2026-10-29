#!/usr/bin/env bash
# A new game at a checkpoint past the level 2 jetpack gives each player the jetpack; two background copies.
# MUTANT=nogrant (no checkpoint items) must fail.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='2 8'
source "$here/mdk.sh"
trap mdk_kill EXIT
"$here/install.sh" >/dev/null
mdk_boot
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-checkpoint-items"
python3 "$here/check_checkpoint_items.py" --dirs "$(mdk_wsl "$(mdk_dir 1)")" "$(mdk_wsl "$(mdk_dir 2)")" --output "$out"
