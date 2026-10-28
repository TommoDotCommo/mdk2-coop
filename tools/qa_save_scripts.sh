#!/usr/bin/env bash
# Two real games at level 2 checkpoint 7: native attack tasks must survive an
# instant-save/resume. MUTANT=unresolved removes the native function's global
# resolver name and must fail the task checks.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="2 7" "$here/install.sh" >/dev/null || exit 1

out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-save-scripts"; mkdir -p "$out"
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
    local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
q() {
    local tag="Q$RANDOM$RANDOM"
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    wait_log "$1" "^lua: $tag " 10 >/dev/null
    grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'
}
wait_q() { local t; for t in $(seq 1 "$4"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }

mdk_boot
sleep 2
if [ "${MUTANT:-}" = unresolved ]; then
    for i in 1 2; do lua "$i" 'coopNativeDoganboyAttack=nil'; done
fi
for i in 1 2; do
    check "game $i loaded level 2 checkpoint 7" wait_log "$i" '^level 2 checkpoint 7 started' 5
    check "game $i native attack task has a resolver" \
        wait_q "$i" 'FindGlobalFunc(l2l_poopsy1.stack.script[1][1])' coopNativeDoganboyAttack 5
done
lua 1 'coop.SaveNow()'
sleep 3
lua 1 'coop.ResumeLatest()'
for i in 1 2; do
    check "game $i resumed the instant save" wait_log "$i" 'co-op save [0-9]+ loaded' 20
    check "game $i restored the Poopsy attack task" \
        wait_q "$i" 'type(l2l_poopsy1.stack.script[1][1])' function 10
    check "game $i restored the Bif attack task" \
        wait_q "$i" 'type(l2r8bif.stack.script[1][1])' function 10
done
check "no Lua errors" bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
printf '%s\n' "${results[@]}"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
