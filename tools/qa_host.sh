#!/usr/bin/env bash
# Hosting that explains itself, with two games in background test mode and the launchers hidden, driven
# through coop\launcher.txt ([test] launcher=1, see bridge/launcher.c). Everything is read back from the
# launchers' own state dumps (coop\launcher-state.ini), not from screenshots:
#   1. the host opens a lobby: "Same network" answers on this PC's LAN address, the router is asked to open
#      the port and what it said is recorded, the firewall rules are read; leaving removes the mapping
#   2. the host reopens (router port off, so the game started at the end leaves nothing on the router), the
#      client joins with the invite code only (no address typed), and the lobby key went with its hello
#   3. invite only: the client joining by address without the code is refused with the invite-only words;
#      with the code it gets in; Kick removes it with "The host removed you" and it stays out of that lobby
#   4. Mute me sends no voice (the host receives none), Unmute sends again
#   5. the pad: LB/RB change tabs, the D-pad walks the focus to "Start game", A starts the countdown
#   6. that last lobby is invite only: the game it starts lets the client's game in by the invite's key (net.c)
#      before any of its player packets, also when the guest's game is up first (the key goes ahead of each one)
#   tools/qa_host.sh            about 2 minutes, 2 game copies (see tools/mdk.sh)
#   MUTANT=latekey              the guest's key comes 3 s late ([test] late_key=1): the host refuses its first player
#                               packet and the last check must fail
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
case "${MUTANT:-}" in ''|latekey) : ;; *) echo 'Unknown mutant' >&2; exit 2 ;; esac
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null
if [ "${MUTANT:-}" = latekey ]; then
    sed -i '0,/^\[test\]/s//[test]\nlate_key=1/' "$(mdk_wsl "$(mdk_dir 2)")/coop/coop.ini"   # inside the existing section
fi

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-host"; mkdir -p "$out"
check() {
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
state() { lcmd "$1" dump >/dev/null && sleep 0.3; cat "$(dir "$1")/coop/launcher-state.ini" | tr -d '\r'; }
value() { state "$1" | sed -n "s/^$2=//p" | head -1; }
state_is() {
    local player="$1"; shift
    lcmd "$player" dump || return 1
    python3 "$here/check_launcher_state.py" "$(dir "$player")/coop/launcher-state.ini" "$@"
}
wait_state() {  # wait_state <player> <seconds> key=value...
    local player="$1" secs="$2" i; shift 2
    for i in $(seq 1 "$secs"); do state_is "$player" "$@" && return 0; sleep 1; done; return 1
}
shot()  { lcmd "$1" "shot host_$2" && python3 "$here/tga2png.py" "$(dir "$1")/coop/host_$2.tga" "$out/$2.png"; }
PLAY="click 1000 660"

names=(Hosty Alice)
for i in 1 2; do
    setini $i test launcher 1
    setini $i launcher show 1
    setini $i launcher lobby 1
    setini $i network invite_only 0
    setini $i network upnp 1
    printf '[player]\r\nid=0000000%d\r\nname=%s\r\n' "$i" "${names[$((i - 1))]}" > "$(dir $i)/coop/player.ini"
    rm -f "$(dir $i)/coop/launcher.txt"
done
mdk_kill; sleep 2
: > "$MDK2_PIDS"
for i in 1 2; do mdk_start "$i" >> "$MDK2_PIDS"; sleep 2; done
for i in 1 2; do wait_log "$i" "^launcher: shown" 60 || true; done

# 1. the checks, with the router port on
lcmd 1 "$PLAY"
check "host: lobby open"                               wait_log 1 "^lobby: open as host" 10
check "host: same network answers on the LAN address"  wait_log 1 "^host: same network: [1-9][0-9]* address\(es\), lobby answers" 15
check "host: firewall rules read"                      wait_log 1 "^host: firewall: read=1" 20
check "host: router asked and its answer recorded"     wait_log 1 "^host: router: " 30
check "host: state: Same network ok"                   wait_state 1 5 host.lan_probe=1 host.open=1
state 1 > "$out/host-checks.ini"
grep -E '^(router_|row|firewall_|tailscale)' "$out/host-checks.ini" | sed 's/^/  /' > "$out/router-said.txt"
shot 1 lobby_checks
mapped=0; grep -q '^router_state=2' "$out/host-checks.ini" && mapped=1
lcmd 1 "key 27"
check "host: left the lobby"                           wait_state 1 10 launcher.lobby=0
if [ "$mapped" = 1 ]; then
    check "host: leaving removed the router mapping"   wait_log 1 "^host: router mapping of UDP [0-9]+ removed" 15
fi

# 2. the invite code only
lcmd 1 "upnp 0" "$PLAY"
check "host: lobby reopened"                           wait_state 1 10 launcher.lobby=1 host.open=1
sleep 3
code=$(value 1 invite_code)
echo "$code" > "$out/invite-code.txt"
check "host: an invite code"                           test -n "$code"
lcmd 2 "join $code" "$PLAY"
check "client: joined by invite with the lobby key"    wait_log 2 "^lobby: joining by invite .*with the lobby key" 10
check "client: connected with the code only"           wait_state 2 15 launcher.connected=1 launcher.has_key=1
check "host: two players"                              wait_state 1 10 launcher.players=2
lan=$(value 2 host_addr)
lcmd 2 "key 27"
check "client: left"                                   wait_state 2 10 launcher.lobby=0
check "host: client gone"                              wait_state 1 10 launcher.players=1

# 3. invite only
lcmd 1 "visibility 1"
sleep 1
code=$(value 1 invite_code)
lcmd 2 "join $lan" "$PLAY"
check "invite only: address without the code refused"  wait_state 2 10 launcher.refused=2 launcher.connected=0
check "invite only: the host refused it"               wait_log 1 "^lobby: refused Alice .*invite only" 10
check "invite only: host still alone"                  state_is 1 launcher.players=1
shot 2 refused
lcmd 2 "key 27"; sleep 1
lcmd 2 "join $code" "$PLAY"
check "invite only: the code gets in"                  wait_state 2 15 launcher.connected=1 launcher.has_key=1 launcher.refused=0
lcmd 1 "kick 1"
check "kick: client told it was removed"               wait_state 2 10 launcher.refused=1
check "kick: host back to one player"                  wait_state 1 5 launcher.players=1
lcmd 2 "key 27"; sleep 1
lcmd 2 "$PLAY"
check "kick: refused again this session"               wait_state 2 10 launcher.refused=1
lcmd 2 "key 27"; sleep 1
# the ban lasts as long as this lobby: the host leaves, opens a new one (invite only again) and Alice joins with
# its new invite; the game this lobby starts checks the same key
lcmd 1 "key 27"; sleep 1
lcmd 1 "$PLAY"
check "host: a new lobby"                              wait_state 1 10 launcher.lobby=1 launcher.players=1
sleep 3
code=$(value 1 invite_code)
lcmd 2 "join $code"

# 4. and 5. Mute me, then the pad: tabs, the focus to Start, A starts
lcmd 1 "pad 0x0200 0 0" "pad 0 0 0"
check "pad: RB to Saves"                               state_is 1 launcher.page=1 launcher.pad_last=1
lcmd 1 "pad 0x0100 0 0" "pad 0 0 0"
check "pad: LB back to the Lobby"                      state_is 1 launcher.page=0
shot 1 pad_lobby
check "host: Start waits while nobody joined"          state_is 1 launcher.countdown=0
lcmd 2 "$PLAY"
check "client: joins the host's new lobby"            wait_state 2 15 launcher.connected=1
# D4: Mute me sends no voice; unmuted, the host hears the client again
lcmd 2 "press Mute me" "voice 600"; sleep 2
check "mute me: set on the client"                     state_is 2 launcher.mute_me=1
check "mute me: the host received no voice"            state_is 1 player1.voice_frames=0
lcmd 2 "press Unmute me" "voice 600"; sleep 2
check "unmuted: the host hears the client"             state_is 1 "player1.voice_frames>=5"
lcmd 2 "key 13"
check "host: client ready"                             wait_state 1 10 player1.ready=1
reached=0
for i in $(seq 1 30); do
    lcmd 1 "pad 0x0002 0 0" "pad 0 0 0"
    if [ "$(value 1 focus_label)" = "Start game" ]; then reached=1; break; fi
done
check "pad: the D-pad reaches Start game"              test "$reached" = 1
shot 1 pad_start
lcmd 1 "pad 0x1000 0 0" "pad 0 0 0"
check "pad: A on Start starts the countdown"           wait_log 1 "^lobby: host started the countdown \(2 players\)" 10
check "host: game started"                             wait_log 1 "^launcher: play, host over UDP" 15
check "client: game started"                           wait_log 2 "^launcher: play, join over UDP" 15
check "client joined the game"                         wait_log 2 "^we are player [0-9]+" 60
check "game: invite only, the client's game let in by its key" wait_log 1 "^invite only: .* has the invite" 5
check "game: the key came before any player packet"   bash -c "! grep -q '^invite only: refused' '$(mdk_log 1)'"

for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
cat "$out/router-said.txt"
echo "logs, states and screenshots: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
