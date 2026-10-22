#!/usr/bin/env bash
# Actual walking/strafe/jump transforms for Kurt, Max and Doc in two background games.
# A per-frame Lua probe observes motion without teleporting or writing player transforms.
# MDK2_MOTION_MUTANT=1 suppresses the probe's input; the motion assertions must fail.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1
out="$here/../local/qa-motion/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
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
    if "$@" > "$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
wait_since() {
    local i=$1 offset=$2 pattern=$3 deadline=$((SECONDS + $4))
    while [ "$SECONDS" -le "$deadline" ]; do
        tail -n +"$((offset + 1))" "$(mdk_log "$i")" | grep -E "$pattern" >/dev/null && return 0
        sleep 0.1
    done
    return 1
}
mdk_boot
for i in $(mdk_seq); do
    mdk_cmd "$(mdk_pid "$i")" "$(cat "$here/fixtures/motion.lua")" || exit 1
    mdk_cmd "$(mdk_pid "$i")" "coop.SetPreset(\"modern\") coop.qa_motion_no_input = ${MDK2_MOTION_MUTANT:-0} == 1" || exit 1
done
for level in 1 2 3; do
    if [ "$level" != 1 ]; then
        h0=$(wc -l < "$(mdk_log 1)"); c0=$(wc -l < "$(mdk_log 2)")
        mdk_cmd "$(mdk_pid 1)" "mdkNewGame($level, 2)" || exit 1
        check "level $level loaded on host" wait_since 1 "$h0" "^level $level checkpoint 2 started" 40
        check "level $level loaded on client" wait_since 2 "$c0" "^level $level checkpoint 2 started" 40
        sleep 1
    fi
    for specification in idle:-1 forward:17 back:31 left:30 right:32 jump:57; do
        name=${specification%:*}; code=${specification#*:}; tag="l${level}_$name"
        hold=400; duration=1.0
        if [ "$name" = jump ]; then hold=200; duration=1.5; fi
        offsets=(); jobs=()
        for i in $(mdk_seq); do
            offsets[$i]=$(wc -l < "$(mdk_log "$i")")
            mdk_cmd "$(mdk_pid "$i")" "coop.QAMotionStart(\"$tag\", $code, $hold, $duration)" & jobs+=($!)
        done
        for job in "${jobs[@]}"; do wait "$job" || exit 1; done
        for i in $(mdk_seq); do check "game $i sampled $tag" wait_since "$i" "${offsets[$i]}" "^lua: qa-motion $tag type " 5; done
    done
    sleep 0.3
    check "worlds match after level $level movement" mdk_state "motion_$level" "$out"
done
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
check "every character actually walks, strafes and jumps in both games" python3 -B "$here/motion_check.py" "$out/game1.log" "$out/game2.log"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; logs and states: $out"
[ "$fail" = 0 ]
