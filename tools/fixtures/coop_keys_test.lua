-- Run by tools/test_coop_keys.py with the real Lua 3.1: the key table block of lua/coop_dev.lua is loaded first
-- (with the game's functions stubbed below), then these checks. Prints "PASS name" / "FAIL name".
-- Written in Lua 3.1 (no local functions, no true).

coop = {}
ini = {}
logs = {}
nlogs = 0
bound = {}
function coop_get(sec, key, def)
    local v = ini[sec .. "|" .. key]
    if (v == nil) then return def end
    return v
end
function coop_set(sec, key, v) ini[sec .. "|" .. key] = v end
function coop_log(s) nlogs = nlogs + 1; logs[nlogs] = s end
function omBindCommandI(c, k) end
function omBindCommandI2(c, k) end
function omGetCommandBinding(c) return -1 end
function omGetCommandBinding2(c) return -1 end
gamekeys = {}
function coop_game_keys(which, a, b) gamekeys[which] = { a, b } end
function coop_emote_keys() end
function coop_pad_keys() end
function coop_guide_keys() end
guide = {}
nguide = 0
function coop_guide_clear() guide = {}; nguide = 0 end
function coop_guide_line(k, d) nguide = nguide + 1; guide[nguide] = { k, d } end
guide_names = nil
function coop_guide_names(a, b, c, d, e, f) guide_names = { a, b, c, d, e, f } end

nfail = 0
function check(name, ok)
    if (ok) then print("PASS " .. name) else print("FAIL " .. name); nfail = nfail + 1 end
end
function keys_are(id, preset, a, b)
    local k = preset[id]
    return k and k[1] == a and k[2] == b
end
function log_has(text)
    local i = 1
    while (i <= nlogs) do
        if (strfind(logs[i], text, 1, 1)) then return 1 end
        i = i + 1
    end
    return nil
end
function reset(saved)
    ini = {}
    logs = {}
    nlogs = 0
    coop.custom = {}
    if (saved) then
        local k, v = next(saved, nil)
        while (k) do ini["controls_custom|" .. k] = v; k, v = next(saved, k) end
    end
    coop.LoadCustom()
    coop.MigrateControls()
end
function old_copy()
    local t = {}
    local id, a = next(coop.old_modern, nil)
    while (id) do t[id] = format("%d,%d", a[1], a[2]); id, a = next(coop.old_modern, id) end
    return t
end
function find_line(keytext)
    local i = 1
    while (i <= nguide) do
        if (guide[i][1] == keytext) then return guide[i] end
        i = i + 1
    end
    return nil
end

-- 1. defaults of the shared keys
check("voice default: B and the mouse side button (modern)", keys_are("voice", coop.preset_modern, 48, 1003))
check("voice default: B and the mouse side button (original)", keys_are("voice", coop.preset_original, 48, 1003))
check("player list is Tab", keys_are("list", coop.preset_modern, 15, -1) and keys_are("list", coop.preset_original, 15, -1))
check("watch previous / next are [ and ]", keys_are("watchp", coop.preset_modern, 26, -1) and keys_are("watchn", coop.preset_modern, 27, -1))
check("modern open inventory lost Tab, F stays", keys_are("kuse", coop.preset_modern, 33, -1))
check("ping is V and the middle mouse, chat T, emote wheel H", keys_are("ping", coop.preset_modern, 47, 1002) and keys_are("chat", coop.preset_modern, 20, -1) and keys_are("emote", coop.preset_modern, 35, -1))

-- 2. no action but the owner holds a shared key in the Modern preset (the Original keeps the game's own [ ] for
-- Max's and Doc's inventories, which do nothing while you watch from outside the fight)
function stray(preset, allowed)
    local bad = nil
    local i, a = next(coop.actions, nil)
    while (i) do
        local k = preset[a.id]
        local n = 1
        while (n <= 2) do
            if (k[n] >= 0 and coop.Reserved(a.id, k[n]) and not (allowed and allowed[a.id])) then bad = a.id .. " has " .. k[n] end
            n = n + 1
        end
        i, a = next(coop.actions, i)
    end
    return bad
end
check("modern preset: shared keys belong to their actions only", stray(coop.preset_modern, nil) == nil)
check("original preset: only the game's [ ] for Max and Doc", stray(coop.preset_original, { minvl = 1, minvr = 1, dup = 1, ddown = 1 }) == nil)

-- 3. reserved keys
check("Tab is reserved for the player list", coop.Reserved("kuse", 15) == "list" and coop.Reserved("list", 15) == nil)
check("F1 and F2 are fixed", coop.Reserved("jump", 59) == "f1" and coop.Reserved("chat", 60) == "f2")
check("the side button and B are push to talk's", coop.Reserved("shoot", 1003) == "voice" and coop.Reserved("shoot", 48) == "voice")
check("V and the middle mouse are the ping's, T the chat's, H the emote wheel's", coop.Reserved("jump", 47) == "ping" and coop.Reserved("jump", 1002) == "ping" and coop.Reserved("jump", 20) == "chat" and coop.Reserved("jump", 35) == "emote")
check("[ and ] are the watch keys'", coop.Reserved("jump", 26) == "watchp" and coop.Reserved("jump", 27) == "watchn")
check("an unshared key is free", coop.Reserved("jump", 44) == nil)

-- 4. migration of a saved Custom slot: only defaults move, the player's own binding stays
reset(nil)
check("nothing saved: Custom is Modern, marked done", keys_are("kuse", coop.custom, 33, -1) and ini["controls_custom|v"] == "2")
old = old_copy()
reset(old)
check("untouched copy of the old Modern: open inventory moves off Tab", keys_are("kuse", coop.custom, 33, -1) and ini["controls_custom|kuse"] == "33,-1")
check("  ... and nothing else moved", keys_are("jump", coop.custom, 57, -1) and keys_are("voice", coop.custom, 48, -1) and keys_are("chat", coop.custom, 20, -1))
check("  ... the move is marked done", ini["controls_custom|v"] == "2")
old = old_copy()
old["jump"] = "44,-1"
reset(old)
check("a changed Custom keeps its Tab on open inventory", keys_are("kuse", coop.custom, 33, 15) and ini["controls_custom|kuse"] == "33,15")
check("  ... and says so in the log", log_has("kept"))
check("  ... and its own jump key stays", keys_are("jump", coop.custom, 44, -1))
old = old_copy()
old["kuse"] = "20,-1"
reset(old)
check("a player's own open inventory key stays", keys_are("kuse", coop.custom, 20, -1))
old = old_copy()
old["v"] = "2"
reset(old)
check("already migrated: left alone even if it looks like the old copy", keys_are("kuse", coop.custom, 33, 15))
old = old_copy()
old["voice"] = "48,-1"
old["chat"] = "21,-1"
reset(old)
check("a saved push to talk keeps its keys (defaults move, theirs stay)", keys_are("voice", coop.custom, 48, -1))
reset({ voice = "1003,-1" })
check("a saved custom side-button-only voice stays", keys_are("voice", coop.custom, 1003, -1))
check("an action missing from the file takes the new default", keys_are("list", coop.custom, 15, -1) and keys_are("watchn", coop.custom, 27, -1))

-- 5. the guide's key lines come from the bindings
coop.preset = "modern"
reset(nil)
coop.SyncGuideKeys()
check("guide has the fixed F1 and F2 lines", find_line("Hold F1") and find_line("F2"))
check("guide: chat T, push to talk B / Side mouse, ping V / Middle mouse", find_line("T") and find_line("Hold B / Side mouse") and find_line("V / Middle mouse"))
check("guide: player list is Hold Tab", find_line("Hold Tab"))
check("guide: watch is [ / ]", find_line("[ / ]"))
check("guide: wheels read Hold H + Mouse and Hold J + Mouse", find_line("Hold H + Mouse") and find_line("Hold J + Mouse"))
check("guide names for the sentences: revive R, voice B / Side mouse, list Tab", guide_names and guide_names[1] == "R" and guide_names[2] == "B / Side mouse" and guide_names[6] == "Tab")
coop.preset = "custom"
reset({ chat = "44,-1", list = "58,-1", emote = "46,-1" })
coop.SyncGuideKeys()
check("guide follows a rebound chat key (Z)", find_line("Z") and not find_line("T"))
check("guide follows a rebound player list key (Caps)", find_line("Hold Caps") and not find_line("Hold Tab"))
check("guide follows a rebound emote wheel (C)", find_line("Hold C + Mouse") and not find_line("Hold H + Mouse"))
reset({ watchp = "30,-1", watchn = "32,-1" })
coop.SyncGuideKeys()
check("guide follows rebound watch keys (A / D)", find_line("A / D") and not find_line("[ / ]"))
reset({ chat = "44,-1", list = "58,-1", emote = "46,-1" })
coop.SyncGuideKeys()
check("guide names follow too", guide_names[6] == "Caps")
n = 0
i, a = next(coop.actions, nil)
while (i) do
    if (a.guide and a.guide ~= "") then
        n = n + 1
        local want = coop.KeyText(coop.Keys(a.id))
        if (a.hold) then want = "Hold " .. want end
        if (a.wheel) then want = want .. " + Mouse" end
        if (not find_line(want)) then check("guide has a line for " .. a.id, nil) end
    end
    i, a = next(coop.actions, i)
end
check("every co-op action with a guide text has its line (" .. n .. ")", n >= 8)

-- 6. the shared words
check("one wording table", coop.words and coop.words.joined == "%s joined" and coop.words.left == "%s left" and
    coop.words.lost == "Lost %s, waiting up to 30 s" and coop.words.reconnecting == "Reconnecting to %s" and
    coop.words.version == "%s has a different version (%s)")
check("leaving: the others keep playing / everyone returns to the lobby", coop.words.leave_client == "The others keep playing" and coop.words.leave_host == "Everyone returns to the lobby")

-- 7. joined / left notices, from the one wording table
notices = {}
nnotices = 0
function coop_notice(s) nnotices = nnotices + 1; notices[nnotices] = s end
people = {}
function coop_slot() return 0 end
function coop_players() return 2, 4 end
function coop_player_info(s) return people[s] end
function scan()
    coop.notice_n = 99
    coop.NoticeTick()
end
people[1] = "Fox"
scan()
check("the first look only learns who is here (no notice)", nnotices == 0)
people[2] = "Wren"
scan()
check("a new player: 'Wren joined'", nnotices == 1 and notices[1] == "Wren joined")
scan()
check("no repeat while they stay", nnotices == 1)
people[1] = nil
scan()
check("a player gone: 'Fox left'", nnotices == 2 and notices[2] == "Fox left")
people[3] = "Kit"
people[1] = "Fox"
scan()
check("two at once: both joined", nnotices == 4)
coop.notice_n = 0
people[2] = nil
coop.NoticeTick()
check("not every frame: the scan waits", nnotices == 4)

if (nfail > 0) then print("RESULT FAIL " .. nfail) else print("RESULT OK") end
