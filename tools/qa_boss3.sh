#!/usr/bin/env bash
# Level 3's boss and a client (level 3 checkpoint 11, two games, about three minutes). The boss picks the place of
# its psionic blast in its own AI, which a client's copy does not run; the client's handler for the shot then raised a
# Lua error at every blast, and some ten errors later the client's Lua refused every call for the rest of the session.
# Both games fight the boss until the client has replayed two of its psionic shots (type 440). Checked from each game's
# log: the client named a target for the blast, no game logged a Lua error, both are alive.
#   MUTANT=nopsitarget   the client is given no target ([test] psitarget=0): must fail with the Lua error
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START="3 11"
case "${MUTANT:-}" in ''|nopsitarget) : ;; *) echo 'Unknown mutant' >&2; exit 2 ;; esac
source "$here/mdk.sh"
"$here/install.sh" >/dev/null || exit 1
if [ "${MUTANT:-}" = nopsitarget ]; then
    for i in 1 2; do printf 'psitarget=0\r\n' >> "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.ini"; done      # the file ends in the [test] section
fi
pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-boss3"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
P1=$(mdk_pid 1); P2=$(mdk_pid 2)
check "both games are in level 3 checkpoint 11" bash -c "grep -qa '^level 3 checkpoint 11 started' '$(mdk_log 1)' && grep -qa '^level 3 checkpoint 11 started' '$(mdk_log 2)'"
shots() { grep -ac '^replay fire type 440 ' "$(mdk_log 2)"; }
# walk, fire and strafe as the sweep does, until the client has replayed two psionic shots (4 minutes at most)
for round in $(seq 1 55); do
    [ "$(shots)" -ge 2 ] && break
    mdk_cmd "$P1" "coop_press(17, 1500)"; mdk_cmd "$P2" "coop_press(17, 1000)"; sleep 1.5
    mdk_cmd "$P1" "coop_press(256, 1200)"; mdk_cmd "$P2" "coop_press(256, 1200)"; sleep 1.2
    mdk_cmd "$P1" "coop_press(32, 800)"; mdk_cmd "$P2" "coop_press(30, 800)"; sleep 0.8
done
sleep 6                                                   # the last shot lands
check "the client replayed two of the boss's psionic shots ($(shots))" test "$(shots)" -ge 2
check "the client named the blast's target ($(grep -a '^lua: boss: psionic target' "$(mdk_log 2)" | tail -1 | tr -d '\r'))" \
    grep -qa '^lua: boss: psionic target l3tp_psitarg0[123]' "$(mdk_log 2)"
for g in 1 2; do
    check "game $g: no Lua error ($(grep -ac 'LUA ERROR' "$(mdk_log "$g")"): $(grep -a 'LUA ERROR' "$(mdk_log "$g")" | sort | uniq -c | head -2 | tr -s ' ' | tr -d '\r' | tr '\n' ';'))" \
        bash -c "! grep -qa 'LUA ERROR' '$(mdk_log "$g")'"
done
mdk_alive "$P1" && mdk_alive "$P2"; alive=$?
check "both games are still running" test "$alive" = 0
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
echo "process evidence: $out"
mdk_crash_report
[ "$fail" -eq 0 ]
