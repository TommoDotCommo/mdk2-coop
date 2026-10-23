#!/usr/bin/env bash
# Level 8's intro movie with two real games: it deletes the level's coneheads by name (Level.ForAllConeheads(omGobDelete)),
# which a client has already removed as the host's enemies. The client crashed there in gob_delete until coop.lua's
# omGobDelete skipped gone gobs. Needs isolated game copies.
#   tools/qa_level8_intro.sh                    (about 3 minutes)
#   MUTANT=gonegob tools/qa_level8_intro.sh     omGobDelete without the check: passes only if the crash is caught
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
out="$here/../local/qa-level8/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
pass=0; fail=0; results=()
cleanup() { local i; for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log" 2>/dev/null || true; done; mdk_kill; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
check() {
    local name=$1; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has() { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i=$1 pattern=$2 deadline=$((SECONDS + $3)); while [ "$SECONDS" -le "$deadline" ]; do has "$i" "$pattern" && return 0; sleep 0.5; done; return 1; }
MDK2_BACKGROUND=1 MDK2_START="8 8" "$here/install.sh" >/dev/null || exit 1
if [ "${MUTANT:-}" = gonegob ]; then
    for i in 1 2; do
        python3 - "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.lua" <<'PY' || exit 2
import sys
path = sys.argv[1]
old, new = 'if (coop_alive(gob)) then coop.delete_orig(gob) end', 'coop.delete_orig(gob)'
data = open(path, newline='').read()
if old not in data:
    sys.exit('mutant: text not found')
open(path, 'w', newline='').write(data.replace(old, new))
PY
    done
fi
mdk_boot
check "host plays the intro" wait_log 1 'movie 1: go' 60
check "client plays the intro" wait_log 2 'movie 1: (go|the host had already said go)' 60
# the coneheads go after the first cut (an 8 s wait and the camera's first animation); the whole intro is about 48 s
check "both see the intro to its end" wait_log 2 'movie 1: (ended here|everyone has seen the end)' 90
for i in 1 2; do check "game $i still running" mdk_alive "$(mdk_pid "$i")"; done
check "no Lua errors" bash -c "! grep -h 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs: $out"
if [ "${MUTANT:-}" = gonegob ]; then
    printf '%s\n' "${results[@]}" | grep -qF "FAIL  game 2 still running" && { echo "mutant gonegob caught"; exit 0; }
    echo "mutant gonegob NOT caught"; exit 1
fi
[ "$fail" = 0 ]
