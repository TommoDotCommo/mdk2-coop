#!/usr/bin/env bash
# Level-one boss ring turrets (scenery guns) fire at and hurt a client far from the host; two background copies.
# MUTANT=targeting or hostaoe must fail; MUTANT=near (host placed beside the client) must be rejected by the geometry check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='1 8'
source "$here/mdk.sh"
trap mdk_kill EXIT
"$here/install.sh" >/dev/null
mdk_boot
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-boss-turrets"
python3 "$here/check_boss_turrets.py" --dirs "$(mdk_wsl "$(mdk_dir 1)")" "$(mdk_wsl "$(mdk_dir 2)")" --output "$out"
