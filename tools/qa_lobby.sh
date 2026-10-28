#!/usr/bin/env bash
# The launcher's lobby with three games in background test mode, the launchers hidden and driven through
# coop\launcher.txt ([test] launcher=1, see bridge/launcher.c): the host opens its lobby, two clients join
# it over UDP, everyone is listed with their name, ready state and ping, a client's Ready shows on the
# host, the host's Start counts down on every launcher, and then all three games start and connect.
#   tools/qa_lobby.sh
#   MDK2_EXTENDED=1 tools/qa_lobby.sh  profile controls/probes, full lobby with synthetic peers,
#                                     host loss/reconnect, hostile/flood chat and speaking indicators
#   MUTANT=nostart tools/qa_lobby.sh    the host never sends the countdown: passes only if the start check fails
# Game copies: see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-lobby"; mkdir -p "$out"
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
shot()  { lcmd "$1" "shot lobby_$2" && python3 "$here/tga2png.py" "$(dir "$1")/coop/lobby_$2.tga" "$out/$2.png"; }
state_is() {
    local player="$1"; shift
    lcmd "$player" dump || return 1
    python3 "$here/check_launcher_state.py" "$(dir "$player")/coop/launcher-state.ini" "$@"
}
PLAY="click 1000 660"

names=(Hosty Alice Bob)
for i in 1 2 3; do
    setini $i test launcher 1
    setini $i launcher show 1
    setini $i launcher lobby 1
    printf '[player]\r\nid=0000000%d\r\nname=%s\r\n' "$i" "${names[$((i - 1))]}" > "$(dir $i)/coop/player.ini"
    rm -f "$(dir $i)/coop/launcher.txt"
    if [ "${MDK2_EXTENDED:-0}" = 1 ]; then
        # Archive disposable-copy profile state before seeding a fresh UI fixture.
        for f in profiles.tsv profiles-export.tsv profiles-import.tsv; do
            if [ -f "$(dir $i)/coop/$f" ]; then
                cp "$(dir $i)/coop/$f" "$out/player$i-$f"
                rm "$(dir $i)/coop/$f"
            fi
        done
    fi
done
mdk_kill; sleep 2
: > "$MDK2_PIDS"
for i in 1 2 3; do mdk_start "$i" >> "$MDK2_PIDS"; sleep 2; done
for i in 1 2 3; do wait_log "$i" "^launcher: shown" 60 || true; done

# the host opens its lobby, then the clients join it
lcmd 1 "$PLAY"
check "host: lobby open"                       wait_log 1 "^lobby: open as host" 10
lcmd 2 "$PLAY"; lcmd 3 "$PLAY"
check "client 2: connected to the host"        wait_log 2 "^lobby: connected to the host" 15
check "client 3: connected to the host"        wait_log 3 "^lobby: connected to the host" 15
check "host: Alice joined"                     wait_log 1 "^lobby: Alice joined from 127.0.0.1" 10
check "host: Bob joined"                       wait_log 1 "^lobby: Bob joined from 127.0.0.1" 10
sleep 1; shot 1 host_waiting; shot 2 client_waiting
live_check() {
    python3 "$here/check_lobby_live.py" "$1" --dirs "$(dir 1)" "$(dir 2)" "$(dir 3)" \
        --output "$out/extended" --port "$MDK2_PORT" --version "$live_version"
}
if [ "${MDK2_EXTENDED:-0}" = 1 ]; then
    lcmd 1 dump
    live_version=$(sed -n 's/^version=//p' "$(dir 1)/coop/launcher-state.ini" | head -1 | tr -d '\r')
    live_check profiles || exit 1
    i686-w64-mingw32-gcc -O2 -Wall -Wextra -o "$out/lobby_peers.exe" "$here/../bridge/tests/launcher_lobby_peers.c" -lws2_32 || exit 1
    "$out/lobby_peers.exe" "$MDK2_PORT" 13 "$live_version" 10000 &
    peer_pid=$!
    live_check full; full_result=$?
    wait "$peer_pid" || exit 1
    [ "$full_result" = 0 ] || exit 1
    sleep 5
    live_check lifecycle || exit 1
fi
# Browse every other page while the session remains connected. Read process state.
for p in 1 2 3 4 5; do
    lcmd 2 "page $p"
    sleep 1
    check "client 2: page $p preserves session" state_is 2 "launcher.page=$p" launcher.lobby=1 launcher.connected=1 launcher.players=3 launcher.phase=2
    check "host: client remains in roster while browsing $p" state_is 1 launcher.players=3 launcher.phase=2
done
lcmd 2 "page 0"

# Pregame text chat: Alice sends message, host and Bob receive it
lcmd 2 "say Hello lobby"
sleep 1
check "host: received Alice's chat" state_is 1 launcher.chat_total=1 chat0.name=Alice "chat0.text=Hello lobby"
check "client 3: received Alice's chat" state_is 3 launcher.chat_total=1 chat0.name=Alice "chat0.text=Hello lobby"
alice_index=$(python3 - "$(dir 1)/coop/launcher-state.ini" <<'PY'
import configparser, sys
s = configparser.ConfigParser(interpolation=None); s.read(sys.argv[1])
print(s.getint('chat0', 'color', fallback=1))
PY
)

# Pregame voice: Alice sends simulated tone, host receives frames, mutes and adjusts volume
lcmd 2 "voice 400"
sleep 1
check "host: received Alice voice frames" state_is 1 "player$alice_index.voice_frames>=5"
lcmd 1 "mute $alice_index"
sleep 0.5
check "host: muted Alice" state_is 1 "player$alice_index.muted=1"
lcmd 1 "vol $alice_index 150"
sleep 0.5
check "host: scaled Alice volume" state_is 1 "player$alice_index.volume=150"
if [ "${MDK2_EXTENDED:-0}" = 1 ]; then live_check chat || exit 1; fi
# Alice is ready: the host's roster shows it
lcmd 2 "key 13"; sleep 2; shot 1 host_alice_ready
check "client 2 process ready" state_is 2 launcher.ready=1
check "host process sees Alice ready" state_is 1 "player$alice_index.ready=1"
check "client 2 sees three players"            wait_log 2 "^lobby: connected to the host \(3 players\)" 5

# nobody has started yet: no game
check "no game before Start"                   bash -c "! grep -q '^launcher: play' '$(mdk_log 1)'"

if [ "${MUTANT:-}" = nostart ]; then
    # Withhold the host's start and verify the very same handoff oracle rejects
    # the resulting state. Do not spend minutes waiting for absent game logs.
    sleep 5
    check "nostart: host remains in lobby" state_is 1 launcher.phase=2 launcher.countdown=0
    check "nostart: client remains in lobby" state_is 2 launcher.phase=2 launcher.countdown=0
    check "nostart: game-start oracle rejects withheld start" bash -c \
        '! python3 "$1" "$2" launcher.phase=5' _ "$here/check_launcher_state.py" "$(dir 2)/coop/launcher-state.ini"
    for i in 1 2 3; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
    mdk_kill
    printf '%s\n' "${results[@]}"
    echo "logs and screenshots: $out"
    echo "$pass passed, $fail failed (nostart mutant detected)"
    [ "$fail" -eq 0 ]; exit $?
else
    lcmd 1 "key 13"
fi
check "host: countdown started"                wait_log 1 "^lobby: host started the countdown \(3 players\)" 10
check "host: start"                            wait_log 1 "^lobby: start \(3 players\)" 15
check "client 2: start"                        wait_log 2 "^lobby: start" 15
check "client 3: start"                        wait_log 3 "^lobby: start" 15
check "host: game started as host"             wait_log 1 "^launcher: play, host over UDP" 15
check "host: game listens"                     wait_log 1 "^udp listen" 40
check "client 2 joined the game"               wait_log 2 "^we are player [0-9]+" 60
check "client 3 joined the game"               wait_log 3 "^we are player [0-9]+" 60
check "host sees both players join the game"   wait_log 1 "^player 3 joined" 30
if [ "${MDK2_EXTENDED:-0}" = 1 ]; then
    check "real game process reports three peers after handoff" "$out/lobby_peers.exe" "$MDK2_PORT" game "$live_version" 1000
fi

for i in 1 2 3; do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "logs and screenshots: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
