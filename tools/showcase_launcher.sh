#!/usr/bin/env bash
# Launcher showcase shots with three background games whose launchers run hidden and are shot from their own
# canvas: the Play page (host), the Join list finding two hosts, the clipboard offer for an invite (a made-up
# code on a documentation address, put in through the test command, never on the real clipboard), the host's lobby
# with hero portraits and "Friends can join by", the guest's lobby, and the Saves tab with seeded saves.
# Saved as PNG under local/showcase/<time>-launcher/. Not a test: nothing is checked.
# The lobby panel shows this PC's addresses and the invite code: blur those lines before publishing a shot
# (tools/showcase_jpeg.ps1 does, for the lobby shots).
#   MDK2_N=3 tools/showcase_launcher.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null || exit 1
trap mdk_kill EXIT

out="$here/../local/showcase/$(date +%Y%m%d-%H%M%S)-launcher"; mkdir -p "$out"
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do grep -qE "$2" "$(mdk_log "$1")" && return 0; sleep 1; done; echo "timeout: $2 (game $1)" >&2; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
lshot() { lcmd "$1" "shot show_$2" && sleep 0.3 && python3 "$here/tga2png.py" "$(dir "$1")/coop/show_$2.tga" "$out/$2.png" && rm -f "$(dir "$1")/coop/show_$2.tga" && echo "$2.png"; }

# canvas spots (1280x720), as tools/menu_review.sh
T_PLAY="click 780 50"; T_SAVES="click 880 50"
HOST="click 1000 200"; JOIN="click 1000 283"; PLAY="click 1000 660"

# a made-up invite on a documentation address (RFC 5737): code v1 with one LAN route, bytes as docs/DISCOVERY.md
fake=$(python3 - <<'PY'
A = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
def crc8(d):
    c = 0
    for b in d:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 7) & 255 if c & 128 else (c << 1) & 255
    return c
p = bytearray([0x10]) + (0x5a17c3e9).to_bytes(4, "little") + (27900).to_bytes(2, "little") + bytes([1, 1, 192, 0, 2, 44])
p.append(crc8(p))
bits = acc = 0; s = ""
for b in p:
    acc = (acc << 8) | b; bits += 8
    while bits >= 5:
        s += A[(acc >> (bits - 5)) & 31]; bits -= 5; acc &= (1 << bits) - 1
if bits: s += A[(acc << (5 - bits)) & 31]
print("MDK2-" + "-".join(s[i:i + 5] for i in range(0, len(s), 5)))
PY
)
python3 "$here/invite_decode.py" "$fake" >/dev/null || { echo "made-up invite does not decode" >&2; exit 1; }

# fake saves and names (the harness's), the real ones put back at the end
now=$(date +%s); d="$(dir 1)/coop"
for f in player.ini saves.ini; do rm -f "$d/$f.show"; [ -e "$d/$f" ] && cp "$d/$f" "$d/$f.show"; done
printf '[player]\r\nid=0a0b0c0d\r\nname=Hank\r\n' > "$d/player.ini"
cat > "$d/saves.ini" <<SEED
[h0a0b0c0d_1234]
a_no=3
a_level=4
a_cp=3
a_file=save\\c0c0d1234a.sav
started=$((now - 259200))
updated=$((now - 7200))
peak=3
m_n=3
m0=Hank
m1=Alice
m2=Bob
host_name=Hank
me=Hank
[h1f2e3d4c_9abc]
a_no=7
a_level=7
a_cp=2
a_file=save\\c0c0d9abca.sav
updated=$((now - 100000))
peak=2
m_n=2
m0=Dave
m1=Hank
host_name=Dave
me=Hank
SEED
printf '[player]\r\nid=0e0f0a0b\r\nname=Alice\r\n' > "$(dir 2)/coop/player.ini"
printf '[player]\r\nid=0c0d0e0f\r\nname=Doc Hawkins\r\n' > "$(dir 3)/coop/player.ini"
restore() { local f; for f in player.ini saves.ini; do rm -f "$d/$f"; [ -e "$d/$f.show" ] && mv "$d/$f.show" "$d/$f"; done; }
trap 'mdk_kill; restore' EXIT

mdk_kill; sleep 2
for i in 1 2 3; do setini $i test launcher 1; setini $i launcher show 1; rm -f "$(dir $i)/coop/launcher.txt"; done
setini 1 launcher mode host; setini 2 launcher mode join; setini 3 launcher mode host
P1=$(mdk_start 1); P2=$(mdk_start 2); P3=$(mdk_start 3); echo "$P1 $P2 $P3" > "$MDK2_PIDS"
wait_log 1 "^launcher: shown at" 40 && wait_log 2 "^launcher: shown at" 40 && wait_log 3 "^launcher: shown at" 40 || exit 1

lcmd 1 "$T_PLAY" "$HOST"; lshot 1 play
lcmd 1 "$T_SAVES"; lshot 1 saves
lcmd 1 "$T_PLAY" "$PLAY"; wait_log 1 "^lobby: open as host" 15
lcmd 3 "$T_PLAY" "$HOST" "$PLAY"; wait_log 3 "^lobby: open as host" 15
lcmd 2 "$T_PLAY"; sleep 6; lshot 2 join                      # both lobbies listed
lcmd 2 "clipboard Doc Hawkins is hosting MDK2 Co-op. Join: https://example.invalid/join/#$fake"; sleep 1; lshot 2 offer
lcmd 2 "click 1000 410"; lcmd 3 "click 220 652"; sleep 8     # Not now; the second host leaves, so Join goes to Hank's lobby
lcmd 2 "$PLAY"; wait_log 2 "^lobby: connected to the host" 20; wait_log 1 "^lobby: Alice joined" 15
lcmd 2 "say Over here: the door is open"; sleep 1.5
lcmd 2 "primary"; sleep 1.5
lshot 1 lobby_host; lshot 2 lobby_guest
echo "launcher shots: $out"
