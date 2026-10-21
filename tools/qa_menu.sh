#!/usr/bin/env bash
# Options > Co-op checks with MDK2_N (default 3) games in background test mode: the menu
# reached with real key presses, names, friendly fire (the host's setting, both ways), tougher
# enemies, difficulty presets and the rules they send, a client changing host address live, the controls presets, and settings kept across a
# restart. Screenshots of every page go to the output folder.
#   MDK2_N=3 tools/qa_menu.sh
# Game copies: see tools/mdk.sh. About 3 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-3}"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 3 ] || { echo "qa_menu.sh needs MDK2_N >= 3"; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-menu$MDK2_N"; mkdir -p "$out"
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
has()   { grep -qE "$2" "$(mdk_log "$1")"; }
count() { grep -cE "$2" "$(mdk_log "$1")"; }
wait_log() { local i; for i in $(seq 1 "$3"); do has "$1" "$2" && return 0; sleep 1; done; return 1; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
set_pid() { local i=0 p n=""; for p in $(mdk_pids); do i=$((i + 1)); [ $i = "$1" ] && p=$2; n="$n $p"; done; echo "$n" > "$MDK2_PIDS"; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
ini() { grep -E "^$2=" "$(mdk_wsl "$(mdk_dir "$1")")/coop/$3" | tail -1 | cut -d= -f2 | tr -d '\r'; }
hp_of() {   # hp_of <instance> <lua gob expression>: logs and prints its hp
    lua "$1" "coop_log(format(\"HP %d\", coop_hp($2) or -1))"; sleep 0.5
    grep -E "^lua: HP " "$(mdk_log "$1")" | tail -1 | sed 's/.* //' | tr -d '\r'; }
H=1; L=$MDK2_N

mdk_boot
check "all $MDK2_N games running"                       all_alive
check "settings read at start"                          has $H "^settings: friendly fire 0, enemy hp \+50%"

# 1. with keys, as a player would: Esc, down to Options, Enter, down to Co-op, Enter
HP=$(mdk_pid 1)
mdk_key "$HP" 1; sleep 2                                      # the pause menu fades in: keys before that are lost
mdk_key "$HP" 208 3; sleep 0.5; mdk_key "$HP" 28; sleep 2
mdk_key "$HP" 208 4; sleep 0.5; mdk_key "$HP" 28; sleep 1
check "Esc > Options > Co-op opens the co-op page"      wait_log $H "^lua: menu: coop" 5
mdk_shot "$HP" "$out/page_coop.png"
mdk_key "$HP" 208 1; mdk_key "$HP" 28; sleep 2           # Players
check "Players page opens"                              wait_log $H "^lua: menu: coopplayers" 5
mdk_shot "$HP" "$out/page_players.png"
for pg in coopsession coopgame coopdiff coopvoice; do lua 1 "coop.ShowPage(\"$pg\")"; sleep 1; mdk_shot "$HP" "$out/page_$pg.png"; done
lua 1 'HideMenu() mdkPauseGame(0)'

# 2. names: set on the last client, seen in the others' player lists
lua $L 'coop_set_name("Tester-'$L'")'; sleep 2
lastnum=$(sed -nE 's/^we are player ([0-9]+).*/\1/p' "$(mdk_log $L)" | tail -1 | tr -d '\r')
lua 1 "coop_log(\"NAME \" .. (coop_player_info($((lastnum - 1))) or \"?\"))"; sleep 0.5
check "host sees the new name"                          has 1 "^lua: NAME Tester-$L"
lua 2 "coop_log(\"NAME \" .. (coop_player_info($((lastnum - 1))) or \"?\"))"; sleep 0.5
check "another client sees it too (relayed)"            has 2 "^lua: NAME Tester-$L"
check "name kept in player.ini"                         test "$(ini $L name player.ini)" = "Tester-$L"
lua 1 "local n, p = coop_player_info($((lastnum - 1))) coop_log(format(\"PING %d\", p))"; sleep 0.5
check "host measures the client's ping"                 bash -c "grep -E '^lua: PING [0-9]+' '$(mdk_log 1)' | tail -1 | grep -qvE 'PING 0\b'"

# 3. friendly fire: off by default (the host decides), then on
h0=$(hp_of 1 "mdkGetPlayerGob()")
lua $L 'mdkDealDamage(mdkGetPlayerGob(), coop_gob("@p1"), 7, 1, -1)'; sleep 2
check "friendly fire off: client's hit on the host dropped" has 1 "friendly fire off: player $lastnum's hit on "
check "host hp unchanged ($h0)"                         test "$(hp_of 1 'mdkGetPlayerGob()')" = "$h0"
lua 1 'coop_set("game", "friendly_fire", 1)'; sleep 0.5
lua $L 'mdkDealDamage(mdkGetPlayerGob(), coop_gob("@p1"), 7, 1, -1)'; sleep 2
check "friendly fire on: host took the client's hit"    test "$(hp_of 1 'mdkGetPlayerGob()')" = "$((h0 - 7))"
c0=$(hp_of $L "mdkGetPlayerGob()")
lua 1 "mdkDealDamage(mdkGetPlayerGob(), coop_gob(\"@p$lastnum\"), 6, 1, -1)"; sleep 2
check "friendly fire on: client took the host's hit"    test "$(hp_of $L 'mdkGetPlayerGob()')" = "$((c0 - 6))"
lua 1 'coop_set("game", "friendly_fire", 0)'
check "setting written to the host's coop.ini"         test "$(ini 1 friendly_fire coop.ini)" = "0"

# 4. tougher enemies: Doganboys spawned with +50% per extra player, then with it off
lua 1 'coop_set("game", "enemy_scale", 0)'
mdk_key "$HP" 63; sleep 3                                     # F5: coop_dog1, plain
lua 1 'coop_set("game", "enemy_scale", 50)'
mdk_key "$HP" 63; sleep 3                                     # F5: coop_dog2, tougher
base=$(hp_of 1 coop_dog1); tough=$(hp_of 1 coop_dog2)
want=$((base + base * 50 * (MDK2_N - 1) / 100))
check "tougher enemy: $tough hp = $base + 50% x $((MDK2_N - 1))" test "$tough" = "$want"
check "client 2 has the same hp for it"                 test "$(hp_of 2 'coop_gob("coop_dog2") or coop_dog2')" = "$tough"

# 4a. difficulty: presets and Custom, the rules reaching the clients, then what they do
lua 1 'coop_log("PRESET " .. coop.DiffPresetName())'; sleep 0.5
check "default settings read as the Normal preset"      has 1 "^lua: PRESET Normal"
lua 1 'coop.SetDiffPreset(4)'; sleep 1
check "Nightmare preset written to coop.ini"            test "$(ini 1 dmg_taken coop.ini)-$(ini 1 down_secs coop.ini)-$(ini 1 friendly_fire coop.ini)" = "200-10-1"
lua 1 'coop_log("PRESET " .. coop.DiffPresetName())'; sleep 0.5
check "and read back as Nightmare"                      has 1 "^lua: PRESET Nightmare"
check "client 2 got the rules from the host"            has 2 "^rules from the host: damage taken 200%, dealt 100%, down 10 s, revive 5 s"
check "the last client too"                             has $L "^rules from the host: damage taken 200%, dealt 100%, down 10 s, revive 5 s"
lua 1 'coop_set("game", "dmg_taken", 75) coop_log("PRESET " .. coop.DiffPresetName())'; sleep 0.5
check "one value changed: Custom"                       has 1 "^lua: PRESET Custom"
# damage taken (the world's hits on players), at 200%: the host's player, then a client's
lua 1 'coop_set("game", "dmg_taken", 200)'; sleep 0.5
lua 1 'mdkDealDamage(coop_dog1, mdkGetPlayerGob(), 7, 2, -1)'; sleep 2   # a size the Doganboys never deal
check "damage taken 200%: host's player took 14 of a 7 hit" has 1 "^difficulty: coop_dog1 -> .* damage 7 -> 14"
c1=$(hp_of $L "mdkGetPlayerGob()")
lua 1 "mdkDealDamage(coop_dog1, coop_gob(\"@p$lastnum\"), 5, 2, -1)"; sleep 2
check "damage taken 200%: client lost 10 too"           test "$(hp_of $L 'mdkGetPlayerGob()')" = "$((c1 - 10))"
lua 1 'coop_set("game", "dmg_taken", 50)'; sleep 0.5
lua 1 'mdkDealDamage(coop_dog1, mdkGetPlayerGob(), 9, 2, -1)'; sleep 2
check "damage taken 50%: host's player took 4 of a 9 hit" has 1 "^difficulty: coop_dog1 -> .* damage 9 -> 4"
# damage dealt (players' hits on the world), at 200%
lua 1 'coop_set("game", "dmg_taken", 100) coop_set("game", "dmg_dealt", 200)'; sleep 0.5
d0=$(hp_of 1 coop_dog2)
lua 1 'mdkDealDamage(mdkGetPlayerGob(), coop_dog2, 5, 1, -1)'; sleep 2
check "damage dealt 200%: enemy lost 10 of a 5 hit"     test "$(hp_of 1 coop_dog2)" = "$((d0 - 10))"
# enemy health: 200% of normal, no per-player bonus
lua 1 'coop_set("game", "dmg_dealt", 100) coop_set("game", "enemy_hp", 200) coop_set("game", "enemy_scale", 0)'
mdk_key "$HP" 63; sleep 3                                     # F5: coop_dog3
check "enemy health 200%: $(hp_of 1 coop_dog3) hp = 2 x $base" test "$(hp_of 1 coop_dog3)" = "$((base * 2))"
lua 1 'coop.SetDiffPreset(2)'; sleep 1                            # back to Normal for the rest
check "back to the Normal preset"                       bash -c "test '$(ini 1 enemy_hp coop.ini)-$(ini 1 dmg_taken coop.ini)' = '100-100'"

# 4b. developer keys off: F5 spawns nothing; on again: it does
lua 1 'coop_set("controls", "devkeys", 0) coop.ApplyDevKeys(nil)'
n0=$(count 1 "^ent spawn .* coop_dog"); mdk_key "$HP" 63; sleep 2
check "developer keys off: F5 spawns nothing"           test "$(count 1 '^ent spawn .* coop_dog')" = "$n0"
lua 1 'coop_set("controls", "devkeys", 1) coop.ApplyDevKeys(1)'
mdk_key "$HP" 63; sleep 2
check "developer keys on: F5 spawns a Doganboy"         test "$(count 1 '^ent spawn .* coop_dog')" -gt "$n0"

# 5. the last client points at a wrong host address, waits, then back at the right one
lua $L 'coop_set_host("127.0.0.1", 28099)'
check "client with a wrong host address waits"         wait_log $L "waiting: Connection to the host lost" 8
lua $L "coop_set_host(\"127.0.0.1\", $MDK2_PORT)"
check "back on the right address it plays again"        wait_log $L "host is back: playing again" 20
check "with its player number"                          bash -c "grep -E '^we are player' '$(mdk_log $L)' | tail -1 | grep -q 'player $lastnum'"
check "address kept in coop.ini"                        test "$(ini $L peer_port coop.ini)" = "$MDK2_PORT"

# 6. controls presets: what Space and right mouse do in Original and Modern (tools/qa_controls.sh
# covers the page itself)
keys() { lua "$1" 'coop_log(format("KEYS jump %s snipe %s", omGetCommandKeyName(COM_JUMP), omGetCommandKeyName(COM_SMTOGGLE)))'; sleep 0.5
         grep -E "^lua: KEYS " "$(mdk_log "$1")" | tail -1 | tr -d '\r'; }
lua 2 'coop.SetPreset("modern")'; k1=$(keys 2)
lua 2 'coop.SetPreset("original")'; k0=$(keys 2)
lua 2 'coop.SetPreset("modern")'; k2=$(keys 2)
check "controls presets (${k1#lua: } / ${k0#lua: })" bash -c "[ '$k1' != '$k0' ] && [ '$k1' = '$k2' ]"

# 7. settings kept across a restart of client 2
lua 2 'coop_set("save", "interval", 300) coop_set_name("Second")'; sleep 1
taskkill.exe /PID "$(mdk_pid 2)" /F >/dev/null 2>&1; sleep 2
set_pid 2 "$(mdk_start 2)"
check "restarted client keeps its name"                 wait_log 2 "name Second" 30
lua_ok=0; for i in $(seq 1 30); do has 2 "^level 0 checkpoint 0 started" && { lua_ok=1; break; }; sleep 1; done
lua 2 'coop_log(format("INTERVAL %d", coop.save_interval))'; sleep 1
check "and its save interval"                           has 2 "^lua: INTERVAL 300"
check "all $MDK2_N games still running"                 all_alive
check "no Lua errors"                                   bash -c "! grep -l 'LUA ERROR' $(for i in $(mdk_seq); do printf '%q ' "$(mdk_log "$i")"; done)"
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
mdk_kill

printf '%s\n' "${results[@]}"
echo "screenshots and logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
