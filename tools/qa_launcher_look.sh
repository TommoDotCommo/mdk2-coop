#!/usr/bin/env bash
# The launcher's game look, live, read from the launcher's own state dump (coop\launcher-state.ini, [launcher] look_*):
#  * with the player's mdk2.tar: the title art, the menu font, the buttons' steel and the three heroes' panels load;
#    on every page (Play in each mode, Saves, each Settings section, About, the lobby as host and as guest) every
#    button label is drawn in the menu font and none is wider than its button
#  * the host's lobby shows each player's hero (a new game starts at level 1: Kurt) and the guest's does not (its
#    roster does not say which level the host chose)
#  * without the art ([test] launcher_art=0, as without mdk2.tar) the launcher draws everything in the GDI face, and the
#    art checks above fail on that dump (they can fail)
#   tools/qa_launcher_look.sh
# Game copies: see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-launcher-look"; mkdir -p "$out"
check() { local name="$1"; shift
          if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
dir()   { mdk_wsl "$(mdk_dir "$1")"; }
not()   { ! "$@"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
# dump <instance> <name>: the launcher's state now, kept as <name>.ini in the evidence folder
dump()  { local f="$(dir "$1")/coop/launcher-state.ini"; rm -f "$f"; lcmd "$1" "dump"; sleep 0.3
          tr -d '\r' < "$f" > "$out/$2.ini" 2>/dev/null; }
val()   { awk -F= -v k="$2" '/^\[/ { s = $0 } s == "[launcher]" && $1 == k { print $2; exit }' "$out/$1.ini"; }
trap 'mdk_kill' EXIT

# canvas spots (1280x720), as tools/qa_launcher.sh
T_PLAY="click 780 50"; T_SAVES="click 880 50"; T_SET="click 1000 50"; T_ABOUT="click 1070 50"
HOST="click 1000 200"; JOIN="click 1000 283"; SOLO="click 1000 367"; HOST_ANY="click 1000 185"
PLAY="click 1000 660"; SEC_NEXT="click 1220 115"

# art_ok <dump>: the art loaded and every label of the page in the menu font, none over its button
art_ok() {
    [ "$(val "$1" look_title)" = 1 ] && [ "$(val "$1" look_font)" = 1 ] && [ "$(val "$1" look_steel)" = 1 ] &&
    [ "$(val "$1" look_heroes)" = 3 ] && [ "$(val "$1" look_overflow)" = 0 ] && [ "$(val "$1" look_gdi_labels)" = 0 ] &&
    [ "${2:-1}" -le "$(val "$1" look_game_labels)" ]
}

mdk_kill; sleep 2
for i in 1 2; do
    setini $i test launcher 1; setini $i test launcher_scale 100; setini $i test launcher_art 1; setini $i launcher show 1
    rm -f "$(dir $i)/coop/launcher.txt"
done
setini 1 launcher mode host; setini 2 launcher mode join
P1=$(mdk_start 1); P2=$(mdk_start 2); echo "$P1 $P2" > "$MDK2_PIDS"
wait_log 1 "^launcher: shown at" 40 && wait_log 2 "^launcher: shown at" 40
check "both launchers up" has 2 "^launcher: shown at"

# ---- every page as host, with the art ----
lcmd 1 "$T_PLAY" "$HOST"; dump 1 play_host
lcmd 1 "$JOIN"; sleep 1; dump 1 play_join
lcmd 1 "$HOST_ANY" "$SOLO"; dump 1 play_solo
lcmd 1 "$HOST_ANY" "$T_SAVES"; dump 1 saves
lcmd 1 "$T_SET"
for n in 0 1 2 3 4 5 6; do dump 1 "settings_$n"; lcmd 1 "$SEC_NEXT"; done
lcmd 1 "$T_ABOUT"; dump 1 about
for p in play_host play_join play_solo saves settings_0 settings_1 settings_2 settings_3 settings_4 settings_5 settings_6 about; do
    check "art: $p in the game's look, no label over its button (labels $(val $p look_game_labels), over $(val $p look_overflow))" art_ok "$p"
done

# ---- the lobby: host and guest ----
lcmd 1 "$T_PLAY" "$PLAY"; wait_log 1 "^lobby: open as host" 15
lcmd 2 "$T_PLAY"; sleep 4; lcmd 2 "$PLAY"
wait_log 2 "^lobby: connected to the host" 20; wait_log 1 "^lobby: Alice joined|^lobby: .* joined" 15
sleep 1.5
dump 1 lobby_host; dump 2 lobby_guest
lcmd 1 "shot look_lobby_host"; lcmd 2 "shot look_lobby_guest"
check "lobby: two players on the host"                                  test "$(val lobby_host players)" = 2
check "lobby host: in the game's look (labels $(val lobby_host look_game_labels))" art_ok lobby_host 8
check "lobby host: the hero of level 1 (Kurt) known"                    test "$(val lobby_host look_hero)" = 1
check "lobby host: a portrait in each player's row ($(val lobby_host look_portraits))" test "$(val lobby_host look_portraits)" = 2
check "lobby guest: in the game's look (labels $(val lobby_guest look_game_labels))" art_ok lobby_guest 3
check "lobby guest: no hero claimed (its roster does not say the level)" test "$(val lobby_guest look_portraits)" = 0
python3 "$here/tga2png.py" "$(dir 1)/coop/look_lobby_host.tga" "$out/lobby_host.png" 2>/dev/null
python3 "$here/tga2png.py" "$(dir 2)/coop/look_lobby_guest.tga" "$out/lobby_guest.png" 2>/dev/null
rm -f "$(dir 1)/coop/look_lobby_host.tga" "$(dir 2)/coop/look_lobby_guest.tga"

# ---- without the art: everything as before the art, and the art checks fail on it ----
mdk_kill; sleep 2
setini 1 test launcher_art 0; setini 1 launcher mode host
rm -f "$(dir 1)/coop/launcher.txt"
P1=$(mdk_start 1); echo "$P1" > "$MDK2_PIDS"
wait_log 1 "^launcher: shown at" 40
lcmd 1 "$T_PLAY" "$HOST"; dump 1 noart_play
lcmd 1 "$T_SET"; dump 1 noart_settings
check "no art: logged"                                                  has 1 "^launcher: art off"
check "no art: nothing from the game drawn"                             bash -c "[ '$(val noart_play look_font)$(val noart_play look_steel)$(val noart_play look_title)$(val noart_play look_heroes)' = 0000 ]"
check "no art: labels in the GDI face ($(val noart_play look_gdi_labels)), none over its button" \
      bash -c "[ '$(val noart_play look_game_labels)' = 0 ] && [ '$(val noart_play look_gdi_labels)' -ge 2 ] && [ '$(val noart_play look_overflow)' = 0 ]"
check "no art: the page still works (primary $(val noart_play focus_label))" bash -c "[ '$(val noart_settings page)' = 2 ]"
check "the art checks fail without the art (they can fail)"              not art_ok noart_play
lcmd 1 "shot look_noart"; python3 "$here/tga2png.py" "$(dir 1)/coop/look_noart.tga" "$out/noart_play.png" 2>/dev/null
rm -f "$(dir 1)/coop/look_noart.tga"
setini 1 test launcher_art 1

for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log" 2>/dev/null; done
mdk_kill
printf '%s\n' "${results[@]}"
echo "evidence: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
