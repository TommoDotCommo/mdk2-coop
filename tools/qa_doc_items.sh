#!/usr/bin/env bash
# Doc's items given at a checkpoint start (coop.CheckpointItems, level 3 checkpoint 6: atomic toaster, duct tape, pipes,
# and the loaf through a pickup) are drawn stacked above the hands, in both games. Given in the level's first frames,
# the inventory left some in its centre slot: slot1 at the HUD's origin, the middle of the screen, for good (the owner
# saw the toaster float over Doc's head and the tape and loaf mid-screen). Read from each game: the gobs below the
# docinventory HUD scene (coop_children), every item slot beside a hand (|x| at least 1) and at least three of them.
#   MUTANT=nodelay   the items are given at once ([test] grantdelay=0): must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about a minute.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START="3 6"
case "${MUTANT:-}" in ''|nodelay) : ;; *) echo 'Unknown mutant' >&2; exit 2 ;; esac
source "$here/mdk.sh"
"$here/install.sh" >/dev/null || exit 1
if [ "${MUTANT:-}" = nodelay ]; then
    for i in 1 2; do printf 'grantdelay=0\r\n' >> "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.ini"; done      # the file ends in the [test] section
fi
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-docitems"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
check "both games are in level 3 checkpoint 6" bash -c "grep -qa '^level 3 checkpoint 6 started' '$(mdk_log 1)' && grep -qa '^level 3 checkpoint 6 started' '$(mdk_log 2)'"
for g in 1 2; do
    for t in $(seq 1 40); do grep -qa 'checkpoint start: [0-9]* items given to Doc' "$(mdk_log "$g")" && break; sleep .5; done
done
sleep 3
for g in 1 2; do
    check "game $g: Doc's checkpoint items were given" grep -qa 'checkpoint start: [0-9]* items given to Doc' "$(mdk_log "$g")"
    n=$(grep -ac "" "$(mdk_log "$g")")
    mdk_cmd "$(mdk_pid "$g")" 'coop_children(docinventory)'
    for t in $(seq 1 20); do tail -n +"$n" "$(mdk_log "$g")" | grep -qa '^children: ' && break; sleep .25; done
    tail -n +"$n" "$(mdk_log "$g")" | grep -a '^child' | tr -d '\r' > "$out/hud_$g.txt"
    check "game $g: every item slot is beside a hand, none in the middle of the screen (slots: $(awk '$2 == 2 && $3 ~ /^slot/ {printf "%s x %s; ", $3, $7}' "$out/hud_$g.txt"))" \
        python3 - "$out/hud_$g.txt" <<'PY'
import sys
slots = [l.split() for l in open(sys.argv[1]) if l.split()[1] == '2' and l.split()[2].startswith('slot')]
print(slots)
sys.exit(0 if len(slots) >= 3 and all(abs(float(s[6])) >= 1 for s in slots) else 1)
PY
done
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
echo "process evidence: $out"
mdk_crash_report
[ "$fail" -eq 0 ]
