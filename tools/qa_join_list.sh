#!/usr/bin/env bash
# Joining without typing an address (launcher Join side, bridge/launcher_join.c), two games in background
# test mode with the launchers hidden and driven through coop\launcher.txt ([test] launcher=1): the host opens
# its lobby; the joiner, with no address in its settings, finds it in "Games near you" (directed broadcast on
# every adapter), joins from the list, is remembered as a recent host, leaves, joins again from a pasted invite
# code (two routes, the LAN one kept), sees "Rejoin <name>" on Play and is offered a clipboard invite once.
# Every assertion reads the launcher's own state dump (tools/check_lobby_live.py join_list).
#   tools/qa_join_list.sh
# The scan itself is proven game-free with mutants (tools/test_launcher_join.py: broadcast only to
# 255.255.255.255 fails the two-adapter model). Game copies: see tools/mdk.sh. About 1 minute.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-join-list"; mkdir -p "$out"
check() {
    local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
PLAY="click 1000 660"

names=(Hosty Alice)
for i in 1 2; do
    setini $i test launcher 1
    setini $i launcher show 1
    setini $i launcher lobby 1
    printf '[player]\r\nid=0000000%d\r\nname=%s\r\n' "$i" "${names[$((i - 1))]}" > "$(dir $i)/coop/player.ini"
    rm -f "$(dir $i)/coop/launcher.txt"
    for f in profiles.tsv recents.tsv; do      # a fresh store: archive what an earlier run left
        if [ -f "$(dir $i)/coop/$f" ]; then mv "$(dir $i)/coop/$f" "$out/player$i-$f"; fi
    done
done
setini 2 net peer_host ""                      # nothing typed: the joiner has no address at all
check "joiner has no host address configured" bash -c "! grep -qE '^peer_host=.+' '$(dir 2)/coop/coop.ini'"
mdk_kill; sleep 2
: > "$MDK2_PIDS"
for i in 1 2; do mdk_start "$i" >> "$MDK2_PIDS"; sleep 2; done
for i in 1 2; do wait_log "$i" "^launcher: shown" 60 || true; done
lcmd 1 "$PLAY"
check "host: lobby open as host"               wait_log 1 "^lobby: open as host" 10
lcmd 1 dump
live_version=$(sed -n 's/^version=//p' "$(dir 1)/coop/launcher-state.ini" | head -1 | tr -d '\r')
check "join list, list join, recents, invite join, rejoin, clipboard (launcher state)" \
    python3 "$here/check_lobby_live.py" join_list --dirs "$(dir 1)" "$(dir 2)" \
        --output "$out/live" --port "$MDK2_PORT" --version "$live_version"
mdk_crash_scan
check "joiner remembered the host by itself"   wait_log 2 "^join: remembered Hosty \(lan\)" 1
check "no clipboard text in the log"           bash -c "! grep -q 'tommodotcommo.github.io' '$(mdk_log 2)'"

for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs, state dumps and screenshots: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
