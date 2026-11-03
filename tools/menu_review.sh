#!/usr/bin/env bash
# Menu review captures with two games in background test mode: every launcher tab, every Settings section,
# the lobby as host and as guest (launcher drawn hidden and shot from its own canvas), then every
# Options > Co-op page in the game at 1280x720 and 1920x1080 (shot by the bridge from the back buffer, so
# covered windows are fine). Not a test: nothing is asserted except that the pages could be reached.
#   tools/menu_review.sh [launcher|game|all] [outdir]
# PNGs go to <outdir> (default local/menu-review/shots): L<scale>_<name>.png for the launcher (100 = 1280x720,
# 150 = 1920x1080), G<w>x<h>_<host|guest>_<page>.png for the game. Game copies: see tools/mdk.sh.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
what="${1:-all}"
out="${2:-$here/../local/menu-review/shots}"; mkdir -p "$out"
MDK2_BACKGROUND=1 MDK2_LOBBY=1 "$here/install.sh" >/dev/null || exit 1

dir()   { mdk_wsl "$(mdk_dir "$1")"; }
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; echo "timeout: $2 (game $1)" >&2; return 1; }
setini() { powershell.exe -NoProfile -Command "\$null = Add-Type -Name P -Namespace W -PassThru -MemberDefinition '[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'; [W.P]::WritePrivateProfileString('$2', '$3', '$4', '$(mdk_dir "$1")\\coop\\coop.ini')" >/dev/null; }
lcmd()  { local f i; f="$(dir "$1")/coop/launcher.txt"; shift
          printf '%s\n' "$@" > "$f.tmp"; mv "$f.tmp" "$f"
          for i in $(seq 1 100); do [ -e "$f" ] || return 0; sleep 0.1; done; return 1; }
lshot() { lcmd "$1" "shot menu_$2" && sleep 0.3 && python3 "$here/tga2png.py" "$(dir "$1")/coop/menu_$2.tga" "$out/L${SCALE}_$3.png" && rm -f "$(dir "$1")/coop/menu_$2.tga"; }
lua()   { mdk_cmd "$(mdk_pid "$1")" "$2"; }
trap 'mdk_kill' EXIT

# canvas spots (1280x720), see qa_launcher.sh
T_PLAY="click 780 50"; T_SAVES="click 880 50"; T_SET="click 1000 50"; T_ABOUT="click 1070 50"
HOST="click 1000 200"; JOIN="click 1000 283"; SOLO="click 1000 367"; HOST_ANY="click 1000 185"
PLAY="click 1000 660"; SEC_NEXT="click 1220 115"

# ---- launcher ----------------------------------------------------------------------------------------------
seed_saves() {
    local now; now=$(date +%s); local d="$(dir 1)/coop"
    for f in player.ini saves.ini; do rm -f "$d/$f.menu"; [ -e "$d/$f" ] && cp "$d/$f" "$d/$f.menu"; done
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
}
unseed() { local d="$(dir 1)/coop"; for f in player.ini saves.ini; do rm -f "$d/$f"; [ -e "$d/$f.menu" ] && mv "$d/$f.menu" "$d/$f"; done; }

launcher_stage() {   # launcher_stage <scale percent>
    SCALE=$1
    local i settings_next=""
    mdk_kill; sleep 2
    for i in 1 2; do
        setini $i test launcher 1; setini $i test launcher_scale "$SCALE"; setini $i launcher show 1
        rm -f "$(dir $i)/coop/launcher.txt"
    done
    setini 1 launcher mode host; setini 2 launcher mode join
    seed_saves
    local P1 P2; P1=$(mdk_start 1); P2=$(mdk_start 2); echo "$P1 $P2" > "$MDK2_PIDS"
    wait_log 1 "^launcher: shown at" 40 && wait_log 2 "^launcher: shown at" 40 || return 1
    # Play: the three modes, as host
    lcmd 1 "$T_PLAY" "$HOST"; lshot 1 a play_host
    lcmd 1 "$JOIN"; lshot 1 a play_join
    lcmd 1 "$HOST_ANY" "$SOLO"; lshot 1 a play_solo
    lcmd 1 "$HOST_ANY"
    lcmd 1 "$T_SAVES"; lshot 1 a saves
    lcmd 1 "click 1000 290"; lshot 1 a saves_picked
    lcmd 1 "$T_SET"
    local n names=(coop_rules players_interface voice_sound controls display network launcher)
    for n in 0 1 2 3 4 5 6; do lshot 1 a "settings_${names[$n]}"; lcmd 1 "$SEC_NEXT"; done
    lcmd 1 "$T_ABOUT"; lshot 1 a about
    lcmd 1 "key 112"; lshot 1 a guide; lcmd 1 "key 112"
    # the lobby: the host opens it, the guest joins
    lcmd 1 "$T_PLAY" "$PLAY"; wait_log 1 "^lobby: open as host" 15
    sleep 1; lshot 1 a lobby_host_alone
    lcmd 2 "$T_PLAY"; sleep 4; lshot 2 b join_list
    lcmd 2 "$PLAY"; wait_log 2 "^lobby: connected to the host" 20; wait_log 1 "^lobby: Alice joined" 15
    lcmd 2 "say Hello from the guest"; sleep 1.5
    lshot 1 a lobby_host_guest_joined; lshot 2 b lobby_guest
    lcmd 2 "primary"; sleep 1.5; lshot 1 a lobby_host_ready; lshot 2 b lobby_guest_ready
    # other tabs while in the lobby (the lobby bar), settings as guest (host-decided rows) and as host
    lcmd 2 "$T_SET" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115"
    lshot 2 b settings_guest_rules
    lcmd 2 "$SEC_NEXT"; lcmd 2 "$SEC_NEXT"; lshot 2 b settings_guest_voice
    lcmd 1 "$T_SET" ; lcmd 1 "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115" "click 1050 115"
    lshot 1 a settings_host_rules_in_lobby
    lcmd 1 "$T_SAVES"; lshot 1 a saves_in_lobby
    lcmd 1 "$T_ABOUT"; lshot 1 a about_in_lobby
    mdk_kill; sleep 2
    unseed
}

# ---- the game ----------------------------------------------------------------------------------------------
set_video() {   # set_video <instance> <w> <h>: the instance's own settings.lua
    local f="$(dir "$1")/profile/MDK2HD/settings.lua"
    sed -i "s/^chSetVideoMode.*/chSetVideoMode('', -8, -8, $2, $3, 0, 0)/" "$f"
    grep -q "^chSetVideoMode.*$2, $3" "$f"
}
gshot() {   # gshot <instance> <role> <page-name>
    sleep 1.2
    mdk_shot "$(mdk_pid "$1")" "$out/G${W}x${H}_$2_$3.png"
}
# The DPS meter, live (state from the game's own combat dump, not the log): off by default, the in-game
# Players and interface switch turns it on and off within seconds, Ctrl+M cycles off / line / full.
dps_checks=0; dps_fail=0
dps_meter() { lua "$1" 'coop_combat_dump()'; sleep 0.8; awk '$1 == "combat" { print $4; exit }' "$(dir "$1")/coop/combat.txt" | tr -d '\r'; }
dps_ini()   { tr -d '\r' < "$(dir "$1")/coop/coop.ini" | awk -F= '/^\[/ { s = $0 } s == "[hud]" && $1 == "dps" { print $2 }'; }
dps_expect() {   # dps_expect <what> <wanted meter> [wanted ini value]
    local got ini; got=$(dps_meter 1); ini=$(dps_ini 1)
    dps_checks=$((dps_checks + 1))
    if [ "$got" = "$2" ] && { [ -z "${3:-}" ] || [ "$ini" = "$3" ]; }; then echo "PASS dps: $1 (meter $got, [hud] dps '${ini}')"
    else echo "FAIL dps: $1: meter $got (want $2), [hud] dps '${ini}' (want ${3:-any})"; dps_fail=$((dps_fail + 1)); fi
}
dps_stage() {
    local hp; hp=$(mdk_pid 1)
    dps_expect "off by default" 0
    mdk_key "$hp" 1; sleep 2                                   # pause menu
    mdk_key "$hp" 208 3; sleep 0.5; mdk_key "$hp" 28; sleep 2  # Options
    mdk_key "$hp" 208 4; sleep 0.5; mdk_key "$hp" 28; sleep 1.5   # Co-op
    mdk_key "$hp" 208 3; sleep 0.5; mdk_key "$hp" 28; sleep 1.5   # Players and interface (the cursor starts above the first row)
    mdk_key "$hp" 208 3; sleep 0.5; gshot 1 host dps_row; mdk_key "$hp" 28; sleep 3    # DPS meter row: switch on
    dps_expect "in-game switch on reaches the meter live" 1 1
    mdk_key "$hp" 28; sleep 3
    dps_expect "in-game switch off reaches the meter live" 0 0
    mdk_key "$hp" 28; sleep 3
    lua 1 'HideMenu() mdkPauseGame(0)'; sleep 1
    dps_expect "switched on again" 1 1
    lua 1 'coop_press(29,500) coop_press(50,250)'; sleep 1; dps_expect "Ctrl+M: line to full" 2
    lua 1 'coop_press(29,500) coop_press(50,250)'; sleep 1; dps_expect "Ctrl+M: full to off" 0
    lua 1 'coop_press(29,500) coop_press(50,250)'; sleep 1; dps_expect "Ctrl+M: off to line" 1
    echo "dps checks: $((dps_checks - dps_fail)) passed, $dps_fail failed" | tee "$out/dps_result.txt"
    lua 1 'coop_set("hud", "dps", 0)'
}

game_stage() {   # game_stage <w> <h>
    W=$1; H=$2
    mdk_kill; sleep 2
    local i
    for i in 1 2; do setini $i test launcher 0; setini $i launcher show 0; set_video $i "$W" "$H" || echo "video not set for $i" >&2; done
    # level 1, checkpoint 2: the host loads it by itself once the guest has joined (MDK2_START at install)
    printf '\r\ncoop.autostart = { 1, 2 }\r\ncoop.autostart_players = 1\r\n' >> "$(dir 1)/coop/coop.lua"
    mdk_boot
    [ "$W" = 1280 ] && dps_stage
    local who inst pg
    for inst in 1 2; do
        who=host; [ $inst = 2 ] && who=guest
        # open the pause menu, then Options > Co-op with keys as a player would (the first time), pages by name after
        mdk_key "$(mdk_pid $inst)" 1; sleep 2
        mdk_key "$(mdk_pid $inst)" 208 3; sleep 0.5; mdk_key "$(mdk_pid $inst)" 28; sleep 2
        mdk_key "$(mdk_pid $inst)" 208 4; sleep 0.5; mdk_key "$(mdk_pid $inst)" 28; sleep 1.5
        wait_log $inst "^lua: menu: coop" 8
        gshot $inst $who coop
        for pg in coopsession coopplayers coopstart coopgame coopdiff coopui coopvoice coopctrl; do
            lua $inst "coop.ShowPage(\"$pg\")"; gshot $inst $who $pg
        done
        for pg in 1 2 3 7 8; do   # a few Controls sub-pages: On foot, Kurt, Sniper, Co-op, Gamepad
            lua $inst "coop.ctrlpage = $pg coop.ShowPage(\"coopctrl\")"; gshot $inst $who coopctrl_$pg
        done
        lua $inst 'coop_ctrl_show(0) HideMenu() mdkPauseGame(0)'; sleep 1
    done
    # the quick guide (hold F1) from the host
    mdk_press "$(mdk_pid 1)" 59 1 6000 100 & sleep 2.5; gshot 1 host guide; wait
    mdk_kill; sleep 2
}

case "$what" in
    launcher|all) launcher_stage 100; launcher_stage 150 ;;
esac
case "$what" in
    game|all) for res in ${MENU_GAME_RES:-1280x720 1920x1080}; do game_stage "${res%x*}" "${res#*x}"; done ;;
esac
echo "shots in $out"
