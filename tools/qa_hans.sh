#!/usr/bin/env bash
# Authored level-one final boss task, native aim and damage; two background copies.
# MUTANT=script-scope, targeting or hostaoe must fail; MUTANT=near (host placed beside the client) must be rejected by the geometry check.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='1 8'
source "$here/mdk.sh"
trap mdk_kill EXIT
"$here/install.sh" >/dev/null
mdk_boot
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-hans-ai"
python3 "$here/check_hans_ai.py" --dirs "$(mdk_wsl "$(mdk_dir 1)")" "$(mdk_wsl "$(mdk_dir 2)")" --output "$out"
