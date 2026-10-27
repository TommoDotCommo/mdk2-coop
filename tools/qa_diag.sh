#!/usr/bin/env bash
# Diagnostics with two real games in background test mode (bridge/diag.c, dist/diag.ps1, tools/diag_report.py):
# link quality is logged ("netq:"), a quiet game raises no desync (the false-positive gate), F2 (the mark) on the client
# makes both games write a flight recording of the same moment, a host that reports a wrong enemy count makes
# the client log which field differs and both dump (and a deaf client does not), every session log's hash
# chain is intact, and the bundle script zips the lot without names or addresses and the report reads it.
#   MDK2_N=2 tools/qa_diag.sh
#   MUTANT=deaf tools/qa_diag.sh      the client ignores the host's report: the desync checks must fail
# Game copies: see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-diag"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
has()  { grep -qE "$2" "$(mdk_log "$1")"; }
lua()  { mdk_cmd "$(mdk_pid "$1")" "$2"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
coop() { dirname "$(mdk_log "$1")"; }
flights() { ls "$(coop "$1")"/logs/*.flight-*-"$2".txt 2>/dev/null; }
marker() { grep -aoE "^lua: MARK [0-9]+" "$(mdk_log "$1")" | head -1 | grep -oE "[0-9]+"; }
trap 'mdk_kill' EXIT

mdk_kill; sleep 1
for i in 1 2; do rm -rf "$(coop "$i")/logs"; done      # the copies keep earlier runs' files: only this run's count
mdk_boot
[ "${MUTANT:-}" = deaf ] && lua 2 'coop_diag_test(0, 1)'

check "link quality is logged on the host"       wait_log 1 "^netq: player 2 rtt [0-9]+ ms, jitter" 30
check "link quality is logged on the client"     wait_log 2 "^netq: player 1 rtt [0-9]+ ms, jitter" 30
sleep 15
check "a quiet game raises no desync (host)"     bash -c "! grep -qE '^DESYNC' '$(mdk_log 1)'"
check "a quiet game raises no desync (client)"   bash -c "! grep -qE '^DESYNC' '$(mdk_log 2)'"

# F2 on the client (the existing mark): both games save the same moment
lua 2 'coop_press(60, 80)'
check "the client marks"                         wait_log 2 "^lua: MARK [0-9]+ at" 10
check "the host got the mark"                    wait_log 1 "^lua: MARK [0-9]+ \(from player 2\)" 10
check "the client's flight recording is there"   test -n "$(flights 2 mark)"
check "the host's flight recording is there"     test -n "$(flights 1 markp2)"
check "the same mark number on both"             test -n "$(marker 2)" -a "$(marker 2)" = "$(marker 1)"
check "the recording has samples and the log"    bash -c "grep -q '^S ' $(flights 2 mark | head -1) && grep -q '^L ' $(flights 2 mark | head -1)"
check "it holds the other player's round trip"   bash -c "grep -E '^S ' $(flights 2 mark | tail -1) | tail -1 | grep -qE '\| .*2 [0-9]+ [0-9.]+ [0-9.]+ [0-9] '"

# a host that reports the wrong enemy count
lua 1 'coop_diag_test(7, 0)'
check "the client names the field that differs"  wait_log 2 "^DESYNC enemies: host has [0-9]+ .*, we have [0-9]+" 20
check "the host is told"                         wait_log 1 "^DESYNC reported: player 2 sees enemies differently" 20
check "the client dumped a flight recording"     test -n "$(flights 2 desync)"
check "the host dumped one too"                  test -n "$(flights 1 report)"
lua 1 'coop_diag_test(0, 0)'
check "it clears when they agree again"          wait_log 2 "^desync cleared: enemies agrees again" 10

# every session log is whole
for i in 1 2; do cp -r "$(coop "$i")/logs" "$out/logs$i" 2>/dev/null; cp "$(mdk_log "$i")" "$out/game$i.log"; done
check "host's hash chain is intact"              python3 "$here/diag_report.py" --raw "$(coop 1)"
check "client's hash chain is intact"            python3 "$here/diag_report.py" --raw "$(coop 2)"

# the bundle
for i in 1 2; do
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/../dist/diag.ps1")" \
        -CoopDir "$(mdk_dir "$i")\\coop" -OutDir "$(wslpath -w "$out")" -Version qa -NoOpen >/dev/null 2>&1
    mv "$out"/mdk2-coop-diag_*.zip "$out/bundle$i.zip" 2>/dev/null
done
check "both bundles were made"                   test -s "$out/bundle1.zip" -a -s "$out/bundle2.zip"
check "the report reads the pair and finds the shared mark" bash -c "python3 '$here/diag_report.py' '$out/bundle1.zip' '$out/bundle2.zip' --timeline '$out/timeline.txt' | grep -qE '^mark [0-9]+ pressed on'"
check "the timeline has both machines"           bash -c "grep -c . '$out/timeline.txt' | grep -qE '^[1-9][0-9][0-9]' && grep -q bundle1 '$out/timeline.txt' && grep -q bundle2 '$out/timeline.txt'"
names_in() { local z f n; z=$1; shift
    for n in "$@"; do [ "${#n}" -gt 2 ] || continue
        for f in $(unzip -Z1 "$z"); do unzip -p "$z" "$f" | grep -aqiF "$n" && return 0; done; done; return 1; }
winnames=$(powershell.exe -NoProfile -Command '$env:USERNAME; $env:COMPUTERNAME' | tr -d '\r' | tr '\n' ' ')
check "no Windows or computer name in a bundle"  bash -c "$(declare -f names_in); ! names_in '$out/bundle1.zip' $winnames"
check "the manifest says the logs were whole"    bash -c "unzip -p '$out/bundle1.zip' manifest.json | python3 -c 'import json,sys; m=json.load(sys.stdin); c=[f[\"chain\"][\"status\"] for f in m[\"files\"] if \"chain\" in f]; sys.exit(0 if c and set(c)=={\"ok\"} else 1)'"

mdk_kill
printf '%s\n' "${results[@]}"
echo "logs and bundles: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
