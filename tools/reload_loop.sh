#!/usr/bin/env bash
# Checkpoint reload loop for the AI sight check crash (mdk2hd.exe+0x2a95): one game, tools/reload_loop.py.
#   MDK2_N=1 [MINUTES=40] [LEVEL=2 CP=9] tools/reload_loop.sh [out dir]
# Needs a lease and its environment (eval "$(lab env mdk2)"). Keeps the bridge log and any dump under <out>.
# Exit 97 or 2 = the game crashed (the dump and the crash lines say where); 0 = no crash in MINUTES.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
level="${LEVEL:-2}"; cp="${CP:-9}"
out="${1:-$here/../local/qa/$(date +%Y%m%d-%H%M%S)-reload}"; mkdir -p "$out"
MDK2_BACKGROUND=1 MDK2_START="$level $cp" "$here/install.sh" >/dev/null || exit 1
d="$(mdk_wsl "$(mdk_dir 1)")"
mdk_boot
for i in $(seq 1 90); do grep -q "^level $level checkpoint $cp started" "$d/coop/bridge.log" 2>/dev/null && break; sleep 2; done
sleep 5
rc=0
python3 "$here/reload_loop.py" --dir "$d" --pid "$(mdk_pid 1)" --level "$level" --cp "$cp" --minutes "${MINUTES:-40}" \
    --seed "${SEED:-1}" | tee "$out/loop.txt" || rc=$?
cp "$d/coop/bridge.log" "$out/bridge.log" 2>/dev/null
mdk_crash_scan
mdk_crash_collect "$out/dumps"
mdk_kill
exit "$rc"
