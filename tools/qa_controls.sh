#!/usr/bin/env bash
# Options > Co-op > Controls with one game in background test mode: the page reached with real
# key presses, every page, the Original / Modern / Custom presets (what is bound in the game),
# rebinding with a key press, a key taken from another action, clearing, and the Custom slot
# kept across a restart. Screenshots of every page go to the output folder.
#   tools/qa_controls.sh
# Game copy: MDK2_DIR1 or MDK2_DIR_BASE, see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null || exit 1

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-controls"; mkdir -p "$out"
check() {   # check <name> <command...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
}
log() { mdk_log 1; }
has() { grep -qE "$1" "$(log)"; }
wait_log() { local i; for i in $(seq 1 "$2"); do has "$1" && return 0; sleep 1; done; return 1; }
lua() { mdk_cmd "$(mdk_pid 1)" "$1" >/dev/null; }
bound() {   # bound <action>: "k1 k2" from the last dump
    grep -aE "^lua: ctrl $1 " "$(log)" | tail -1 | awk '{print $4, $5}' | tr -d '\r'; }
dump() { lua 'coop.ControlsDump()'; sleep 1; }
ini() { grep -E "^$1=" "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.ini" | tail -1 | cut -d= -f2 | tr -d '\r'; }
eq() { [ "$1" = "$2" ]; }

mdk_boot
HP=$(mdk_pid 1)
wait_log "^level 1 checkpoint 2 started" 60
sleep 3

# 1. as a player would: Esc, down to Options, Enter, down to Co-op, Enter, down to Controls, Enter
mdk_key "$HP" 1; sleep 2
mdk_key "$HP" 208 3; sleep 0.5; mdk_key "$HP" 28; sleep 2
mdk_key "$HP" 208 4; sleep 0.5; mdk_key "$HP" 28; sleep 1.5
mdk_key "$HP" 208 4; sleep 0.5; mdk_key "$HP" 28; sleep 2      # Co-op: Heading, Players, Session, Gameplay, Controls
check "Esc > Options > Co-op > Controls opens the page"   wait_log "^lua: menu: coopctrl" 5
check "the picture starts on the On foot page"            has "^lua: controls page: foot"
mdk_shot "$HP" "$out/page_foot.png"

# 2. Right on the Page combo walks the pages
for pg in kurt sniper max doc mini coop menus system; do
    mdk_key "$HP" 205; sleep 1.2
    check "page $pg"                                      has "^lua: controls page: $pg"
    mdk_shot "$HP" "$out/page_$pg.png"
done

# 3. the presets: what the game has bound
lua 'coop.SetPreset("original")'; dump
check "Original: forward Up / W, jump right mouse"        bash -c "[ '$(bound fwd)' = '200 17' ] && [ '$(bound jump)' = '1001 -1' ] && [ '$(bound sniper)' = '57 -1' ]"
check "Original: Max equip E / Q, Doc up ] / wheel up"    bash -c "[ '$(bound mequip)' = '18 16' ] && [ '$(bound dup)' = '27 1008' ]"
lua 'coop.SetPreset("modern")'; dump
check "Modern: forward W / Up, jump Space"                bash -c "[ '$(bound fwd)' = '17 200' ] && [ '$(bound jump)' = '57 -1' ]"
check "Modern: sniper is the right mouse, zoom on the wheel" bash -c "[ '$(bound sniper)' = '1001 -1' ] && [ '$(bound zoomin)' = '1008 17' ]"
check "Modern: chat T, ping V / middle mouse, revive R"   bash -c "[ '$(bound chat)' = '20 -1' ] && [ '$(bound ping)' = '47 1002' ] && [ '$(bound revive)' = '19 -1' ]"
check "chat, ping and revive keys reach the bridge"       has "^game keys 1: 47/258"
lua 'coop.EditCustom() coop.custom.chat = { 34, -1 } coop.custom.revive = { 35, -1 } coop.ApplyControls()'; sleep 1
check "a custom chat key is handed to the bridge (G)"     has "^game keys 0: 34/-1"
check "a custom revive key is handed to the bridge (H)"   has "^game keys 2: 35/-1"
lua 'coop.SetPreset("modern")'; sleep 1
check "back on Modern the bridge has T again"             has "^game keys 0: 20/-1"
check "preset kept in coop.ini"                           eq "$(ini preset)" modern
# no key does two things in one set of actions, in either preset
lua 'local bad = 0 local i, set = next(coop.sets, nil) while (i) do local seen = {} local n, id = next(set, nil) while (n) do local k = coop.Keys(id) local j = 1 while (j <= 2) do if (k[j] >= 0) then if (seen[k[j]]) then bad = bad + 1 coop_log("DUP "..id.." "..seen[k[j]].." "..k[j]) end seen[k[j]] = id end j = j + 1 end n, id = next(set, n) end i, set = next(coop.sets, i) end coop_log("DUPS "..bad)'; sleep 1
check "Modern has no key used twice in a set"             has "^lua: DUPS 0"

# 4. rebinding: Forward is the 3rd item of the page (Page, Preset, Forward); select it, press Y
lua 'coop.ctrlpage = 1 coop.ctrl_focus = 0 coop.ctrl_dirty = 1'; sleep 1
mdk_key "$HP" 208 2; sleep 0.5; mdk_key "$HP" 28; sleep 1
mdk_shot "$HP" "$out/rebind_wait.png"
mdk_key "$HP" 21; sleep 1.5
check "Forward takes the key pressed (Y)"                 wait_log "^lua: controls: fwd = 200, 21" 3
check "editing a preset switches to Custom"               eq "$(ini preset)" custom
dump
check "Custom: forward is Up / Y in the game"             eq "$(bound fwd)" "200 21"
check "Custom keys saved in coop.ini"                     eq "$(ini fwd)" "200,21"
check "the other actions were copied from Modern"         eq "$(ini jump)" "57,-1"
mdk_shot "$HP" "$out/rebound.png"

# 5. a key another action uses is taken from it: Back (the item below Forward) takes Y
mdk_key "$HP" 208; sleep 0.5; mdk_key "$HP" 28; sleep 1; mdk_key "$HP" 21; sleep 1.5
dump
check "Back now has Y"                                    bash -c "[ '$(bound back)' = '208 21' ] || [ '$(bound back)' = '31 21' ]"
check "Forward lost Y"                                    eq "$(bound fwd)" "200 -1"
check "the note says where Y went"                        has "^lua: controls: back = "

# 6. Delete clears the highlighted action
mdk_key "$HP" 211; sleep 1.5; dump
check "Delete clears the highlighted action"              eq "$(bound back)" "-1 -1"
mdk_shot "$HP" "$out/cleared.png"

# 7. Original and Modern are untouched by Custom edits, and Custom survives a restart
lua 'coop.SetPreset("original")'; dump
check "Original unchanged after editing Custom"           eq "$(bound fwd)" "200 17"
lua 'coop.SetPreset("custom")'; dump
check "Custom comes back"                                 eq "$(bound back)" "-1 -1"
mdk_kill; sleep 3
p=$(mdk_start 1)             # no install: it would rewrite coop.ini; echo "$p" > "$MDK2_PIDS"
wait_log "^level 1 checkpoint 2 started" 60; sleep 4
dump
check "after a restart the Custom preset is still active" eq "$(ini preset)" custom
check "after a restart Back is still cleared"             eq "$(bound back)" "-1 -1"
check "after a restart Forward is still Up only"          eq "$(bound fwd)" "200 -1"
lua 'coop.SetPreset("modern")'
check "no Lua errors"                                     bash -c "! grep -q 'LUA ERROR' '$(log)'"
mdk_kill

printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed; screenshots and log: $out"
cp "$(log)" "$out/game.log" 2>/dev/null
[ "$fail" = 0 ]
