-------------------------------------------------------------------------------
--  coop_dev.lua
--
--  Dev harness for MDK2 HD co-op. Installed as <game dir>\coop\coop.lua and run by
--  dinput8.dll from coop_init(), which a one-line hook in settings.lua calls after
--  mdk2.lua, so every game function is defined. Needs the bridge DLL.
--  Lua 3.1: no 'local function', closures need %upvalues.
-------------------------------------------------------------------------------

coop = {
    puppets = {},       -- other players' puppets by slot (0 = host)
    mirror = 0,         -- height above the other players' positions, for loopback testing
    marks = 0,          -- F2 markers written to bridge.log
    enemies = 0,        -- Doganboys spawned with F5
}

-- Lua errors also go to bridge.log (the game only prints them on screen); the game's own
-- error method still runs --
function coop.OnLuaError(msg)
    coop_log(format("LUA ERROR: %s", tostring(msg)))
    if (coop.olderror) then coop.olderror(msg) end
end
coop.olderror = seterrormethod(coop.OnLuaError)

-- Walker attack tasks run through ScriptUpdate, outside their native class think.
-- Restore the host's handles even when a task raises a Lua error (3.1 protected call).
coop.scriptupdate_orig = ScriptUpdate
function ScriptUpdate(gob)
    if (coop_ai_begin(gob) == 0) then return coop.scriptupdate_orig(gob) end
    local args = { gob }
    args.n = 1
    local r = call(coop.scriptupdate_orig, args, "x")
    coop_ai_end()
    return r
end

-- The first behind-glass enemy calls the native attack directly from its
-- authored OnUpdate, outside ScriptUpdate and outside the class callback.
-- Instant saves resolve native task functions by global name, not through coop.
coopNativeDoganboyAttack = mdkDoganboyAttack
coop.doganattack_orig = coopNativeDoganboyAttack
function mdkDoganboyAttack(gob)
    if (coop_ai_begin(gob) == 0) then return coop.doganattack_orig(gob) end
    local args = { gob }
    args.n = 1
    local r = call(coop.doganattack_orig, args, "x")
    coop_ai_end()
    return r
end

-- the client switches its spawners off (the host's make the real enemies), but some make
-- items (health, ammo respawns: mdk2.lua's item spawners), which the client needs too --
coop.setspawned_orig = mdkSpawnerSetSpawnedObject
function mdkSpawnerSetSpawnedObject(spawner, objtype, a, b, c, d, e, f, g)
    if (objtype and objtype >= 300 and objtype <= 399) then coop_keep_spawner(spawner) end
    return coop.setspawned_orig(spawner, objtype, a, b, c, d, e, f, g)
end

-- a gob that is already gone is left alone: a client has removed its copies of the host's enemies, which
-- movies still delete by name (level 8's intro: Level.ForAllConeheads(omGobDelete)), and the game's delete
-- of a gone gob crashes --
coop.delete_orig = omGobDelete
function omGobDelete(gob)
    if (coop_alive(gob)) then coop.delete_orig(gob) end
end

-- event kinds, as in bridge.h --
EV_HIT = 1
EV_APPLY = 2
EV_HURT = 3
EV_SPAWN = 4
EV_LEVEL = 8
EV_SAVE = 11

-- BioWare debug menu (F9 in game), normally behind '$if features' --
dofile('debug')

-- debug.lua also binds letter, number and keypad keys that clash with play (D is free
-- camera, I prints a nil orientation, 0/9 change game speed...); keep only its F-keys --
coop.debugkeys = {
    COM_CAMENABLE, COM_DEALDAMAGE, COM_DEALMOREDAMAGE, COM_ENABLESCRIPTS, COM_FREECAMERA,
    COM_JETCHEAT, COM_LIGHTCAMERA, COM_LIGHTMOVE, COM_LOOKLEFT, COM_LOOKRIGHT, COM_PRINTCOL,
    COM_PRINTGOBSTATS, COM_PRINTINFO, COM_RANDOMIZE, COM_SCENEGRAPH, COM_SHOWBUMP, COM_SHOWMESH,
    COM_TOGHITCHECK, COM_TURBO, COM_EXECLAST, COM_FASTFORWARD, COM_SLOWMO, COM_MATRIXMODE,
    COM_CAMZOOMIN, COM_CAMZOOMOUT, COM_RAISEUP, COM_RAISEDOWN, COM_LIGHTNEXT, COM_LIGHTPREV,
    COM_LIGHTRESET, COM_LIGHTDEFAULT1, COM_LIGHTDEFAULT2, COM_LIGHTDEFAULT3, COM_LIGHTDEFAULT4,
    COM_LIGHTDEFAULT5, COM_LIGHTDEFAULT6, COM_LIGHTDEFAULT7, COM_LIGHTDEFAULT8, COM_LIGHTDEFAULT9,
}
local i, com = next(coop.debugkeys, nil)
while (i) do
    omMakeCommand(com, "", CON_BUTTON_FALL, 0, 0)
    i, com = next(coop.debugkeys, i)
end

-- ---------------------------------------------------------------------------------------
-- Controls: Original (the game's own keys), Modern (WASD, Space jumps, right mouse aims) and a
-- Custom slot, picked in Options > Co-op > Controls ([controls] preset, custom keys in
-- [controls_custom]). Applied after settings.lua, so they win over the keys the game saved.
-- The page shows a keyboard and mouse with every bound key labelled, one page per situation,
-- drawn by the bridge (coop_ctrl_*); the game's own menu on the right picks page and preset
-- and rebinds (select an action, press a key). Keys are DirectInput scan codes; the mouse is
-- 1000 left, 1001 right, 1002 middle, 1008 wheel up, 1009 wheel down; -1 is unbound.
-- ---------------------------------------------------------------------------------------
coop.keynames = {
    [1]="Esc", [2]="1", [3]="2", [4]="3", [5]="4", [6]="5", [7]="6", [8]="7", [9]="8", [10]="9", [11]="0",
    [12]="-", [13]="=", [14]="Backspace", [15]="Tab", [16]="Q", [17]="W", [18]="E", [19]="R", [20]="T",
    [21]="Y", [22]="U", [23]="I", [24]="O", [25]="P", [26]="[", [27]="]", [28]="Enter", [29]="Ctrl",
    [30]="A", [31]="S", [32]="D", [33]="F", [34]="G", [35]="H", [36]="J", [37]="K", [38]="L", [39]=";",
    [40]="'", [41]="`", [42]="Shift", [43]="\\", [44]="Z", [45]="X", [46]="C", [47]="V", [48]="B", [49]="N",
    [50]="M", [51]=",", [52]=".", [53]="/", [54]="Shift", [56]="Alt", [57]="Space", [58]="Caps",
    [59]="F1", [60]="F2", [61]="F3", [62]="F4", [63]="F5", [64]="F6", [65]="F7", [66]="F8", [67]="F9",
    [68]="F10", [71]="Num 7", [72]="Num 8", [73]="Num 9", [75]="Num 4", [76]="Num 5", [77]="Num 6",
    [79]="Num 1", [82]="Num 0", [87]="F11", [88]="F12", [157]="Ctrl", [184]="Alt", [199]="Home", [200]="Up", [201]="PgUp",
    [203]="Left", [205]="Right", [207]="End", [208]="Down", [209]="PgDn", [210]="Insert", [211]="Delete",
    [1000]="Left mouse", [1001]="Right mouse", [1002]="Middle mouse", [1003]="Side mouse",
    [1004]="Mouse right", [1005]="Mouse left", [1006]="Mouse down", [1007]="Mouse up",
    [1008]="Wheel up", [1009]="Wheel down",
}

function coop.KeyName(k)
    if (not k or k < 0) then return "" end
    return coop.keynames[k] or format("Key %d", k)
end

-- pages: where an action is shown and rebound --
coop.ctrlpages = { "On foot", "Kurt", "Sniper", "Max", "Doc", "Minigames", "Co-op", "Gamepad", "Menus", "System" }
coop.pageid = { "foot", "kurt", "sniper", "max", "doc", "mini", "coop", "pad", "menus", "system" }

-- groups (colours) are the bridge's: 1 move, 2 look, 3 jump, 4 combat, 5 inventory, 6 sniper,
-- 7 system, 8 co-op, 9 menus. coms: every game command the action's keys are bound to
-- (the game's own key menu ties them the same way). sets: which actions are in use together,
-- for the conflict check when rebinding --
coop.actions = {
    { id="fwd", name="Forward", page="foot",   group=1, short="Fwd",    long="Move forward",    coms={ COM_FORWARD, COM_MINIUP } },
    { id="back", name="Backward", page="foot",   group=1, short="Back",   long="Move backward",   coms={ COM_BACKWARD, COM_MINIDOWN } },
    { id="strl", name="Strafe left", page="foot",   group=1, short="Left",   long="Strafe left",     coms={ COM_STRAFELEFT, COM_SMSTRAFELEFT, COM_MINILEFT2 } },
    { id="strr", name="Strafe right", page="foot",   group=1, short="Right",  long="Strafe right",    coms={ COM_STRAFERIGHT, COM_SMSTRAFERIGHT, COM_MINIRIGHT2 } },
    { id="turnl", name="Turn left", page="foot",   group=1, short="Turn<", long="Turn left",  coms={ COM_TURNLEFT, COM_MINILEFT } },
    { id="turnr", name="Turn right", page="foot",   group=1, short="Turn>", long="Turn right", coms={ COM_TURNRIGHT, COM_MINIRIGHT } },
    { id="lookup", name="Look up", page="foot",   group=2, short="Look^", long="Look up",   coms={ COM_LOOKUP, COM_MINIUP2 } },
    { id="lookdn", name="Look down", page="foot",   group=2, short="Lookv", long="Look down", coms={ COM_LOOKDOWN, COM_MINIDOWN2 } },
    { id="jump", name="Jump / jet", page="foot",   group=3, short="Jump",   long="Jump; jetpack (Max); parachute (Kurt)", coms={ COM_JUMP, COM_CHUTE, COM_JET } },
    { id="shoot", name="Fire", page="foot",   group=4, short="Fire",   long="Fire, melee, use the selected item", coms={ COM_SHOOT, COM_MELEE, COM_SMSHOOT, COM_INVCOMBINE, COM_INVACTIVATE } },
    { id="kinvl", name="Inventory left", page="kurt",   group=5, short="Inv <",  long="Inventory left",  coms={ COM_INVNEXT } },
    { id="kinvr", name="Inventory right", page="kurt",   group=5, short="Inv >",  long="Inventory right", coms={ COM_INVPREV } },
    { id="kuse", name="Open inventory", page="kurt",   group=5, short="Items",  long="Open the inventory", coms={ COM_INVENABLE } },
    { id="kclose", name="Close inventory", page="kurt",   group=5, short="Close",  long="Close the inventory", coms={ COM_INVDISABLE } },
    { id="sniper", name="Sniper mode", page="sniper", group=6, short="Sniper", long="Sniper mode on / off (Kurt)", coms={ COM_SMTOGGLE } },
    { id="zoomin", name="Zoom in", page="sniper", group=6, short="Zoom +", long="Zoom in (sniper mode)",  coms={ COM_SMZOOMIN } },
    { id="zoomout", name="Zoom out",page="sniper", group=6, short="Zoom -", long="Zoom out (sniper mode)", coms={ COM_SMZOOMOUT } },
    { id="minvl", name="Inventory left", page="max",    group=5, short="Inv <",  long="Inventory left",  coms={ COM_MAXINVNEXT } },
    { id="minvr", name="Inventory right", page="max",    group=5, short="Inv >",  long="Inventory right", coms={ COM_MAXINVPREV } },
    { id="mequip", name="Equip", page="max",    group=5, short="Equip",  long="Equip the selected weapon", coms={ COM_MAXEQUIP } },
    { id="munequip", name="Unequip",page="max",   group=5, short="Stow",long="Put the weapon away", coms={ COM_MAXUNEQUIP } },
    { id="dup", name="Inventory up", page="doc",    group=5, short="Up",     long="Inventory up",    coms={ COM_DOCINVUP } },
    { id="ddown", name="Inventory down", page="doc",    group=5, short="Down",   long="Inventory down",  coms={ COM_DOCINVDOWN } },
    { id="dleft", name="Inventory left", page="doc",    group=5, short="Left",   long="Inventory left",  coms={ COM_DOCINVENLEFT } },
    { id="dright", name="Inventory right", page="doc",    group=5, short="Right",  long="Inventory right", coms={ COM_DOCINVENRIGHT } },
    -- the co-op features that read the keyboard themselves (the bridge, coop_game_keys): gk is
    -- theirs; the mouse buttons work for them, the wheel does not
    -- feat: the [features] switch (Options > Co-op > Interface, the launcher) that can take the action's keys away
    { id="chat",   name="Chat",   page="coop", group=8, short="Chat",   long="Open the chat line (Enter sends, Esc cancels)", coms={}, gk=0, feat="chat" },
    { id="ping",   name="Ping",   page="coop", group=8, short="Ping",   long="Ping what you aim at: a marker for everyone", coms={}, gk=1, feat="pings" },
    { id="revive", name="Revive", page="coop", group=8, short="Revive", long="Hold beside a downed teammate to stand them up", coms={}, gk=2 },
    { id="voice",  name="Push to talk", page="coop", group=8, short="Talk", long="Hold to talk: players near you hear you (Options > Co-op > Voice chat)", coms={}, gk=3 },
    { id="emote",  name="Emote wheel", page="coop", group=8, short="Emote", long="Hold, move the mouse to an emote and let go to play it (moving ends it)", coms={}, gk=4, feat="emotes" },
    { id="char",   name="Character wheel", page="coop", group=8, short="Char", long="Hold, move the mouse to Kurt, Max or Doc and let go to switch to them", coms={}, gk=5, feat="charwheel" },
    -- the game's abort-movie command: with others watching, a vote to skip (coop_dev.lua "Cutscenes") --
    { id="skip",   name="Skip cutscene", page="coop", group=8, short="Skip", long="In a cutscene: vote to skip it; press again to take the vote back", coms={ COM_ABORTMOVIE } },
}

-- actions in use together: a key may not do two things in the same set --
coop.sets = {
    { "fwd", "back", "strl", "strr", "turnl", "turnr", "lookup", "lookdn", "jump", "shoot", "kinvl", "kinvr", "kuse", "kclose", "sniper", "chat", "ping", "revive", "voice", "emote", "char" },
    { "fwd", "back", "strl", "strr", "turnl", "turnr", "lookup", "lookdn", "jump", "shoot", "minvl", "minvr", "mequip", "munequip", "chat", "ping", "revive", "voice", "emote", "char" },
    { "fwd", "back", "strl", "strr", "turnl", "turnr", "lookup", "lookdn", "jump", "shoot", "dup", "ddown", "dleft", "dright", "chat", "ping", "revive", "voice", "emote", "char" },
    { "sniper", "zoomin", "zoomout", "strl", "strr", "shoot", "jump", "chat", "ping", "revive", "voice", "emote", "char" },
}

-- the presets: id = { key 1, key 2 } --
coop.preset_original = {
    fwd = { 200, 17 }, back = { 208, 31 }, strl = { 203, 30 }, strr = { 205, 32 }, turnl = { 44, -1 }, turnr = { 45, -1 },
    lookup = { 199, -1 }, lookdn = { 207, -1 }, jump = { 1001, -1 }, shoot = { 1000, -1 },
    kinvl = { 1008, 2 }, kinvr = { 1009, 4 }, kuse = { 33, 18 }, kclose = { 12, -1 },
    sniper = { 57, -1 }, zoomin = { 200, 17 }, zoomout = { 208, 31 },
    minvl = { 26, 1008 }, minvr = { 27, 1009 }, mequip = { 18, 16 }, munequip = { 16, 18 },
    dup = { 27, 1008 }, ddown = { 26, 1009 }, dleft = { 12, 16 }, dright = { 13, 18 },
    chat = { 20, -1 }, ping = { 47, 1002 }, revive = { 19, -1 }, voice = { 48, -1 }, emote = { 35, -1 }, char = { 36, -1 },
    skip = { 14, -1 },
}
coop.preset_modern = {
    fwd = { 17, 200 }, back = { 31, 208 }, strl = { 30, 203 }, strr = { 32, 205 }, turnl = { 44, -1 }, turnr = { 46, -1 },
    lookup = { 199, -1 }, lookdn = { 207, -1 }, jump = { 57, -1 }, shoot = { 1000, -1 },
    kinvl = { 16, 1008 }, kinvr = { 18, 1009 }, kuse = { 33, 15 }, kclose = { 34, -1 },
    sniper = { 1001, -1 }, zoomin = { 1008, 17 }, zoomout = { 1009, 31 },
    minvl = { 16, 1008 }, minvr = { 18, 1009 }, mequip = { 33, -1 }, munequip = { 34, -1 },
    dup = { 34, 1008 }, ddown = { 33, 1009 }, dleft = { 16, -1 }, dright = { 18, -1 },
    chat = { 20, -1 }, ping = { 47, 1002 }, revive = { 19, -1 }, voice = { 48, -1 }, emote = { 35, -1 }, char = { 36, -1 },
    skip = { 14, -1 },
}
coop.preset_names = { original = "Original", modern = "Modern", custom = "Custom" }
coop.preset_order = { "original", "modern", "custom" }

function coop.ActionById(id)
    local i, a = next(coop.actions, nil)
    while (i) do
        if (a.id == id) then return a end
        i, a = next(coop.actions, i)
    end
    return nil
end

-- custom keys from coop.ini: "k1,k2" per action; an action not there takes the Modern key --
coop.custom = {}
function coop.LoadCustom()
    local i, a = next(coop.actions, nil)
    while (i) do
        local v = coop_get("controls_custom", a.id, "")
        local c = strfind(v, ",", 1, 1)
        local k1, k2 = nil, nil
        if (c) then k1, k2 = tonumber(strsub(v, 1, c - 1)), tonumber(strsub(v, c + 1)) end
        if (k1 and k2) then
            coop.custom[a.id] = { k1, k2 }
        else
            coop.custom[a.id] = { coop.preset_modern[a.id][1], coop.preset_modern[a.id][2] }
        end
        i, a = next(coop.actions, i)
    end
end

function coop.SaveCustom(id)
    coop_set("controls_custom", id, format("%d,%d", coop.custom[id][1], coop.custom[id][2]))
end

-- the keys of one action in the active preset --
function coop.Keys(id)
    if (coop.preset == "original") then return coop.preset_original[id] end
    if (coop.preset == "custom") then return coop.custom[id] end
    return coop.preset_modern[id]
end

-- a [features] switch: on unless coop.ini says 0 --
function coop.Feature(name)
    return coop_get("features", name, "1") ~= "0"
end

function coop.BindAction(a)
    local k = coop.Keys(a.id)
    if (a.feat and not coop.Feature(a.feat)) then k = { -1, -1 } end
    if (a.gk) then coop_game_keys(a.gk, k[1], k[2]) end
    local n, c = next(a.coms, nil)
    while (n) do
        omBindCommandI(c, k[1])
        omBindCommandI2(c, k[2])
        n, c = next(a.coms, n)
    end
end

function coop.ApplyControls()
    local i, a = next(coop.actions, nil)
    while (i) do
        coop.BindAction(a)
        i, a = next(coop.actions, i)
    end
    -- the keys that end an emote (bridge/emote.c): moving, jumping, firing, sniper mode
    local f, b, l, r = coop.Keys("fwd"), coop.Keys("back"), coop.Keys("strl"), coop.Keys("strr")
    local j, s, n = coop.Keys("jump"), coop.Keys("shoot"), coop.Keys("sniper")
    coop_emote_keys(f[1], f[2], b[1], b[2], l[1], l[2], r[1], r[2], j[1], j[2], s[1], s[2], n[1], n[2])
    -- the gamepad (bridge/pad.c) presses the keys of these actions
    coop_pad_keys(coop.PadKey("fwd"), coop.PadKey("back"), coop.PadKey("strl"), coop.PadKey("strr"),
        coop.PadKey("jump"), coop.PadKey("shoot"), coop.PadKey("sniper"), coop.PadKey("kuse"),
        coop.PadKey("revive"), coop.PadKey("ping"))
    coop.SyncGuideKeys()
end

-- the first key of an action a pad button can press (keys and mouse buttons, not the wheel), or -1 --
function coop.PadKey(id)
    local k = coop.Keys(id)
    if (k[1] >= 0 and k[1] < 1008) then return k[1] end
    if (k[2] >= 0 and k[2] < 1008) then return k[2] end
    return -1
end

-- log what is bound now, first command of each action (tests) --
function coop.ControlsDump()
    local i, a = next(coop.actions, nil)
    while (i) do
        if (a.gk) then
            coop_log(format("ctrl %s %d %d", a.id, coop.Keys(a.id)[1], coop.Keys(a.id)[2]))
        else
            coop_log(format("ctrl %s %d %d", a.id, omGetCommandBinding(a.coms[1]), omGetCommandBinding2(a.coms[1])))
        end
        i, a = next(coop.actions, i)
    end
end

function coop.SetPreset(name)
    coop.preset = name
    coop_set("controls", "preset", name)
    coop.ApplyControls()
    coop_log("controls: " .. name)
end

-- short names for the menu's narrow value column --
coop.shortnames = { [1000]="LMB", [1001]="RMB", [1002]="MMB", [1003]="SMB", [1008]="Wh+", [1009]="Wh-",
    [14]="Bksp", [210]="Ins", [211]="Del", [201]="PgUp", [209]="PgDn", [205]="Right", [58]="Caps" }
function coop.ShortKeyText(k)
    local t, u = "", ""
    if (k[1] >= 0) then t = coop.shortnames[k[1]] or coop.KeyName(k[1]) end
    if (k[2] >= 0) then u = coop.shortnames[k[2]] or coop.KeyName(k[2]) end
    if (t ~= "" and u ~= "") then return t .. " " .. u end
    if (t ~= "") then return t end
    if (u ~= "") then return u end
    return "-"
end

-- "W / Up" for an action's keys --
function coop.KeyText(k)
    local t = coop.KeyName(k[1])
    if (k[2] >= 0) then
        if (t ~= "") then t = t .. " / " end
        t = t .. coop.KeyName(k[2])
    end
    if (t == "") then t = "(none)" end
    return t
end

function coop.SyncGuideKeys()
    local s, t = coop.ShortKeyText, coop.KeyText
    local k = coop.Keys
    local p = function(id)
        local a = coop.Keys(id)
        if (a[1] >= 0) then return coop.shortnames[a[1]] or coop.KeyName(a[1]) end
        if (a[2] >= 0) then return coop.shortnames[a[2]] or coop.KeyName(a[2]) end
        return "-"
    end
    coop_guide_keys(
        coop.preset_names[coop.preset] or "Modern",
        format("%s / %s, %s / %s", p("fwd"), p("back"), p("strl"), p("strr")),
        t(k("jump")),
        t(k("shoot")),
        t(k("sniper")),
        format("%s / %s", s(k("zoomin")), s(k("zoomout"))),
        format("%s / %s, %s open, %s close", s(k("kinvl")), s(k("kinvr")), s(k("kuse")), s(k("kclose"))),
        format("%s / %s, %s equip, %s stow", s(k("minvl")), s(k("minvr")), s(k("mequip")), s(k("munequip"))),
        format("%s / %s U/D, %s / %s L/R", s(k("dup")), s(k("ddown")), s(k("dleft")), s(k("dright"))),
        t(k("chat")),
        t(k("ping")),
        t(k("revive")),
        t(k("voice")),
        t(k("emote")),
        t(k("char")),
        t(k("skip"))
    )
end

-- the action using key `code` in a set with action `id`, if any --
function coop.Conflict(id, code)
    local si, set = next(coop.sets, nil)
    while (si) do
        local inset = nil
        local ni, nid = next(set, nil)
        while (ni) do
            if (nid == id) then inset = 1 end
            ni, nid = next(set, ni)
        end
        if (inset) then
            ni, nid = next(set, nil)
            while (ni) do
                if (nid ~= id) then
                    local k = coop.Keys(nid)
                    if (k[1] == code or k[2] == code) then return nid end
                end
                ni, nid = next(set, ni)
            end
        end
        si, set = next(coop.sets, si)
    end
    return nil
end

-- the game's rule for a new key: it becomes the second, the first drops out (when both are set) --
function coop.AddKey(id, code)
    local k = coop.custom[id]
    if (code == k[1] or code == k[2]) then return end
    if (k[1] >= 0 and k[2] >= 0) then k[1] = k[2]; k[2] = code
    elseif (k[1] >= 0) then k[2] = code
    elseif (k[2] >= 0) then k[1] = k[2]; k[2] = code
    else k[1] = code end
end

function coop.RemoveKey(id, code)
    local k = coop.custom[id]
    if (k[1] == code) then k[1] = k[2]; k[2] = -1
    elseif (k[2] == code) then k[2] = -1 end
end

-- Custom starts as a copy of whatever was active when you first change a key --
function coop.EditCustom()
    if (coop.preset == "custom") then return end
    local i, a = next(coop.actions, nil)
    while (i) do
        local k = coop.Keys(a.id)
        coop.custom[a.id] = { k[1], k[2] }
        coop.SaveCustom(a.id)
        i, a = next(coop.actions, i)
    end
    coop.SetPreset("custom")
end

coop.preset = coop_get("controls", "preset", "")
if (coop.preset ~= "original" and coop.preset ~= "modern" and coop.preset ~= "custom") then
    -- before presets there was [controls] modern (default on)
    if (coop_get("controls", "modern", "1") == "1") then coop.preset = "modern" else coop.preset = "original" end
end
coop.LoadCustom()
coop.ApplyControls()

-- Couch co-op ([couch] in coop.ini, written by dist/mdk2-couch.ps1): this
-- game is one of 2-4 local players, each its own window on one screen. The window is sized by
-- the bridge; the game is told the same size so it renders into all of it. Only player 1
-- makes sound, and everyone plays with the Modern preset, whose keys the gamepad presses
-- (bridge/pad.c; not saved, so the game copy's own preset is kept). --
do
    local slot, count = coop_couch()
    if (slot) then
        coop.couch_slot, coop.couch_count = slot, count
        local w = tonumber(coop_get("couch", "w", "960")) or 960
        local h = tonumber(coop_get("couch", "h", "540")) or 540
        chSetVideoMode('', 0, 0, w, h, 165, 0)
        coop.preset = "modern"
        coop.ApplyControls()
        if (slot > 1) then
            chSndSetEffectsVolume(0)
            chSndMusicVolume(0)
            chSndSetVoiceVolume(0)
        end
    end
end

-- The game's quicksave (F6) and quickload (F7) are off in co-op: a quickload restores this
-- game's own save, which the partner cannot follow (it showed up as level(l, -1) and the
-- client crashed loading its own save); F6 / F7 are the checkpoint keys instead --
omMakeCommand(COM_SAVEINSTANT, "", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_LOADINSTANT, "", CON_BUTTON_FALL, 0, 0)

-- Developer keys ([controls] devkeys, Options > Co-op > Gameplay; off for players, on in
-- the test installs): Num 1 hit / spawn, F3 lava, F4 teleport, F5 Doganboy, F6 / F7 / F8
-- checkpoints and levels for everyone, F9 BioWare debug menu, F10 test error, F11 anim log,
-- PgDn / PgUp (keypad 6 / 4) step through the player's animations as emotes (Home / End, keypad 9 / 7,
-- jump 10; Insert, keypad 5, keeps; Delete, keypad 8, lists; keypad 0 stops, and so does Backspace -- the game's
-- abort-movie key, see the cutscene vote -- when no movie plays).
-- F1 holds open the Mod Quick Guide. F2 (marker in every log, and a screenshot) stays on: it is how players report a problem. --
coop.devkeys = {
    { COM_SHOWBARS, "NUM 1", 79 }, { COM_CLEARCONSOLE, "F3" }, { COM_TOGLG, "F4" }, { COM_NEXTSECTION, "F5" },
    { COM_TOGL1, "F6" }, { COM_TOGL2, "F7" }, { COM_TOGL3, "F8" }, { COM_DEBUGMENUTOG, "F9" },
    { COM_PRINTCOL, "F10" }, { COM_TOGL4, "F11" },
    { COM_LIGHTRESET, "NUM 0" }, { COM_LIGHTDEFAULT4, "NUM 4", 201 }, { COM_LIGHTDEFAULT5, "NUM 5", 210 },
    { COM_LIGHTDEFAULT6, "NUM 6", 209 }, { COM_LIGHTDEFAULT7, "NUM 7", 207 }, { COM_LIGHTDEFAULT8, "NUM 8", 211 },
    { COM_LIGHTDEFAULT9, "NUM 9", 199 },
}

function coop.ApplyDevKeys(on)
    local i, k = next(coop.devkeys, nil)
    while (i) do
        if (on) then omMakeCommand(k[1], k[2], CON_BUTTON_FALL, 0, 0)
        else omMakeCommand(k[1], "", CON_BUTTON_FALL, 0, 0) end
        if (k[3]) then              -- the emote browser's keys on the keypad-less keys too (scan codes)
            if (on) then omBindCommandI(k[1], k[3]) else omBindCommandI(k[1], -1) end
        end
        i, k = next(coop.devkeys, i)
    end
    -- the abort-movie key (the cutscene vote, the "skip" action, Backspace by default); older builds bound
    -- Backspace to the emote stop too, and a saved settings.lua can still say so: keypad 0 stops the emote --
    if (on) then omBindCommandI(COM_LIGHTRESET, 82) else omBindCommandI(COM_LIGHTRESET, -1) end
    coop.BindAction(coop.ActionById("skip"))
end

-- a gob by the name the others use: a player's character ("@p<n>"), a named gob, or an
-- "n<id>" network name --
function coop.FindGob(name)
    if (strsub(name, 1, 1) == "@") then
        return coop_gob(name)
    end
    local gob = getglobal(name)
    if (gob and type(gob) ~= "number" and type(gob) ~= "string" and coop_alive(gob)) then
        return gob
    end
    return coop_gob(name)
end

-- client: bind a host actor to our gob of the same name, creating it if we lack one --
function coop.Spawn(name, netid, objtype, x, y, z, hp)
    local gob = coop_gob(format("n%x", netid))
    if (not gob) then
        gob = coop.FindGob(name)
        if (gob and (mdkGetGobType(gob) ~= objtype or coop_netid(gob) ~= 0)) then gob = nil end
    end
    if (not gob) then
        gob = mdkCreateObjectLua(name, objtype, mdkGetScene(), nil, nil, x,y,z, 1,0,0,0, nil,0,0,0,0)
    end
    if (gob) then
        coop_apply(EV_SPAWN, gob, netid, hp)
    else
        coop_log(format("spawn %s type %d failed", name, objtype))
    end
end

-- carry out events from the others --
function coop.HandleEvents()
    local kind, name, damage, dtype, part, netid, objtype, x, y, z, extra1, extra2 = coop_event()
    while (kind) do
        if (kind == EV_LEVEL) then
            coop.FollowLevel(dtype, damage, part, extra1, extra2)
        elseif (kind == EV_SAVE) then
            -- the host is saving: save this game under the same number --
            coop.MakeSave(extra2, netid, dtype, damage, extra1)
        elseif (kind == EV_SPAWN) then
            coop.Spawn(name, netid, objtype, x, y, z, damage)
        elseif (kind == EV_MOVIE) then
            coop.MovieEvent(name, damage, dtype, part, netid, x, y, z, extra1, extra2)
        else
            local target = coop.FindGob(name)
            if (target) then
                coop_apply(kind, target, damage, dtype, part)
            else
                coop_log(format("event %d: no gob named '%s'", kind, name))
            end
        end
        kind, name, damage, dtype, part, netid, objtype, x, y, z, extra1, extra2 = coop_event()
    end
end

-- The host is in level l at checkpoint cp: get there. same: 2 = we are in that very level
-- already (our link dropped and came back), 1 = the host restarted (load even if we seem to be
-- there). save: the co-op save to load, from host hostid's session: our own copy of it has our
-- inventory, hp and position; without it, the checkpoint. --
function coop.FollowLevel(l, cp, same, save, hostid)
    if (coop.join_cancelled) then return end
    if (same == 2) then
        coop_log(format("host is in level %d checkpoint %d: we are still there", l, cp))
        return
    end
    if (save > 0) then
        -- only that very save: an older one of the same level may be from another playthrough --
        local file, no, slevel, scp = coop_save_find(hostid, save, 0)
        if (file and slevel == l and mdkSaveFileExists(file) == 1) then
            coop_log(format("host is in level %d checkpoint %d: loading our co-op save %d", l, cp, no))
            coop.loading = { no = no, level = slevel, cp = scp }
            mdkStartInstantLoad(file)
            return
        end
        coop_log(format("no co-op save %d here: following to the checkpoint", save))
    end
    if (cp < 0) then
        -- a reload from the host's autosave: ours does not match, and loading it
        -- crashed the client (mdkGobCreateInstant) --
        coop_log(format("host reloaded level %d from its save: not followed", l))
    else
        -- same == 2 above is the only safe reuse: the epoch identifies the world.
        -- A host reset can load a different world at exactly this level/checkpoint.
        coop_log(format("host started level %d checkpoint %d: following", l, cp))
        coop.following = l * 1000 + cp
        mdkNewGame(l, cp)
    end
end

-- every frame, called by the bridge after the scene update (menus and title included):
-- send our player, apply the others' events, put each puppet where its player is --
-- Script handlers for what happened to another player (bridge/remote.c). The level scripts' handlers run only in the
-- game where the event happens, and what they change (a door locked behind the player, "the mortars are gone") is
-- script state no one syncs: a pickup or a room entered by someone else is run here too, with their puppet as "the
-- player" (mdkGetPlayerGob), because some handlers act on the player. A handler that locks a door closes an area:
-- whoever is on the far side of it, or outside the area, is brought to the one who triggered it, like a modern game. --
if (not coop_handlers_wrapped) then
    coop_handlers_wrapped = 1
    local real_player = mdkGetPlayerGob
    mdkGetPlayerGob = function()
        if (coop.ctx) then return coop.ctx end
        return %real_player()
    end
    local real_lock = mdkProxDoorLock
    mdkProxDoorLock = function(door, lock)
        -- a lock the handler of a remote event makes (not one already there) closes something --
        if (coop.ctxslot and lock and lock ~= 0 and door and coop_door_locked(door) == 0) then
            coop.nclosed = (coop.nclosed or 0) + 1
            coop.closed[coop.nclosed] = door
        end
        return %real_lock(door, lock)
    end
    coop.real_player = real_player
end
coop.closed = {}
coop.nclosed = 0

function coop.RunHandler(name, event, slot, arg)
    local g = getglobal(name)
    if (type(g) == "userdata" and not coop_alive(g)) then g = nil end
    local h = nil
    if (g) then h = g[event] end
    if (type(h) ~= "function") then
        coop_log(format("remote %s %s: nothing to run here", event, name))
        return
    end
    -- the trace (bridge/scripttrace.c) sees the handlers the engine runs; this one is ours --
    coop_log(format("script event L%d: %s %s (remote, player %d)", coop.level or 0, event, name, slot + 1))
    coop.ctx = coop.puppets[slot]
    coop.ctxslot = slot
    coop.nclosed = 0
    local args = {}
    args[1] = g args[2] = arg args.n = 2
    call(h, args, "x")
    coop.ctx = nil
    coop.ctxslot = nil
    if (coop.nclosed > 0) then coop.StartJoin(slot, name) end
    coop.nclosed = 0
end

-- is the door between a and b: near the straight line from one to the other, and between them --
function coop.DoorBetween(door, a, b)
    local dx, dy, dz = door.position.x, door.position.y, door.position.z
    local ax, ay, az = a.position.x, a.position.y, a.position.z
    local vx, vy, vz = b.position.x - ax, b.position.y - ay, b.position.z - az
    local len2 = vx * vx + vy * vy + vz * vz
    if (len2 < 1) then return nil end
    local t = ((dx - ax) * vx + (dy - ay) * vy + (dz - az) * vz) / len2
    if (t <= 0.02 or t >= 0.98) then return nil end
    local ex, ey, ez = ax + t * vx - dx, ay + t * vy - dy, az + t * vz - dz
    if (ex * ex + ey * ey + ez * ez <= coop.DOOR_REACH * coop.DOOR_REACH) then return 1 end
    return nil
end
coop.DOOR_REACH = 7

-- a cutscene, a minigame, a menu, or a ladder: not the moment to be moved --
function coop.JoinBusy()
    if (Save.movietocancel or Save.inminigame or coop.waiting) then return 1 end
    if (gamecontrol and gamecontrol.showmenu == 1) then return 1 end
    if (coop_near_type(OBJ_LADDER, OBJ_LADDERSPOT, 4) == 1) then return 1 end
    return nil
end

-- doors player `slot`'s handler `why` just locked: are we cut off from them? --
function coop.StartJoin(slot, why)
    local doors, n = {}, coop.nclosed
    local i = 1
    while (i <= n) do doors[i] = coop.closed[i] i = i + 1 end
    coop.join = { slot = slot, why = why, doors = doors, n = n, ends = coop_ms() + 20000 }
    coop.TryJoin()
end

function coop.TryJoin()
    local j = coop.join
    if (not j) then return end
    local pp = coop.puppets[j.slot]
    local me = coop.real_player()
    if (coop_ms() > j.ends or not pp or not me or coop_life(j.slot) ~= 0 or coop_life(coop_slot()) ~= 0) then
        coop.join = nil
        return
    end
    -- outside their area, or on the far side of a door that just locked --
    local cut = nil
    if (coop_stranded(j.slot) == 1) then cut = "outside their area" end
    local i = 1
    while (not cut and i <= j.n) do
        if (coop_alive(j.doors[i]) and coop.DoorBetween(j.doors[i], me, pp)) then cut = "on the far side of a locked door" end
        i = i + 1
    end
    if (not cut) then
        coop.join = nil
        return
    end
    if (coop.JoinBusy()) then return end        -- try again in a moment, for up to 20 s
    coop_log(format("%s closed an area behind player %d and we are outside it (%s): joining them", j.why, j.slot + 1, cut))
    coop_log(format("join: we are at (%.1f, %.1f, %.1f), they at (%.1f, %.1f, %.1f)", me.position.x, me.position.y, me.position.z,
                    pp.position.x, pp.position.y, pp.position.z))
    mdkGobSetPositionXYZ(me, pp.position.x + 1.5, pp.position.y, pp.position.z + 0.5)
    coop_notice(format("The way back closed: you joined %s", coop_player_name(j.slot)))
    coop.join = nil
end

-- The native dome result includes the glass, fan, animation and mortar spawn
-- point. Replay that result instead of hoping a visual projectile hits identically.
function coop.HookDomePuzzle()
    coop.dome_pending = {}
    if (coop.level ~= 1 or not Level or not Level.ShotHitDome) then return end
    local original = Level.ShotHitDome
    Level.ShotHitDome = function(gob)
        local was = gob.shattered
        %original(gob)
        if (not was and gob.shattered) then
            local i = 1
            while (i <= 3) do
                if (gob == getglobal("platportal" .. i)) then coop_puzzle(i) end
                i = i + 1
            end
        end
    end
    local guard = Level.GetPlatformGuard
    Level.GetPlatformGuard = function(gob)
        local g = %guard(gob)
        if (g and coop_alive(g) == 1) then return g end
        return nil
    end
    local i = 1
    while (i <= 3) do
        local g = getglobal("platportal" .. i)
        if (g and g.shattered and coop_role() == "host") then coop_puzzle(i) end
        i = i + 1
    end
end
coop.dome_pending = {}
function coop.DomeTick()
    local name = next(coop.dome_pending, nil)
    while (name) do
        local following = next(coop.dome_pending, name)
        local g = getglobal(name)
        local p = coop.real_player()
        if (g and p and Level and Level.ShotHitDome) then
            if (not g.shattered) then
                g.bulletgob = p
                Level.ShotHitDome(g)
                coop_log("shared dome result " .. name)
            end
            coop.dome_pending[name] = nil
        end
        name = following
    end
end

-- Inventory rewards use native collection for ammo limits and item effects.
-- Health rewards heal at the recipient's cap. World handlers run immediately;
-- tutorial inventory handlers wait until after the recipient receives its ammo.
coop.rewards = {}
coop.reward_serial = 0
function coop.SharedPickup(name, itemtype)
    coop.reward_serial = coop.reward_serial + 1
    local key = format("coop_reward_%d", coop.reward_serial)
    local source = name
    -- World state must follow now, even if our hero is down or cannot use this
    -- item yet. Only the two tutorial handlers change the recipient's inventory.
    if (name ~= "grenades" and name ~= "snipergrenade") then
        if (name ~= "") then
            coop.RunHandler(name, "OnPickedUp", coop_slot(), itemtype)
            coop_pickup_finish(name)
        end
        source = ""
    end
    coop.rewards[key] = { source = source, label = name, itemtype = itemtype }
end

-- A game started at a checkpoint (the launcher, the developer keys, a checkpoint restart) begins
-- with the level's bare inventory: what the rooms before it gave is missing. The items a later room
-- cannot be passed without, by level: { item type, the checkpoints that are past its pickup... }
-- (from the level scripts: where the pickup is registered, and the checkpoints' positions).
-- Each game gives its own player, through native collection like a shared pickup.
function coop.CheckpointItems(l, cp)
    local items = {
        [2] = { { OBJ_JETPACK, 6, 7, 8, 9, 10 } },       -- l2_r5jetpack, in room 5 (checkpoint 5)
        [5] = { { OBJ_JETPACK, 9, 10 } },                -- l5_r9_g04, below checkpoint 8
        [8] = { { OBJ_ATOMICJETPACK, 6, 7 } },           -- l8r5_jetpack, at checkpoint 5
    }
    local list = items[l]
    if (not list) then return end
    local k = 1
    while (list[k]) do
        local e = list[k]
        local i = 2
        while (e[1] and e[i]) do
            if (e[i] == cp) then
                coop.reward_serial = coop.reward_serial + 1
                coop.rewards[format("coop_reward_%d", coop.reward_serial)] =
                    { source = "", label = "checkpoint start", itemtype = e[1] }
                coop_log(format("level %d checkpoint %d starts without item type %d: given", l, cp, e[1]))
            end
            i = i + 1
        end
        k = k + 1
    end
end

function coop.SharedPickupDone(gob, itemtype)
    local key, r = next(coop.rewards, nil)
    while (key and r.gob ~= gob) do key, r = next(coop.rewards, key) end
    if (not key) then return end
    coop.SharedPickupComplete(key, itemtype)
end

function coop.SharedPickupComplete(key, itemtype)
    local r = coop.rewards[key]
    if (not r) then return end
    coop.rewards[key] = nil
    if (r.label == "checkpoint start") then coop.cp_given = (coop.cp_given or 0) + 1 end
    if (r.source ~= "") then
        coop.RunHandler(r.source, "OnPickedUp", coop_slot(), itemtype)
        coop_pickup_finish(r.source)
    end
    coop_log(format("shared pickup %s type %d received", r.label, itemtype))
end

function coop.SharedPickupTick()
    local p = coop.real_player()
    if (not p or coop_life(coop_slot()) ~= 0) then return end
    local key, r = next(coop.rewards, nil)
    while (key) do
        local following, following_reward = next(coop.rewards, key)
        if (not r.gob and coop_shared_health(r.itemtype) == 1) then
            coop.SharedPickupComplete(key, r.itemtype)
        elseif (not r.gob) then
            r.gob = mdkCreateObjectLua(key, r.itemtype, mdkGetScene(), nil, nil,
                p.position.x, p.position.y, p.position.z, 1,0,0,0, nil,0,0,0,0)
            if (r.gob) then
                r.gob.OnPickedUp = coop.SharedPickupDone
                omGobGMSetVisible(r.gob, 0)
            end
        elseif (coop_alive(r.gob)) then
            mdkGobSetPositionXYZ(r.gob, p.position.x, p.position.y, p.position.z)
        end
        key, r = following, following_reward
    end
end

function coop.HandlerTick()
    local n = 0
    local name, event, slot, arg, kill = coop_next_handler()
    while (name and n < 8) do
        if (kill == 3) then
            coop.dome_pending[name] = 1
        elseif (kill == 4) then
            coop.rear_gate_result = 1
        elseif (kill == 2) then
            coop.SharedPickup(name, arg)
        else
            coop.RunHandler(name, event, slot, arg)
        end
        if (kill == 1) then coop_pickup_finish(name) end
        n = n + 1
        name, event, slot, arg, kill = coop_next_handler()
    end
    coop.DomeTick()
    coop.SharedPickupTick()
    if (coop.join) then coop.TryJoin() end
    coop.ProximityTick()
    coop.RearGateTick()
end

-- Opening r9's entrance normally locks r8's exit behind the single player.
-- Animation callbacks run natively on both peers, outside RunHandler's join
-- context. Keep this one rear gate usable until everyone has crossed it; the
-- host commits the original script result and reliably tells the clients.
function coop.RearGatePast(player, gate, destination)
    local dx = destination.position.x - gate.position.x
    local dy = destination.position.y - gate.position.y
    local dz = destination.position.z - gate.position.z
    local length = sqrt(dx * dx + dy * dy + dz * dz)
    if (length < 1) then return nil end
    return (player.position.x - gate.position.x) * dx +
           (player.position.y - gate.position.y) * dy +
           (player.position.z - gate.position.z) * dz > length * 3
end

-- A named global callback survives the game's instant-save function lookup.
function coopRearGateOnAnimLoop(gob, anim)
    if (anim ~= ANIM_OPEN) then return end
    if (coop_players() == 0) then
        mdkProxDoorLock(l2_r8entrdoor06, 1)
    elseif (coop_role() == "host") then
        Save.coop_rear_gate_pending = 1
    end
end

function coop.RearGateTick()
    if (coop.level ~= 2) then return end
    local trigger = getglobal("l2_r9entrdoor")
    local gate = getglobal("l2_r8entrdoor06")
    if (not trigger or not gate or not coop_alive(trigger) or not coop_alive(gate)) then return end
    if (type(trigger.OnAnimLoop) == "function") then
        trigger.OnAnimLoop = coopRearGateOnAnimLoop
    end
    if (coop_role() == "host" and Save.coop_rear_gate_pending) then
        local me = coop.real_player()
        if (not me or coop_life(coop_slot()) ~= 0 or not coop.RearGatePast(me, gate, trigger)) then return end
        local slot = 0
        local others, slots = coop_players()
        while (slot < slots) do
            -- A downed teammate must remain reachable for revival.
            if (slot ~= coop_slot() and coop_connected(slot) == 1 and coop_life(slot) <= 1) then
                local p = coop.puppets[slot]
                if (not p or not coop_alive(p) or not coop.RearGatePast(p, gate, trigger)) then return end
            end
            slot = slot + 1
        end
        Save.coop_rear_gate_pending = nil
        Save.coop_rear_gate_closed = 1
    end
    if (coop_role() == "host" and Save.coop_rear_gate_closed) then
        coop_puzzle(4)
        coop.rear_gate_result = 1
    end
    if (coop.rear_gate_result) then
        mdkProxDoorLock(gate, 1)
        coop.rear_gate_result = nil
    end
end

-- Handlers that start something when "the player" comes near (the boss on level 7 when the player is within 40 of
-- r8_center) look at the local player only, so a game whose player was far away never started it. These look at the
-- player and nothing else (tools/script_census.py --proximity-lua); each game gives them the NEAREST player,
-- ours or a puppet, so whoever comes near starts it on every game. --
coop.nearest = {
    [1] = { "l1_r3spinners", "OnUpdate", "l1_r8", "OnUpdate" },
    [2] = { "l2_r8", "OnUpdate", "l2_rright", "OnUpdate" },
    [3] = { "l3_maze", "OnUpdate", "l3_hublad01", "OnUpdate" },
    [4] = { "l4_r4snipball00", "OnUpdate", "l4_r4snipball01", "OnUpdate", "l4_r4snipball02", "OnUpdate", "l4_r5side", "OnUpdate" },
    [6] = { "l6_r4a", "OnUpdate", "l6_r4d", "OnUpdate", "l6_doors12", "OnUpdate", "l6_doors13", "OnUpdate", "l6_doors15", "OnUpdate", "l6_door", "OnUpdate" },
    [7] = { "l7r2", "OnUpdate", "l7r4", "OnUpdate", "l7r6", "OnTimer", "r8", "OnUpdate", "l7r1_floor1", "OnUpdate" },
    [11] = { "l10r2", "OnUpdate" },
}

function coop.NearestPlayer(gob)
    local me = coop.real_player()
    local best = me
    local bd = nil
    if (me) then bd = coop.Dist2(me, gob) end
    local s, p = next(coop.puppets, nil)
    while (s) do
        if (p and coop_alive(p) and coop_life(s) == 0) then
            local d = coop.Dist2(p, gob)
            if (not bd or d < bd) then best = p bd = d end
        end
        s, p = next(coop.puppets, s)
    end
    return best
end

function coop.Dist2(a, b)
    local x, y, z = a.position.x - b.position.x, a.position.y - b.position.y, a.position.z - b.position.z
    return x * x + y * y + z * z
end

coop.nearwrapped = {}

function coop.MakeNearest(h)
    return function(gob, a, b, c)
        local before = coop.ctx
        if (not before) then coop.ctx = coop.NearestPlayer(gob) end
        local args = {}
        args[1] = gob args[2] = a args[3] = b args[4] = c args.n = 4
        local r = call(%h, args, "x")
        coop.ctx = before
        return r
    end
end

function coop.ProximityTick()
    coop.proxticks = (coop.proxticks or 0) + 1
    if (coop.proxticks < 45 or not coop.level or coop_players() < 1 or coop.no_nearest) then return end
    coop.proxticks = 0
    local list = coop.nearest[coop.level]
    if (not list) then return end
    local i = 1
    while (list[i]) do
        local g = getglobal(list[i])
        if (g) then
            local event = list[i + 1]
            local h = g[event]
            if (type(h) == "function" and not coop.nearwrapped[h]) then
                local w = coop.MakeNearest(h)
                g[event] = w
                coop.nearwrapped[w] = 1
                coop_log(format("proximity: %s.%s sees the nearest player", list[i], event))
            end
        end
        i = i + 2
    end
end

-- ---------------------------------------------------------------------------------------
-- Campaign bot pilot (tools/campaign_bot.py drives it): walks this game's own hero to a point
-- with ordinary controls. The hero faces the point (an aim, like a mouse turn) and the forward
-- key is held; the game's physics does the walking. No progress for a while: jump, then strafe.
-- coop.pilot.state: "go", "arrived", "stuck"; coop.pilot.trail counts the samples written to
-- coop/pilot.txt (time, position, distance) for the run's evidence.
-- ---------------------------------------------------------------------------------------
coop.pilot = nil
coop.pilot_keys = { forward = 17, back = 31, left = 30, right = 32, jump = 57 }
function coop.PilotGo(x, y, z, radius, flat, explore)
    coop.pilot = { x = x, y = y, z = z, r = radius or 2.5, flat = flat or 1, state = "go", explore = explore or 0,
                   t0 = coop_ms(), best = 1000000, best_ms = coop_ms(), stuck = 0, next_ms = 0 }
end
-- The pilot's eye: the engine's own line of sight (walls, floors, ceilings) from the hero to an
-- invisible probe at a point, at body height.
function coop.PilotSees(p, x, y, z)
    if (not coop.pilot_probe or coop_alive(coop.pilot_probe) ~= 1) then
        coop.pilot_probe = mdkCreateObjectLua("coop_pilot_probe", OBJ_GRENADEITEM, mdkGetScene(), nil, nil,
                                              0, 0, 0, 1, 0, 0, 0, nil, 0, 0, 0, 0)
        omGobEnterStasis(coop.pilot_probe)
    end
    mdkGobSetPositionXYZ(coop.pilot_probe, x, y, z)
    return mdkAILineOfSight(p, coop.pilot_probe, 6.3, 100000) == 1
end
-- When the goal is out of sight: the visible point around the hero that brings it nearest the
-- goal, counting how often the hero has already been near that point (so it backs out of dead ends).
function coop.PilotChoose(p, pl)
    local best, bx, by = nil, nil, nil
    local z = p.position.z + 1.5
    local r = 1
    while (r <= 3) do
        local reach = r * 10
        local a = 0
        while (a < 16) do
            local x = p.position.x + reach * cos(a * 22.5)
            local y = p.position.y + reach * sin(a * 22.5)
            if (coop.PilotSees(p, x, y, z)) then
                local gx, gy = pl.x - x, pl.y - y
                local cell = format("%d_%d", floor(x / 6), floor(y / 6))
                local score = sqrt(gx * gx + gy * gy) + 25 * (pl.visits[cell] or 0)
                if (not best or score < best) then best, bx, by = score, x, y end
            end
            a = a + 1
        end
        r = r + 1
    end
    return bx, by
end
-- Navigation scan: the engine's own collision, asked through line of sight between two invisible
-- probes, maps where a hero can stand. A flood fill from a point: each neighbouring cell's floor is
-- found by a vertical search near the current height, and cells are joined when the step is small and
-- the way between them is clear at chest height. Done a few cells per frame; the cells go to
-- coop/note_nav_<n>.txt in batches ("x y z i j" per cell, neighbours as "e i j k l").
function coop.NavProbe(name)
    local g = getglobal(name)
    if (not g or coop_alive(g) ~= 1) then
        g = mdkCreateObjectLua(name, OBJ_GRENADEITEM, mdkGetScene(), nil, nil, 0, 0, 0, 1, 0, 0, 0, nil, 0, 0, 0, 0)
        omGobEnterStasis(g)
        setglobal(name, g)
    end
    return g
end
function coop.NavClear(ax, ay, az, bx, by, bz)
    local a, b = coop.NavProbe("coop_nav_a"), coop.NavProbe("coop_nav_b")
    mdkGobSetPositionXYZ(a, ax, ay, az)
    mdkGobSetPositionXYZ(b, bx, by, bz)
    return mdkAILineOfSight(a, b, 6.3, 100000) == 1
end
-- the floor under (x, y) between top and bottom: the lowest z still in sight from top, to 0.25 units --
function coop.NavFloor(x, y, top, bottom)
    if (coop.NavClear(x, y, top, x, y, bottom)) then return nil end
    local lo, hi = bottom, top
    while (hi - lo > 0.25) do
        local mid = (lo + hi) / 2
        if (coop.NavClear(x, y, top, x, y, mid)) then hi = mid else lo = mid end
    end
    return hi
end
-- The first of the places pts ({x, y, z, ...}) from which a point of the target at one of the heights
-- dzs is in sight (an eye 3 above the place); coop_note("view", "<place index> <height>") or "none".
function coop.NavView(tx, ty, tz, pts, dzs)
    local i = 1
    while (pts[i]) do
        local k = 1
        while (dzs[k]) do
            if (coop.NavClear(pts[i], pts[i + 1], pts[i + 2] + 3, tx, ty, tz + dzs[k])) then
                coop_note("view", format("%d %.2f", (i + 2) / 3, dzs[k]))
                return
            end
            k = k + 1
        end
        i = i + 3
    end
    coop_note("view", "none")
end
function coop.NavStart(x, y, z, cell, maxcells, up, down)
    local n = { cell = cell or 2, max = maxcells or 20000, up = up or 2.5, down = down or 6, head = 1, tail = 1,
                q = {}, seen = {}, out = {}, batch = 0, count = 0, edges = 0 }
    local i, j = floor(x / n.cell + 0.5), floor(y / n.cell + 0.5)
    local f = coop.NavFloor(i * n.cell, j * n.cell, z + 3, z - 3)
    if (not f) then coop_note("nav_state", "nofloor") return end
    -- the hero's own body blocks the probes' line of sight: links next to where it stands are not
    -- tested (the scan from the middle of level 2's arena saw a wall 2 units south of the hero) --
    n.hx, n.hy, n.hz = x, y, z
    n.q[1] = { i, j, f }
    n.tail = 2
    n.seen[format("%d_%d_%d", i, j, floor(f / 4))] = 1
    coop.nav = n
    coop_note("nav_state", "running")
end
function coop.NavFlush(n, last)
    if (n.out[1] or last) then
        n.batch = n.batch + 1
        local text, k = "", 1
        while (n.out[k]) do text = text .. n.out[k] .. "\n" k = k + 1 end
        coop_note(format("nav_%d", n.batch), text)
        n.out = {}
    end
end
function coop.NavTick()
    local n = coop.nav
    if (not n or n.done) then return end
    local budget = 40
    local dirs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
    while (budget > 0 and n.head < n.tail and n.count < n.max) do
        local c = n.q[n.head]
        n.q[n.head] = nil
        n.head = n.head + 1
        local x, y, z = c[1] * n.cell, c[2] * n.cell, c[3]
        local line = format("c %.1f %.1f %.2f %d %d", x, y, z, c[1], c[2])
        n.count = n.count + 1
        local d = 1
        while (d <= 4) do
            local ni, nj = c[1] + dirs[d][1], c[2] + dirs[d][2]
            local nx, ny = ni * n.cell, nj * n.cell
            local f = coop.NavFloor(nx, ny, z + n.up + 2, z - n.down)
            local mx, my = (x + nx) / 2 - n.hx, (y + ny) / 2 - n.hy
            local byhero = mx * mx + my * my < 9 and abs(z - n.hz) < 4
            if (f and f <= z + n.up and (byhero or (coop.NavClear(x, y, z + 2, nx, ny, f + 2) and
                coop.NavClear(x, y, z + 4, nx, ny, f + 4)))) then
                line = line .. format(" %d %d %.2f", ni, nj, f)
                local key = format("%d_%d_%d", ni, nj, floor(f / 4))
                if (not n.seen[key]) then
                    n.seen[key] = 1
                    n.q[n.tail] = { ni, nj, f }
                    n.tail = n.tail + 1
                end
            end
            d = d + 1
        end
        n.nout = (n.nout or 0) + 1
        n.out[n.nout] = line
        if (n.nout >= 200) then coop.NavFlush(n) n.nout = 0 end
        budget = budget - 1
    end
    if (n.head >= n.tail or n.count >= n.max) then
        coop.NavFlush(n, 1)
        n.done = 1
        coop_note("nav_state", format("done %d %d", n.count, n.batch))
    end
end
-- The gunner: while on, the nearest living enemy the hero can see (engine line of sight) within
-- range is faced and shot with the fire button; the pilot waits meanwhile. Counts shots and targets.
coop.gunner = nil
function coop.GunnerOn(range) coop.gunner = { range = range or 60, shots = 0, engaged = 0, next_ms = 0, skip = {}, kills = 0 } end
function coop.GunnerTick()
    local gn = coop.gunner
    if (not gn) then return end
    local p = coop.real_player()
    local now = coop_ms()
    if (not p or now < gn.next_ms) then return end
    gn.next_ms = now + 100
    local e, k = nil, 1
    while (k <= 8 and not e) do
        local c = coop_nearest_enemy(p.position.x, p.position.y, p.position.z, gn.range, k)
        if (not c) then
            k = 9
        else
            if (not gn.skip[c] and mdkAILineOfSight(p, c, 6.3, gn.range) == 1) then e = c end
            k = k + 1
        end
    end
    if (e) then
        if (gn.target ~= e) then
            gn.engaged = gn.engaged + 1
            gn.target = e
            gn.hp = coop_hp(e)
            gn.since = now
        end
        -- no damage done in 3 s (out of reach, behind cover the line of sight misses): leave it --
        local hp = coop_hp(e)
        if (hp and gn.hp and hp < gn.hp) then gn.hp = hp gn.since = now end
        if (not gn.hp) then gn.hp = hp end
        if (now - gn.since > 3000) then
            gn.skip[e] = 1
            gn.target = nil
            return
        end
        mdkGobPointToXYZ(p, e.position.x - p.position.x, e.position.y - p.position.y, e.position.z - p.position.z)
        coop_press(256, 150)
        gn.shots = gn.shots + 1
        gn.busy = now + 300
    else
        gn.target = nil
    end
end
function coop.PilotStop()
    if (coop.pilot and coop.pilot.state == "go") then coop.pilot.state = "stopped" end
end
function coop.PilotTick()
    local pl = coop.pilot
    if (not pl or pl.state ~= "go") then return end
    local p = coop.real_player()
    if (not p) then return end
    local now = coop_ms()
    if (now < pl.next_ms) then return end
    pl.next_ms = now + 50
    -- the gunner has the hero's aim: wait, and do not count it as stuck --
    if (coop.gunner and (coop.gunner.busy or 0) > now) then pl.best_ms = now return end
    local dx = pl.x - p.position.x
    local dy = pl.y - p.position.y
    local dz = pl.z - p.position.z
    local d = sqrt(dx * dx + dy * dy)
    if (pl.flat == 0) then d = sqrt(dx * dx + dy * dy + dz * dz) end
    pl.d = d
    if (d <= pl.r) then
        pl.state = "arrived"
        pl.ms = now - pl.t0
        return
    end
    -- out of sight: head for a visible stepping point instead, chosen again every 300 ms --
    pl.visits = pl.visits or {}
    local cell = format("%d_%d", floor(p.position.x / 6), floor(p.position.y / 6))
    if (cell ~= pl.cell) then pl.visits[cell] = (pl.visits[cell] or 0) + 1 pl.cell = cell end
    local tx, ty = pl.x, pl.y
    if (pl.explore == 1 and not coop.PilotSees(p, pl.x, pl.y, pl.z + 1.5)) then
        if (not pl.sx or now >= (pl.choose_ms or 0) or
            (pl.sx - p.position.x) * (pl.sx - p.position.x) + (pl.sy - p.position.y) * (pl.sy - p.position.y) < 9) then
            pl.sx, pl.sy = coop.PilotChoose(p, pl)
            pl.choose_ms = now + 300
        end
        if (pl.sx) then tx, ty = pl.sx, pl.sy end
    end
    dx, dy = tx - p.position.x, ty - p.position.y
    if (dx * dx + dy * dy > 0.01) then mdkGobPointToXYZ(p, dx, dy, 0) end
    coop_press(coop.pilot_keys.forward, 120)
    -- a waypoint above a step: jump on the way to it, once, when close --
    if (pl.jump == 1 and d < 3.5 and not pl.jumped) then
        coop_press(coop.pilot_keys.jump, 250)
        pl.jumped = 1
    end
    if (d < pl.best - 0.75) then
        pl.best = d
        pl.best_ms = now
    elseif (now - pl.best_ms > 1500) then
        pl.stuck = pl.stuck + 1
        pl.best_ms = now
        if (pl.stuck - floor(pl.stuck / 2) * 2 == 1) then
            coop_press(coop.pilot_keys.jump, 200)
        elseif (pl.stuck - floor(pl.stuck / 4) * 4 == 2) then
            coop_press(coop.pilot_keys.left, 600)
        else
            coop_press(coop.pilot_keys.right, 600)
        end
        if (pl.stuck >= (pl.max_stuck or 10)) then pl.state = "stuck" end
    end
end

function coop.Tick()
    coop.ControlsCheck()
    -- host autostart (tools/play.sh <level> <cp>): once the title screen has been up a while
    -- and coop.autostart_players others have joined (or a minute has gone by), load the start
    -- checkpoint; retried until a level is running --
    if (coop.autostart and coop.level == 0) then
        coop.ticks = (coop.ticks or 0) + 1
        local joined = coop_players()
        if (coop.ticks >= (coop.nextstart or 600) and
            (joined >= (coop.autostart_players or 0) or coop.ticks >= 3600)) then
            coop.nextstart = coop.ticks + 600
            if (coop.autostart_sid) then
                local rp = coop.ResumePoint(coop.autostart_sid)
                coop_log(format("autostart: resuming co-op session %d, %d others here", coop.autostart_sid, joined))
                if (rp) then coop.ResumeAt(rp) else coop.autostart = nil end
            else
                coop_log(format("autostart: level %d checkpoint %d, %d others here", coop.autostart[1],
                                coop.autostart[2], joined))
                mdkNewGame(coop.autostart[1], coop.autostart[2])
            end
        end
    elseif (coop.autostart and coop.level and coop.level >= 1) then
        coop.autostart = nil
    end
    local p = mdkGetPlayerGob()
    if (coop.PuppetSlot(p)) then p = nil end
    coop_send(p)
    coop_recv()
    coop.HandleEvents()
    coop.HandlerTick()
    if (coop.pilot) then coop.PilotTick() end
    if (coop.nav) then coop.NavTick() end
    if (coop.gunner) then coop.GunnerTick() end
    coop.ReviveTick(p)
    coop.UnstickTick(p)
    coop.CharTick(p)
    coop.EmoteTick(p)
    coop.WaitTick()
    coop.MovieTick()
    coop.HudTick()
    coop.PendingSaveTick()
    if (coop_role() == "host") then coop.SaveTick() end
    local route_blocked = 0
    if ((gamecontrol and gamecontrol.showmenu == 1) or coop.waiting) then route_blocked = 1 end
    coop_route_tick(p, coop.level or 0, mdkGetCurrentCheckPoint(), route_blocked)
    if (not p or not coop.level or coop.level < 1) then return end
    local me = coop_slot()
    local _, slots = coop_players()
    local s = 0
    while (s < slots) do
        if (s ~= me) then coop.UpdatePuppet(p, s) end
        s = s + 1
    end
end

-- the players panel ([hud] show, Options > Co-op > Gameplay): in a level, no menu up --
coop.hud_on = (coop_get("hud", "show", "1") == "1")
coop.hud_outline = (coop_get("hud", "outline", "0") == "1")
function coop.HudTick()
    local show = 0
    if (coop.hud_on and coop.level and coop.level >= 1 and not coop.waiting and not coop.mv and
        not Save.movietocancel and not Save.inminigame and not (gamecontrol and gamecontrol.showmenu == 1)) then
        show = 1
    end
    coop_hud(show, mdkGetCameraGob(), coop.hud_outline and 1 or 0)
end

-- slot whose puppet gob is, or nil --
function coop.PuppetSlot(gob)
    if (not gob) then return nil end
    local s, g = next(coop.puppets, nil)
    while (s) do
        if (g == gob) then return s end
        s, g = next(coop.puppets, s)
    end
    return nil
end

-- one other player: add their puppet once they are in this level, move it, remove it when
-- they leave --
function coop.UpdatePuppet(p, s)
    local gob = coop.puppets[s]
    if (not coop_connected(s)) then
        if (gob) then
            coop_log(format("player %d left: puppet removed", s + 1))
            coop_face(nil, s)
            if (coop_alive(gob)) then omGobDelete(gob) end
            coop.puppets[s] = nil
            coop.parked[s] = nil
            coop.targetable[s] = nil
        end
        return
    end
    local x, y, z = coop_peer(s)
    if (not x) then return end
    if (gob and mdkGetGobType(gob) ~= coop.PuppetType(p, s)) then
        coop_log(format("player %d changed character: puppet replaced", s + 1))
        coop_face(nil, s)
        if (coop_alive(gob)) then omGobDelete(gob) end
        coop.puppets[s] = nil
        coop.parked[s] = nil
        coop.targetable[s] = nil
        gob = nil
    end
    if (not gob) then gob = coop.SpawnPuppet(p, s) end
    if (not gob) then return end
    -- a dead player spectating someone rides along behind them, hidden: park their puppet
    -- out of everyone's way (it is solid) and hide it --
    local parked = coop_life(s) == 3
    if (parked ~= (coop.parked[s] or 0)) then
        omGobGMSetVisible(gob, parked and 0 or 1)
        coop.parked[s] = parked and 1 or 0
    end
    if (parked) then
        mdkGobSetPositionXYZ(gob, p.position.x, p.position.y, p.position.z - 500)
        return
    end
    x, y = coop.KeepApart(p, x, y)
    mdkGobSetPositionXYZ(gob, x, y, z + coop.mirror)
    coop_face(gob, s)
end
coop.parked = {}
coop.targetable = {}

-- going down, reviving, spectating (bridge revive.c): what needs the game's own calls --
function coop.ReviveTick(p)
    local a = coop_revive_tick()
    while (a) do
        coop_log("revive action: " .. a)
        if (a == "down") then
            mdkDisablePlayerControl()
            if (p) then mdkGobSetTargetable(p, 0) end
        elseif (a == "hide") then
            if (p) then omGobGMSetVisible(p, 0) end
        elseif (a == "up") then
            if (p) then
                omGobGMSetVisible(p, 1)
                mdkGobSetTargetable(p, 1)
            end
            mdkEnablePlayerControl()
        elseif (a == "die") then
            -- nobody left to revive us: the game's own death and checkpoint reload --
            if (p) then
                omGobGMSetVisible(p, 1)
                mdkGobSetTargetable(p, 1)
                mdkEnablePlayerControl()
                mdkDealDamage(nil, p, 9999, DAMAGE_BADGUY, -1)
            end
        elseif (a == "wipe") then
            -- host: everyone is down; the clients follow the load --
            coop_log(format("everyone down: reloading level %d checkpoint %d", coop.level or 0, coop.cp or 0))
            mdkNewGame(coop.level, coop.cp)
        end
        a = coop_revive_tick()
    end
end

-- The puppet is solid and moved every frame. Two characters collide at about 0.88 units (measured at 1-1:
-- closer, the engine moves our player out to 0.88; at 1 or more nothing moves), so the puppet stays where its
-- player is and is only nudged out to coop.apart (horizontally, z is up) when the two would overlap, as they do
-- when both spawn on the same point. It used to be kept 3 away, which made the other player slide off as you
-- came near. --
coop.apart = 1
function coop.KeepApart(p, x, y)
    local dx = x - p.position.x
    local dy = y - p.position.y
    local d2 = dx * dx + dy * dy
    if (d2 >= coop.apart * coop.apart) then return x, y end
    if (d2 < 0.0001) then
        return p.position.x + coop.apart, y
    end
    local k = coop.apart / sqrt(d2)
    return p.position.x + dx * k, p.position.y + dy * k
end

-- the puppet has an empty OnUpdate: a gob with a Lua OnUpdate skips its class think,
-- so the game's Kurt logic leaves it alone --
function coop.PuppetOnUpdate(gob)
end

-- the puppet is the character its player plays (they can change at any time: the character wheel);
-- a type that is not Kurt, Max or Doc gets ours --
function coop.PuppetType(p, s)
    local peer = coop_peer_type(s)
    if (peer == OBJ_KURT or peer == OBJ_MAX or peer == OBJ_DOC) then return peer end
    return mdkGetGobType(p)
end

function coop.SpawnPuppet(p, s)
    local ptype = coop.PuppetType(p, s)
    local peer = coop_peer_type(s)
    if (peer and peer ~= ptype) then
        coop_log(format("player %d plays type %d, we use %d for the puppet", s + 1, peer, ptype))
    end
    local gob = mdkCreateObjectLua(format("coop_p%d", s + 1), ptype, mdkGetScene(), nil, nil, 0,0,0, 1,0,0,0, nil,0,0,0,0)
    if (not gob) then
        coop_log(format("puppet for player %d (type %d) failed", s + 1, ptype))
        return nil
    end
    mdkSetLuaEvent(gob, "OnUpdate", coop.PuppetOnUpdate)
    coop.puppets[s] = gob
    coop_log(format("puppet spawned for player %d (type %d)", s + 1, ptype))
    return gob
end

-- every level load (menu, script, save, debug) ends in level(): reset the bridge for the
-- new level and, on the host, ask the client to follow; Tick adds the puppet --
coop.level_orig = level
function level(l, cp, sec)
    -- the level's own script starts its movie inside this call: the movie state is reset before it --
    coop.mv = nil
    coop.mvr = {}
    coop.mvgo = {}
    coop.mvcount = 0
    coop.level_ready = nil
    coop.fallghost = {}
    coop.fallpos = {}
    coop.fallmis = {}
    coop.fallactive = nil
    coop.level_ms = coop_ms()
    coop_banner()
    coop_movie_box()
    coop.NewLevelChar()
    coop.level_orig(l, cp, sec)
    -- only the host's level changes count: both games run the same scripts, so script-driven
    -- loads (new game: 0 -> 1,9 -> 1,12 -> 1,1) happen on both and must not echo --
    local announce = 0
    if (coop_role() == "host" and l >= 1) then announce = 1 end
    local save = 0
    local loaded = coop.loading
    coop.loading = nil
    if (mdkLoadLevelIsInstant() == 1) then
        -- an instant load brings back the puppets that were in the save, beside the new ones
        -- we are about to make --
        local n = coop_purge_puppets()
        if (n > 0) then coop_log(format("%d puppets from the save removed", n)) end
    end
    if (loaded and l == loaded.level) then
        -- one of our co-op saves (mdkGetCurrentCheckPoint says -1 after an instant load); the
        -- host asks the clients to load their copies of it --
        cp = loaded.cp
        save = loaded.no
        coop_log(format("co-op save %d loaded (level %d checkpoint %d)", save, l, cp))
    end
    -- the host's session: a new game (anything but one of our saves, from the title screen)
    -- starts one; a co-op save loaded goes on with its own --
    if (coop_role() == "host" and l >= 1) then
        if (loaded and l == loaded.level and loaded.sid) then
            coop.sid = loaded.sid
        elseif (coop.resume_sid) then
            coop.sid = coop.resume_sid
        elseif (not coop.sid or (coop.level or 0) < 1) then
            coop.sid = coop_session_new()
            coop_log(format("new co-op session %d", coop.sid))
        end
    end
    -- a checkpoint started afresh (not a save, not the autosave after a death) has no inventory --
    local fresh = (cp >= 0 and save == 0)
    -- checkpoint -1 is a reload from the autosave (after a death): tell the client which
    -- checkpoint that is instead; it cannot load our save --
    if (cp < 0) then cp = mdkGetCurrentCheckPoint() end
    if (cp < 0) then announce = 0 end
    coop.resume_sid = nil
    coop.following = nil
    coop.puppets = {}
    coop.parked = {}
    coop.targetable = {}
    coop.rewards = {}
    coop.cp_given = 0
    coop.rear_gate_result = nil
    coop.level = l
    coop.cp = cp
    if (fresh) then coop.CheckpointItems(l, cp) end
    coop_level(l, cp, announce, save)
    coop.level_ready = 1
    coop.HookMovies()
    coop.HookDomePuzzle()
    -- host: how far the session has got (not when a save brought us back here) --
    if (announce == 1 and save == 0) then coop_progress(l, cp, coop.sid) end
    -- a client that loaded a save has a world from back then: the host sends its own again --
    if (save > 0 and coop_role() ~= "host") then coop_ready() end
    coop.ScheduleSave(10)
end

-- ---------------------------------------------------------------------------------------
-- Cutscenes. Every game runs the level's movie scripts itself (StartMovie), so the movies
-- need keeping together, not copying: the ones that start with a level load (and the ones those
-- start from their Done) begin on a shared go from the host, skipping is a vote (the game's
-- abort key, Backspace: a majority of those watching skips for everyone) and the movie's Done,
-- which usually loads the next level, waits until everyone watching has seen the end (or left) or the vote
-- has skipped it. The wait for everyone to be ready at the start has a countdown; a vote stays until the movie
-- ends; the game's own skip (Esc) is a vote too.
-- The banner counts down the time the movie has left, once its length is known (coop.mvlen: measured,
-- or learned the first time it plays to the end). The host tallies and rules; the clients report
-- (bridge/movie.c carries the EV_MOVIE events).
-- ---------------------------------------------------------------------------------------

EV_MOVIE = 20
MV_START = 1        -- client -> host: my movie is ready; type = its serial
MV_GO = 2           -- host -> all (or one late player): play
MV_VOTE = 3         -- client -> host: damage 1 = skip, 0 = take it back
MV_STATE = 4        -- host -> all: the tally, for the banner (see coop.MovieBroadcast)
MV_SKIP = 5         -- host -> all: the vote passed
MV_DONE = 6         -- client -> host: my movie reached its end and waits
MV_FINISH = 7       -- host -> all: everyone has seen the end (or ran out of time): run Done
MV_BEGIN = 8        -- host -> all: start movie `name` (a function in coop.moviehooks); type = the serial it has here
MV_FALL = 9         -- the skydive: where a player's Kurt is (x, y; damage = its dead state; extra 0 = whose, from the host)
MV_FALLMIS = 10     -- the skydive: where the homing missile chasing a player is (x, y, z; damage 1 = flying, 0 = none)

coop.mv_start_ms = 20000    -- longest wait for the others to load and be ready
coop.mv_trigger_ms = 4000   -- a movie the host's trigger started: the others are told at once, so a short wait
coop.mv_load_ms = 15000     -- a movie that starts this soon after a level load is one every game plays
coop.mv_box_ms = 4000       -- the cutscene box shows this long after a key press
coop.movietimer = (coop_get("hud", "movietimer", "1") ~= "0")    -- the time left in the box
coop.mvr = {}               -- host: reports by serial: coop.mvr[serial][slot] = { start, done, vote }
coop.mvgo = {}              -- client: serials the host has already sent GO for
coop.mvcount = 0
coop.mv = nil               -- the movie this game is in
-- movie lengths in ms of play (waits and minigames left out), by coop.MovieKey, measured by
-- tools/measure_movies.sh (the checkpoint it was loaded from, which may have led into it); coop.ini [movies]
-- adds the ones this game has seen to the end --
coop.mvlen = {
    ["camera_134_68156"] = 61504,          -- 1-9
    ["l0a_56_28075"] = 112089,             -- 1-9
    ["ml1b_camera_28_10231"] = 35467,      -- 1-9
    ["camera_76_32433"] = 35716,           -- 1-10
    ["camera_80_30270"] = 54500,           -- 1-10
    ["ml2b_cameragob_215_112746"] = 57051, -- 1-10
    ["camera_94_44515"] = 67407,           -- 2-14
    ["camera_98_42800"] = 50006,           -- 2-14
    ["camera_185_108232"] = 71652,         -- 3-14
    ["camera_27_11537"] = 15931,           -- 3-14
    ["camera_108_41885"] = 47071,          -- 4-13
    ["ml5a_camera_131_74245"] = 52875,     -- 4-13
    ["camera_116_65182"] = 42525,          -- 4-15
    ["camera_78_35021"] = 34846,           -- 5-12
    ["camera_77_34620"] = 48037,           -- 5-13
    ["camera_97_51857"] = 40075,           -- 5-13
    ["camera_84_46413"] = 62035,           -- 6-9
    ["camera_83_42671"] = 42133,           -- 7-11
    ["camera_93_46684"] = 51284,           -- 7-12
    ["camera_136_49575"] = 62934,          -- 8-8
    ["camera_68_33424"] = 48142,           -- 8-9
    ["camera_127_70103"] = 55331,          -- 8-10
    ["ml9a_camera_106_37192"] = 68010,     -- 8-10
    ["camera_109_57520"] = 55427,          -- 9-14
    ["camera_62_28602"] = 33324,           -- 9-14
    ["_15_2720"] = 329058,                 -- 10-8
    ["camera_57_25907"] = 26685,           -- 10-8
    ["l0z_33_13350"] = 56099,              -- 10-8
    ["camera_159_109543"] = 77497,         -- 10-9
    ["l0x_31_15090"] = 48731,              -- 11-2
    ["l0y_25_11620"] = 42519,              -- 12-2
}

coop.start_orig = StartMovie
coop.stop_orig = StopScript
coop.abort_orig = comfuncs.game[COM_ABORTMOVIE]

-- movies are kept together only while someone else is here --
function coop.MovieOthers()
    local n = coop_players()
    return n and n > 0
end

function StartMovie(self, sc)
    coop.start_orig(self, sc)
    if (self.stack and self.stack.movie) then coop.MovieStarted(self, sc) end
end

function coop.MovieStarted(gob, sc)
    local since = coop_ms() - (coop.level_ms or 0)
    -- a movie of the level load (or from another movie's Done) is one every game plays by itself; one a
    -- trigger starts is only this game's, unless the host announced it (coop.MovieTrigger) --
    local loadmovie = (since < coop.mv_load_ms or coop.in_done)
    if (not (coop.MovieOthers() and not inmovieplayer and (loadmovie or coop.begin_key or coop.remote_run))) then
        coop.mv = nil
        return
    end
    coop.mvcount = coop.mvcount + 1
    local m = {
        serial = coop.mvcount, gob = gob, sc = sc, state = "wait", at = coop_ms(),
        skippable = (sc.Done ~= nil), s = nil, played = 0,
    }
    m.key = coop.MovieKey(m)
    m.len = coop.mvlen[m.key] or tonumber(coop_get("movies", m.key, ""))
    coop.mv = m
    if (sc.Done) then
        local orig = sc.Done
        sc.Done = function() coop.MovieDone(%m, %orig) end
    end
    local wait = coop.mv_start_ms
    if (not loadmovie) then wait = coop.mv_trigger_ms end
    local r = coop.MovieReports(m.serial)
    if (coop_role() == "host") then
        r[0] = { start = 1 }
        m.wait_until = m.at + wait
        PauseScript(gob)
        coop_log(format("movie %d: waiting for the others", m.serial))
        if (coop.begin_key) then
            -- the others play it too, from their copy of the function --
            coop_movie_send(MV_BEGIN, 0, m.serial, -1, 0, 0, 0, 0, 0, coop.begin_key)
            coop.begin_key = nil
        end
    elseif (coop.mvgo[m.serial]) then
        coop.MovieGo(m)     -- an early GO needs the same playback clock as one received while waiting
        coop_log(format("movie %d: the host had already said go", m.serial))
    else
        PauseScript(gob)
        m.wait_until = m.at + wait + 5000
        m.need_start = 1        -- sent from the tick: our level epoch is not set until the load call returns
        coop_log(format("movie %d: ready, waiting for the host", m.serial))
    end
end

-- a movie's name: the gob that plays it, its number of steps and the sum of its animation ids and waits (many
-- movies play on the camera gob). Not the level: a movie of a level load starts before coop.level is set --
function coop.MovieKey(m)
    local n, sum = 0, 0
    while (m.sc[n + 1]) do
        n = n + 1
        local f, args = m.sc[n][1], m.sc[n][2]
        if (type(args) == "table") then
            if ((f == omAnimPlay or f == omAnimJustLooped) and type(args[2]) == "number") then sum = sum + args[2]
            elseif (f == Wait and type(args[2]) == "number") then sum = sum + args[2] end
        end
    end
    return format("%s_%d_%d", m.gob.name or "?", n, floor(sum * 10 + 0.5))
end

function coop.MovieReports(serial)
    if (not coop.mvr[serial]) then coop.mvr[serial] = {} end
    return coop.mvr[serial]
end

-- the movie's end: with others watching, wait for them --
function coop.MovieDone(m, orig)
    if (coop.mv ~= m or m.skipping or not coop.MovieOthers()) then
        coop.RunDone(orig)
        return
    end
    coop.MovieLearn(m)
    m.state = "hold"
    m.finish = orig
    m.hold_at = coop_ms()
    if (coop_role() == "host") then
        coop.MovieReports(m.serial)[0].done = 1
    else
        coop_movie_send(MV_DONE, 0, m.serial, -1, 0, 0, 0, 0, 0, "")
    end
    coop_log(format("movie %d: ended here, waiting for the others", m.serial))
end

-- played to its end, nothing skipped: now its length is known --
function coop.MovieLearn(m)
    -- under a second is no movie to count down (the death camera's one-step script) --
    if (not m.go_at or m.skipped or m.played < 1000) then return end
    coop_log(format("movie %d: length %s %d", m.serial, m.key, m.played))
    coop.mvlen[m.key] = m.played
    coop_set("movies", m.key, format("%d", m.played))
end

-- a movie started by a Done is the same movie session: it is kept together too --
function coop.RunDone(f)
    local before = coop.in_done
    coop.in_done = 1
    f()
    coop.in_done = before
end

-- Backspace (the game's abort key): a skip vote while others watch, else the game's own --
comfuncs.game[COM_ABORTMOVIE] = function()
    local m = coop.mv
    if (not Save.movietocancel and coop_get("controls", "devkeys", "0") == "1") then
        coop_emote(-1)                  -- the emote browser's stop (dev keys)
    end
    if (not m or not coop.MovieOthers()) then
        coop.abort_orig()
        return
    end
    if (m.state ~= "play" or not m.skippable or not Save.movietocancel or Save.inminigame) then return end
    m.myvote = 1 - (m.myvote or 0)
    coop_log(format("movie %d: my skip vote %d", m.serial, m.myvote))
    if (coop_role() == "host") then
        coop.MovieReports(m.serial)[0].vote = m.myvote
    else
        coop_movie_send(MV_VOTE, m.myvote, m.serial, -1, 0, 0, 0, 0, 0, "")
    end
end

-- The engine's own skip (Esc: it calls StopScript on the movie mid-way, no Lua command on the way) would end the
-- movie for this player alone. While others watch, a stop in the middle of the co-op movie that follows a key
-- press is a skip vote instead (not a second press: it does not take a vote back). A movie's own end (the
-- script runner calls StopScript past its last step) and the game's scripted stops (no key) go through --
function StopScript(self)
    local m = coop.mv
    if (m and self == m.gob and m.state == "play" and not m.skipping and coop.MovieOthers() and
        self.stack and self.stack.movie and self.stack.script[self.stack.nexttask]) then
        local code, ago = coop_last_press()
        if (code and ago < 1000) then
            coop_log(format("movie %d: the game's skip (key %d) counts as a vote", m.serial, code))
            if (m.myvote ~= 1) then comfuncs.game[COM_ABORTMOVIE]() end
            return 0
        end
    end
    return coop.stop_orig(self)
end

-- the host's count: who is here, who wants to skip, who has seen the end --
function coop.MovieTally(m)
    local r = coop.MovieReports(m.serial)
    local _, slots = coop_players()
    local parts, yes, done, votes, s = 0, 0, 0, 0, 0
    while (s < slots) do
        local e = r[s]
        if (e and e.start and (s == 0 or coop_connected(s))) then
            parts = parts + 1
            if (e.done) then
                done = done + 1
                yes = yes + 1
            elseif (e.vote == 1) then
                yes = yes + 1
                votes = votes + 1
            end
        end
        s = s + 1
    end
    return parts, yes, done, votes
end

function coop.MovieClear(why)
    if (coop.mv) then coop_log(format("movie %d: over (%s)", coop.mv.serial, why)) end
    coop.mv = nil
    coop_banner()
    coop_movie_box()
end

-- the host's movie is the one the others follow: carry out a skip --
function coop.MovieSkip(m)
    coop_log(format("movie %d: skipped by vote", m.serial))
    m.skipping = 1
    m.skipped = 1
    m.myvote = 0
    local r = coop.MovieReports(m.serial)
    local s = 0
    while (s < 16) do
        if (r[s]) then r[s].vote = nil end
        s = s + 1
    end
    local g = m.gob
    if (g.stack and g.stack.movie) then
        coop.RunDone(function() coop.stop_orig(%g) end)
    end
    m.skipping = nil
end

function coop.MovieGo(m)
    m.state = "play"
    m.go_at = coop_ms()
    m.tick_at = m.go_at
    if (m.gob.stack) then ResumeScript(m.gob) end
    coop_log(format("movie %d: go", m.serial))
end

function coop.MovieFinish(m)
    local f = m.finish
    m.finish = nil
    coop_log(format("movie %d: everyone has seen the end", m.serial))
    coop.MovieClear("finished")
    if (f) then coop.RunDone(f) end
end

-- host: the tally goes to everyone for their banners --
function coop.MovieBroadcast(m, parts, yes, done)
    local now = coop_ms()
    local sleft = -1
    if (m.state == "wait") then sleft = (m.wait_until - now) / 1000 end
    coop_movie_send(MV_STATE, yes, m.serial, -1, -1, -1, sleft, done, parts, m.state)
end

function coop.MovieHostTick(m)
    local now = coop_ms()
    local r = coop.MovieReports(m.serial)
    local parts, yes, done, votes = coop.MovieTally(m)
    local needed = floor(parts / 2) + 1
    if (m.state == "wait") then
        -- everyone who is in the level has to be ready --
        local _, slots = coop_players()
        local ready, s = 1, 1
        while (s < slots) do
            if (coop_connected(s) and coop_player_in_level(s) and not (r[s] and r[s].start)) then ready = nil end
            s = s + 1
        end
        if (ready or now >= m.wait_until) then
            coop.MovieGo(m)
            coop_movie_send(MV_GO, 0, m.serial, -1, 0, 0, 0, 0, 0, "")
            m.bcast = 0
        end
    elseif (m.state == "play") then
        if (votes > 0 and yes >= needed) then
            coop_movie_send(MV_SKIP, 0, m.serial, -1, 0, 0, 0, 0, 0, "")
            coop.MovieSkip(m)
            return
        end
    elseif (m.state == "hold") then
        -- everyone who is watching has seen the end (or left): a player paused in a menu holds everyone --
        if (done >= parts) then
            coop_movie_send(MV_FINISH, 0, m.serial, -1, 0, 0, 0, 0, 0, "")
            coop.MovieFinish(m)
            return
        end
    end
    if (not m.bcast or now >= m.bcast or m.last_yes ~= yes or m.last_done ~= done) then
        m.last_yes = yes
        m.last_done = done
        m.bcast = now + 500
        coop.MovieBroadcast(m, parts, yes, done)
    end
end

function coop.MovieClientTick(m)
    local now = coop_ms()
    if (m.need_start and coop.level_ready) then
        m.need_start = nil
        coop_movie_send(MV_START, 0, m.serial, -1, 0, 0, 0, 0, 0, "")
    end
    local silent, hlevel = coop_host_status()
    -- the host is gone or never answers: carry on alone --
    if (m.state == "wait" and (now >= m.wait_until + 40000 or silent > 5000 or hlevel < 1)) then
        coop_log(format("movie %d: no go from the host, playing", m.serial))
        coop.MovieGo(m)
    elseif (m.state == "hold" and (silent > 5000 or hlevel < 1)) then
        coop_log(format("movie %d: the host is gone, continuing", m.serial))
        coop.MovieFinish(m)
    end
end

function coop.MovieBanner(m)
    local now = coop_ms()
    local s = m.s
    local l1, l2, prog = "", "", -1
    local b1, b2, b3 = "", "", ""
    if (m.state == "wait") then
        l1 = "Waiting for the other players..."
        local left = (m.wait_until - now) / 1000
        if (coop_role() ~= "host") then left = left - 5 end
        if (left > 0) then l2 = format("starting in %d s", floor(left) + 1) end
    elseif (m.state == "hold") then
        local parts, done = 0, 0
        if (coop_role() == "host") then
            local p, _, d = coop.MovieTally(m)
            parts, done = p, d
        elseif (s) then
            parts, done = s.parts, s.done
        end
        l1 = "Waiting for the others to finish the cutscene..."
        if (parts > 0) then l1 = format("Waiting for the others to finish the cutscene (%d of %d)", done, parts) end
    elseif (m.state == "play" and not Save.inminigame) then
        -- the box at the bottom right: the time the movie has left, the skip key and the vote. It shows
        -- for a while after the player presses a key other than the skip key (so it stays out of the
        -- picture for those just watching), and while anyone has voted --
        local canskip = m.skippable and Save.movietocancel
        local yes, needed = 0, 2
        if (canskip) then
            if (coop_role() == "host") then
                local p, y = coop.MovieTally(m)
                yes, needed = y, floor(p / 2) + 1
            elseif (s) then
                yes, needed = s.yes, s.needed
            end
        end
        local key = coop.Keys("skip")
        local code, ago = coop_last_press()
        local pressed = code and ago < coop.mv_box_ms and ago < now - (m.go_at or m.at) and
                        code ~= key[1] and code ~= key[2]
        if (pressed or yes > 0) then
            b1 = "Cutscene"
            if (not coop.movietimer) then
                b1 = ""
            elseif (m.len and m.len > 0) then
                local left = (m.len - m.played) / 1000
                if (left < 0) then left = 0 end
                b1 = format("Cutscene  %d:%02d left", floor(left / 60), floor(left) - floor(left / 60) * 60)
            end
            if (canskip) then
                local kn = coop.KeyText(key)
                if (m.myvote == 1) then
                    b2 = format("You voted to skip  (%s again: take it back)", kn)
                else
                    b2 = format("%s: vote to skip", kn)
                end
                if (yes > 0) then b3 = format("Skip votes: %d of %d", yes, needed) end
            end
        end
    end
    coop_banner(l1, l2, prog)
    coop_movie_box(b1, b2, b3)
end

function coop.MovieTick()
    local m = coop.mv
    if (not m) then
        if (coop.fallactive) then coop.FallClear() end
        return
    end
    if (not coop.MovieOthers()) then
        -- everyone left: carry on alone --
        if (m.state == "wait") then
            coop.MovieGo(m)
        elseif (m.state == "hold") then
            coop.MovieFinish(m)
            return
        end
        if (not (m.gob.stack and m.gob.stack.movie)) then coop.MovieClear("alone") end
        coop_banner()
        coop_movie_box()
        return
    end
    if (m.state ~= "hold" and m.state ~= "wait" and not (m.gob.stack and m.gob.stack.movie)) then
        -- ended by itself (no Done to wait for; with others here Backspace only votes) --
        coop.MovieLearn(m)
        coop.MovieClear("ended")
        return
    end
    if (m.state == "play") then
        -- time played: the minigame inside a movie (the skydive) does not count --
        local now = coop_ms()
        if (not Save.inminigame) then m.played = m.played + (now - m.tick_at) end
        m.tick_at = now
    end
    if (coop_role() == "host") then coop.MovieHostTick(m) else coop.MovieClientTick(m) end
    if (coop.mv == m) then coop.MovieBanner(m) else coop_banner(); coop_movie_box() end
    coop.FallTick()
end

-- an EV_MOVIE: op, a (damage), serial (type), from (netid), then the numbers --
function coop.MovieEvent(name, a, serial, op, from, x, y, z, e0, e1)
    local m = coop.mv
    if (coop_role() == "host") then
        local r = coop.MovieReports(serial)
        if (not r[from]) then r[from] = {} end
        if (op == MV_START) then
            r[from].start = 1
            if (m and m.serial == serial and m.state ~= "wait") then
                -- a late player: it need not wait for anyone --
                coop_movie_send(MV_GO, 0, serial, from, 0, 0, 0, 0, 0, "")
            end
        elseif (op == MV_VOTE) then
            r[from].vote = a
        elseif (op == MV_DONE) then
            r[from].done = 1
        elseif (op == MV_FALL) then
            -- on to the other clients, with whose it is --
            coop.FallHear(from, a, x, y)
            local _, slots = coop_players()
            local s = 1
            while (s < slots) do
                if (s ~= from and coop_connected(s)) then coop_movie_send(MV_FALL, a, serial, s, x, y, 0, from, 0, "") end
                s = s + 1
            end
        elseif (op == MV_FALLMIS) then
            coop.FallMissHear(from, a, x, y, z)
            local _, slots = coop_players()
            local s = 1
            while (s < slots) do
                if (s ~= from and coop_connected(s)) then coop_movie_send(MV_FALLMIS, a, serial, s, x, y, z, from, 0, "") end
                s = s + 1
            end
        end
        return
    end
    if (op == MV_GO) then
        coop.mvgo[serial] = 1
        if (m and m.serial == serial and m.state == "wait") then coop.MovieGo(m) end
    elseif (op == MV_STATE) then
        if (m and m.serial == serial) then
            m.s = { yes = a, needed = floor(e1 / 2) + 1, sleft = z, done = e0, parts = e1, at = coop_ms() }
        end
    elseif (op == MV_SKIP) then
        if (m and m.serial == serial and m.state == "play") then coop.MovieSkip(m) end
    elseif (op == MV_FINISH) then
        if (m and m.serial == serial and m.state == "hold") then coop.MovieFinish(m) end
    elseif (op == MV_BEGIN) then
        coop.MovieBegin(name, serial)
    elseif (op == MV_FALL) then
        coop.FallHear(e0, a, x, y)
    elseif (op == MV_FALLMIS) then
        coop.FallMissHear(e0, a, x, y, z)
    end
end

-- The minigames (level 1's skydive, the torpedo runs of levels 2, 5 and 9): everyone plays their own copy and sees the
-- others as ghosts of the player's own ship (the skydive: the cutscene's stunt Kurt) at the places they send, 8 times a
-- second, through the host. A player who is hit drops out of the others' sky until their next try.
-- Per level: the gob the player steers, the ghost's model, and how its two numbers become a place.
coop.fallspec = {
    [1] = { name = "ktgame_kurt", model = "ml1_stuntkurt" },
    [2] = { name = "torpgob", model = "torpgob" },
    [5] = { name = "podgob", model = "podgob" },
    [9] = { name = "jdgob", model = "jdgob" },
}
coop.fall_ms = 125
coop.fallghost = {}     -- slot -> ghost gob
coop.fallpos = {}       -- slot -> { x, y, dead } last heard
coop.fallseq = 0
coop.fallmis = {}       -- slot -> { hx, hy, hz, vx, vy, vz, at, gob }: the missile chasing that player (level 1)

-- the two numbers that place a ship, and the place they give --
function coop.FallNumbers(l, g)
    if (l == 1) then return g.x, g.y end
    return g.x or 0, g.z
end

function coop.FallPlace(l, g, a, b)
    if (l == 1) then
        mdkGobSetPositionXYZ(g, a, b, ktgame_kurt.z)
    elseif (l == 2) then
        mdkGobSetPositionXYZ(g, 1012, 500, b)
    elseif (l == 5) then
        mdkGobSetPositionXYZ(g, a, 100, b)
    else
        mdkGobSetPositionXYZ(g, a, -1000, b)
    end
end

function coop.FallHear(slot, dead, x, y)
    if (slot == coop_slot()) then return end
    local p = coop.fallpos[slot]
    if (not p) then
        p = { x = x, y = y }
        coop.fallpos[slot] = p
    end
    p.tx = x
    p.ty = y
    p.dead = dead
end

function coop.FallClear()
    local s, g = next(coop.fallghost, nil)
    while (s) do
        if (g and coop_alive(g)) then omGobDelete(g) end
        s, g = next(coop.fallghost, s)
    end
    coop.fallghost = {}
    coop.fallpos = {}
    s, g = next(coop.fallmis, nil)
    while (s) do
        if (g.gob and coop_alive(g.gob)) then omGobDelete(g.gob) end
        s, g = next(coop.fallmis, s)
    end
    coop.fallmis = {}
    coop.fallmis_sent = nil
    coop.fallactive = nil
    coop.fall_next = nil
end

function coop.FallTick()
    local m = coop.mv
    local l = coop.level
    local spec = coop.fallspec[l or 0]
    local own = spec and getglobal(spec.name)
    if (not (spec and own and type(own) ~= "number" and Save.inminigame and m and m.state == "play" and
             (own.x or own.z))) then
        if (coop.fallactive) then coop.FallClear() end
        return
    end
    coop.fallactive = 1
    local now = coop_ms()
    if (not coop.fall_next or now >= coop.fall_next) then
        coop.fall_next = now + coop.fall_ms
        local a, b = coop.FallNumbers(l, own)
        coop_movie_send(MV_FALL, own.dead or 0, m.serial, -1, a, b, 0, 0, 0, "")
        if (l == 1) then
            -- the homing missile the game fires at our Kurt (a bullet: the engine moves it, so read its place) --
            local mis = kurtgame and kurtgame.lastmis
            local x, y, z
            if (mis and type(mis) ~= "number" and not mis.exploding) then x, y, z = coop_pose(mis) end
            if (x) then coop_movie_send(MV_FALLMIS, 1, m.serial, -1, x, y, z, 0, 0, "")
            elseif (coop.fallmis_sent) then coop_movie_send(MV_FALLMIS, 0, m.serial, -1, 0, 0, 0, 0, 0, "") end
            coop.fallmis_sent = x
        end
    end
    if (not coop.Feature("ghosts")) then
        coop.FallHide()
        return
    end
    local ease = chGetDeltaT() * 12
    if (ease > 1) then ease = 1 end
    local s, p = next(coop.fallpos, nil)
    while (s) do
        local g = coop.fallghost[s]
        if (p.dead ~= 0 and g) then
            if (coop_alive(g)) then omGobDelete(g) end
            coop.fallghost[s] = nil
            g = nil
        elseif (p.dead == 0 and not g) then
            coop.fallseq = coop.fallseq + 1
            p.x = p.tx
            p.y = p.ty
            -- facing the way our own ship (or Kurt) faces: the skydiving Kurt is tilted, not upright --
            local _, _, _, qw, qx, qy, qz = coop_pose(own)
            if (not qw) then qw, qx, qy, qz = 1, 0, 0, 0 end
            g = mdkCreateObjectLua(format("coop_fall%d_%d", s, coop.fallseq), OBJ_SCENERY, mdkGetScene(), nil, nil,
                                   0, 0, 0, qw, qx, qy, qz, spec.model,0,0,0,0)
            if (g) then
                if (l == 1) then omAnimPlay(g, ANIM_ACTION19, 0) else omAnimPlay(g, ANIM_ACTION00, ANIMFLAG_NOTRANS) end
                if (l == 2 or l == 9) then mdkGobSetOrientation(g, PI/2, 0, 0) end
                coop.fallghost[s] = g
                coop_log(format("minigame: player %d plays beside you (level %d)", s + 1, l))
            end
        end
        if (g and p.dead == 0) then
            p.x = p.x + (p.tx - p.x) * ease
            p.y = p.y + (p.ty - p.y) * ease
            coop.FallPlace(l, g, p.x, p.y)
        end
        s, p = next(coop.fallpos, s)
    end
    coop.FallMissiles(now)
end

-- the ghosts switched off ([features] ghosts=0): none drawn; where the others are is still heard --
function coop.FallHide()
    local s, g = next(coop.fallghost, nil)
    while (s) do
        if (g and coop_alive(g)) then omGobDelete(g) end
        s, g = next(coop.fallghost, s)
    end
    coop.fallghost = {}
    s, g = next(coop.fallmis, nil)
    while (s) do
        if (g.gob and coop_alive(g.gob)) then omGobDelete(g.gob) end
        g.gob = nil
        s, g = next(coop.fallmis, s)
    end
end

-- the missiles chasing the others: harmless ghosts, carried on along their last heading between reports --
function coop.FallMissHear(slot, flying, x, y, z)
    if (slot == coop_slot()) then return end
    local p = coop.fallmis[slot]
    local now = coop_ms()
    if (flying == 0) then
        if (p and p.gob and coop_alive(p.gob)) then omGobDelete(p.gob) end
        coop.fallmis[slot] = nil
        return
    end
    if (not p) then
        p = { vx = 0, vy = 0, vz = 0 }
        coop.fallmis[slot] = p
    elseif (now > p.at) then
        local dt = (now - p.at) / 1000
        p.vx, p.vy, p.vz = (x - p.hx) / dt, (y - p.hy) / dt, (z - p.hz) / dt
    end
    p.hx, p.hy, p.hz, p.at = x, y, z, now
end

-- a missile ghost faces the way it flies. The missile model's nose is its +z (mdkGobPointToXYZ turns +y
-- that way, which laid it on its side): the rotation from +z to the heading is
-- (1 + d.z, -d.y, d.x, 0), normalised --
function coop.FallMissFace(g, x, y, z, vx, vy, vz)
    local n = sqrt(vx * vx + vy * vy + vz * vz)
    if (n < 0.0001) then return end
    vx, vy, vz = vx / n, vy / n, vz / n
    if (vz < -0.9999) then coop_set_quat(g, 0, 1, 0, 0) else coop_set_quat(g, 1 + vz, -vy, vx, 0) end
end

function coop.FallMissiles(now)
    local s, p = next(coop.fallmis, nil)
    while (s) do
        if (not (p.gob and coop_alive(p.gob))) then
            coop.fallseq = coop.fallseq + 1
            p.gob = mdkCreateObjectLua(format("coop_fallmis%d_%d", s, coop.fallseq), OBJ_SCENERY, mdkGetScene(), nil, nil,
                                       p.hx, p.hy, p.hz, 1,0,0,0, "ktgame_missl0",0,0,0,0)
            if (p.gob) then
                omAnimPlay(p.gob, ANIM_ACTION00, ANIMFLAG_NOTRANS)
                coop_log(format("minigame: player %d's missile flies beside you", s + 1))
            end
        end
        if (p.gob) then
            -- no more than half a second ahead of the last report --
            local ahead = (now - p.at) / 1000
            if (ahead > 0.5) then ahead = 0.5 end
            local x, y, z = p.hx + p.vx * ahead, p.hy + p.vy * ahead, p.hz + p.vz * ahead
            mdkGobSetPositionXYZ(p.gob, x, y, z)
            if (p.vx ~= 0 or p.vy ~= 0 or p.vz ~= 0) then coop.FallMissFace(p.gob, x, y, z, p.vx, p.vy, p.vz) end
        end
        s, p = next(coop.fallmis, s)
    end
end

-- Movies a trigger starts (the host shot the locks, walked into the room...) happen in the host's game
-- alone, and the others would only see its player stand still. The game's functions that start them are
-- wrapped: when the host's one starts a movie, the host tells the others (MV_BEGIN) and they play the
-- same movie from their copy of the function, kept together like the rest. A game whose own trigger fires
-- first just plays it (and ignores the announcement).
coop.moviehooks = {
    { "Level", { "PlayBossMovie", "PlayEndMovie", "PlayIntroMovie", "BridgeMovie", "PlayDeathMovie", "PlayEndComic", "KillZizzy" } },
    { "l1_r6", { "StartCutScene" } }, { "l1_r8", { "StartCutSceneOpening" } }, { "l2_r11", { "StartCutSceneBoss" } },
    { "l7r5", { "SendInTheTroops" } }, { "l3_bathroom03", { "StartCutScene" } }, { "l3_w_hub00", { "StartCutScene" } },
}
coop.mvwrap = {}        -- "table.function" -> our wrapper
coop.mvorig = {}        -- "table.function" -> the game's function
coop.mvran = {}         -- "table.function" -> when it last ran here (ms)

function coop.MovieWrapper(key)
    if (not coop.mvwrap[key]) then
        coop.mvwrap[key] = function(a, b) return coop.MovieTrigger(%key, a, b) end
    end
    return coop.mvwrap[key]
end

-- every level load (the level's script has just defined its functions): wrap the movie starters --
function coop.HookMovies()
    local i, h = next(coop.moviehooks, nil)
    while (i) do
        local t = getglobal(h[1])
        if (t) then
            local j, fname = next(h[2], nil)
            while (j) do
                local fn = t[fname]
                local key = h[1] .. "." .. fname
                if (type(fn) == "function" and fn ~= coop.mvwrap[key]) then
                    coop.mvorig[key] = fn
                    t[fname] = coop.MovieWrapper(key)
                end
                j, fname = next(h[2], j)
            end
        end
        i, h = next(coop.moviehooks, i)
    end
    -- level 6's four movies are Level.C .. Level.F --
    if (coop.level == 6 and Level) then
        local c = 1
        while (c <= 4) do
            local fname = strsub("CDEF", c, c)
            local fn = Level[fname]
            local key = "Level." .. fname
            if (type(fn) == "function" and fn ~= coop.mvwrap[key]) then
                coop.mvorig[key] = fn
                Level[fname] = coop.MovieWrapper(key)
            end
            c = c + 1
        end
    end
end

function coop.MovieTrigger(key, a, b)
    local orig = coop.mvorig[key]
    local since = coop_ms() - (coop.level_ms or 0)
    if (coop.remote_run or inmovieplayer or not coop.MovieOthers() or since < coop.mv_load_ms or coop.in_done or
        coop_role() ~= "host") then
        coop.mvran[key] = coop_ms()
        return orig(a, b)
    end
    -- the host: MovieStarted tells the others once the movie has really started --
    coop.mvran[key] = coop_ms()
    coop.begin_key = key
    local r = orig(a, b)
    coop.begin_key = nil
    return r
end

-- a client: the host started the movie `key` (it will be movie `serial`) --
function coop.MovieBegin(key, serial)
    local orig = coop.mvorig[key]
    local last = coop.mvran[key]
    if (not orig) then
        coop_log(format("movie %s: not in this level here", key))
    elseif (last and coop_ms() - last < 15000) then
        coop_log(format("movie %s: already played here", key))
    elseif (coop.mv and coop.mv.state ~= "hold") then
        coop_log(format("movie %s: another movie is on", key))
    else
        coop_log(format("movie %s: the host started it, playing it too", key))
        coop.mvran[key] = coop_ms()
        coop.mvcount = serial - 1
        coop.remote_run = 1
        call(orig, {}, "x")
        coop.remote_run = nil
    end
end

-- ---------------------------------------------------------------------------------------
-- Co-op saves (bridge/saves.c). The host calls one every coop.save_interval seconds and a
-- few seconds after each checkpoint; every game in the level makes its own full save under
-- the same number. Load Game on the host's title screen offers "Resume co-op".
-- ---------------------------------------------------------------------------------------

coop.save_interval = tonumber(coop_get("save", "interval", "120")) or 120

-- the launcher's choice for this start ([start] in coop.ini, used once): the host begins a new
-- game at a level and checkpoint, or resumes a session, as soon as the title screen has been up
-- a few seconds --
do
    local mode = coop_get("start", "action", "")
    if (mode ~= "") then
        coop_set("start", "action", "")
        if (coop_role() == "host") then
            if (mode == "new") then
                coop.autostart = { tonumber(coop_get("start", "level", "1")) or 1,
                                   tonumber(coop_get("start", "cp", "1")) or 1 }
            elseif (mode == "resume") then
                coop.autostart = { 0, 0 }
                coop.autostart_sid = tonumber(coop_get("start", "sid", "0"))
            end
            coop.nextstart = 300
            coop_log(format("launcher start: %s", mode))
        end
    end
end

-- host: save in `secs` seconds (sooner than any save already due) --
function coop.ScheduleSave(secs)
    if (coop_role() ~= "host") then return end
    local due = coop_time() + secs
    if (not coop.save_due or due < coop.save_due) then coop.save_due = due end
end

-- host, every frame: save when due, the game allows it and every player is in this level --
function coop.SaveTick()
    if (not coop.level or coop.level < 1) then return end
    local cp = mdkGetCurrentCheckPoint()
    if (cp >= 0 and cp ~= coop.cp) then
        coop_log(format("checkpoint %d reached", cp))
        coop.cp = cp
        coop_progress(coop.level, cp, coop.sid)
        coop_revive_checkpoint()
        coop.ScheduleSave(3)
    end
    local now = coop_time()
    if (not coop.save_due or now < coop.save_due) then return end
    if (not CanSaveGameNow() or not coop_all_ready()) then
        coop.save_due = now + 2
        return
    end
    coop.save_due = now + coop.save_interval
    coop.SaveNow()
end

-- host: make co-op save N now, in every game --
function coop.SaveNow()
    local no = coop_save_next()
    coop_save_announce(no, coop.level, coop.cp, coop.sid or 0)
    coop.MakeSave(coop_id(), no, coop.level, coop.cp, coop.sid or 0)
end

-- make our copy of save no of host hostid's session; recorded a second later, once the game
-- has written it --
function coop.MakeSave(hostid, no, l, cp, sid)
    if (not CanSaveGameNow()) then
        coop_log(format("co-op save %d skipped here: the game cannot save right now", no))
        return
    end
    local file = coop_save_file(no, sid)
    mdkStartInstantSave(file, format("Co-op %d-%d", l, cp))
    coop.pending_save = { host = hostid, no = no, level = l, cp = cp, file = file, sid = sid, at = coop_time() + 1 }
end

function coop.PendingSaveTick()
    local ps = coop.pending_save
    if (not ps or coop_time() < ps.at) then return end
    coop.pending_save = nil
    if (mdkSaveFileExists(ps.file) == 1) then
        coop_saved(ps.host, ps.no, ps.level, ps.cp, ps.file, ps.sid)
    else
        coop_log(format("co-op save %d: %s was not written", ps.no, ps.file))
    end
end

-- host: load co-op save `no` (level l, checkpoint cp) from file; level() then asks every client
-- to load its copy --
function coop.Resume(file, no, l, cp, sid)
    coop_log(format("resuming co-op save %d (level %d checkpoint %d, session %d)", no, l, cp, sid or 0))
    coop.loading = { no = no, level = l, cp = cp, sid = sid }
    mdkStartInstantLoad(file)
end

-- host: where to resume session sid (none: the newest one): the newest co-op save, unless the
-- session got further since (a level start or checkpoint after it, e.g. where the game cannot
-- save): then that checkpoint. Returns { file, no, level, cp, sid } for a save, { level, cp,
-- sid } for a checkpoint, or nil --
function coop.ResumePoint(sid)
    if (not sid) then sid = coop_session_info(1) end
    if (not sid) then return nil end
    local file, no, l, cp = coop_save_find(coop_id(), 0, 0, sid)
    if (file and mdkSaveFileExists(file) ~= 1) then file = nil end
    local pl, pcp, pafter = coop_progress_get(sid)
    if (pl and (not file or no <= pafter)) then
        return { level = pl, cp = pcp, sid = sid }
    end
    if (file) then return { file = file, no = no, level = l, cp = cp, sid = sid } end
    return nil
end

function coop.ResumeAt(rp)
    if (rp.file) then
        coop.Resume(rp.file, rp.no, rp.level, rp.cp, rp.sid)
    else
        coop_log(format("resuming co-op at level %d checkpoint %d (no save since)", rp.level, rp.cp))
        coop.sid = rp.sid
        coop.resume_sid = rp.sid
        mdkNewGame(rp.level, rp.cp)
    end
end

function coop.ResumeLatest(sid)
    local rp = coop.ResumePoint(sid)
    if (rp) then
        coop.ResumeAt(rp)
    else
        coop_log("no co-op save to resume")
    end
end

-- Load Game (title screen and pause menu): the host's newest co-op save comes first --
coop.addpcload_orig = AddPCLoadMenuItem
function AddPCLoadMenuItem(filename, descoverride)
    if (coop.addresume) then
        coop.addresume = nil
        coop.AddResumeItem()
    end
    return coop.addpcload_orig(filename, descoverride)
end

coop.buildpcload_orig = BuildPCLoadMenu
function BuildPCLoadMenu()
    coop.addresume = (coop_role() == "host")
    coop.buildpcload_orig()
end

-- "Alice, Bob, Carol, Dan" -> "Alice, Bob, Carol +1" (names is what coop_session_info gives) --
function coop.ShortNames(names)
    local out, n, rest = "", 0, names
    while (rest ~= "") do
        local comma = strfind(rest, ", ", 1, 1)
        local one = rest
        if (comma) then one = strsub(rest, 1, comma - 1) rest = strsub(rest, comma + 2) else rest = "" end
        n = n + 1
        if (n <= 3) then
            if (n > 1) then out = out .. ", " end
            out = out .. one
        end
    end
    if (n > 3) then out = out .. format(" +%d", n - 3) end
    return out
end

-- "checkpoint 7 of 12": the checkpoints of a level numbered in play order (3-16 sits between 3-6
-- and 3-7), not by their raw numbers --
function coop.CpText(l, cp)
    local i = 1
    local first, count, at = 0, 0, 0
    while (i <= coop.ncheckpoints) do
        if (coop.checkpoints[i][1] == l) then
            if (first == 0) then first = i end
            count = count + 1
            if (coop.checkpoints[i][2] == cp) then at = count end
        end
        i = i + 1
    end
    if (at == 0) then return format("checkpoint %d", cp) end
    return format("checkpoint %d of %d", at, count)
end

function coop.AddResumeItem()
    local i = 1
    local sid, l, cp, updated, peak, names = coop_session_info(i)
    while (sid and i <= 3) do
        local rp = coop.ResumePoint(sid)
        if (rp) then
            local who = coop.ShortNames(names)
            local label = format("Resume co-op: level %d, %s", rp.level, coop.CpText(rp.level, rp.cp))
            if (who ~= "") then label = label .. " with " .. who end
            local item = mdkCreateMenuItem(menu.pcload, label)
            item.resume = rp
            item.Select = function(self)
                mdkHideInstantSaveScreenShot()
                menu.pause.Cancel()
                coop.ResumeAt(self.resume)
            end
            item.Highlight = function(self)
                if (self.resume.file) then
                    mdkShowInstantSaveScreenShot(self.resume.file)
                else
                    mdkHideInstantSaveScreenShot()
                end
            end
        end
        i = i + 1
        sid, l, cp, updated, peak, names = coop_session_info(i)
    end
end

-- ---------------------------------------------------------------------------------------
-- Client: lost the host. Nothing in our world moves without the host, so the game waits
-- behind a message (input goes to the menu) until the host is back in a level; a host that
-- restarted asks us to load our copy of the save it resumes.
-- ---------------------------------------------------------------------------------------

menu.coopwait = mdkCreateMenu(0.5, 0.42, 0.05, 0.05, 0, JUSTIFY_CENTER, 1)
menu.coopwait.Cancel = function()
    if (coop_transport() == "steam" and coop_steam_lobby() == "") then coop.ShowPage("coopsession") end
end

-- Client title screen: no New Game selection is required. The host's reliable
-- LEVEL event is the only authority that starts loading our world.
menu.coopjoin = mdkCreateMenu(0.5, 0.38, 0.05, 0.05, 0, JUSTIFY_CENTER, 1)
function coop.CancelJoin()
    coop.join_cancelled = 1
    coop_join_tick(coop.level or 0, 0, 1)
    coop.ShowPage("coopsession")
end
menu.coopjoin.Cancel = coop.CancelJoin

function coop.JoinScreenTick()
    coop_join_screen(0)
    if (coop_role() == "host") then return end
    if (coop_transport() == "steam") then
        local lobby = coop_steam_lobby()
        -- Before joining, keep Session available for entering an ID or accepting
        -- an invite. A new accepted invite also releases an earlier cancellation.
        if (lobby == "" and not coop.join_seen_lobby) then return end
        if (lobby ~= "") then
            coop.join_seen_lobby = 1
            if (coop.join_cancelled and coop_join_cancelled() == 0) then
                coop.join_cancelled = nil
                coop.join_state = nil
                coop_join_retry()
            end
        end
    end
    if (coop.join_cancelled) then return end
    local state, text = coop_join_tick(coop.level or 0, coop.following and 1 or 0, 0)
    if (state == 3) then
        coop.join_state = state
        if (menu.current == "coopjoin") then HideMenu() end
        return
    end
    if (coop.level and coop.level >= 1) then return end
    if (state ~= coop.join_state) then
        coop.join_state = state
        mdkClearMenu(menu.coopjoin)
        coop.Heading(menu.coopjoin, text)
        if (state ~= 5 and state ~= 6) then
            coop.Heading(menu.coopjoin, "You will join automatically when the host is ready")
        end
        if (state == 5 or state == 6) then
            local retry = mdkCreateMenuItem(menu.coopjoin, "Retry connection")
            retry.Select = function() coop_join_retry() coop.join_state = nil end
        end
        local cancel = mdkCreateMenuItem(menu.coopjoin, "Cancel join (Esc)")
        cancel.Select = coop.CancelJoin
    end
    if (menu.current ~= "coopjoin") then ShowMenu("coopjoin") end
    coop_join_screen(menu.current == "coopjoin" and 1 or 0)
end

function coop.WaitTick()
    coop.JoinScreenTick()
    if (coop_role() == "host" or not coop.level or coop.level < 1) then
        if (coop.waiting) then coop.StopWaiting() end
        return
    end
    local silent, hlevel = coop_host_status()
    local why = nil
    if (coop_transport() == "steam" and coop_steam_lobby() == "") then
        why = "Steam lobby disconnected - accept an invite or press Esc"
    elseif (silent > 2000) then
        why = "Connection to the host lost - reconnecting"
    elseif (hlevel < 1) then
        why = "The host left the level - waiting for the host"
    end
    if (why and why ~= coop.waiting) then
        coop_log(format("waiting: %s", why))
        mdkClearMenu(menu.coopwait)
        mdkCreateMenuItem(menu.coopwait, why)
        if (coop_transport() == "steam" and coop_steam_lobby() == "") then
            mdkCreateMenuItem(menu.coopwait, "Press Esc for Session to join by lobby ID")
        else
            mdkCreateMenuItem(menu.coopwait, "Your game is kept and resumes by itself")
        end
        ShowMenu("coopwait")
        coop.waiting = why
    elseif (not why and coop.waiting) then
        coop.StopWaiting()
    end
end

function coop.StopWaiting()
    coop_log("host is back: playing again")
    coop.waiting = nil
    if (menu.current == "coopwait") then HideMenu() end
end

-- Checkpoints in play order, from the debug menu's own list (debug.lua): { {level, cp}, ... } --
coop.checkpoints = {}
coop.ncheckpoints = 0
local ln, lv = next(menu.debugload.checkpoints, nil)
while (ln) do
    local rn, rv = next(lv, nil)
    while (rn) do
        coop.ncheckpoints = coop.ncheckpoints + 1
        coop.checkpoints[coop.ncheckpoints] = { rv[2], rv[3] }
        rn, rv = next(lv, rn)
    end
    ln, lv = next(menu.debugload.checkpoints, ln)
end

-- index of the checkpoint we are at (or the last one before it), 0 if none --
function coop.CheckpointIndex()
    local l, cp = coop.level or 0, coop.cp or 0
    local i, found = 1, 0
    -- the list is in play order, not number order (3-16, "3-6.5", sits between 3-6 and 3-7):
    -- an exact match first --
    while (i <= coop.ncheckpoints) do
        local c = coop.checkpoints[i]
        if (c[1] == l and c[2] == cp) then return i end
        i = i + 1
    end
    i = 1
    while (i <= coop.ncheckpoints) do
        local c = coop.checkpoints[i]
        if (c[1] < l or (c[1] == l and c[2] <= cp)) then found = i end
        i = i + 1
    end
    return found
end

-- host only: the client follows the host's loads; loading on its own would split the games --
function coop.GoToCheckpoint(i)
    if (coop_role() ~= "host") then
        coop_log("checkpoint keys: only the host changes level")
        return
    end
    if (i < 1) then i = coop.ncheckpoints end
    if (i > coop.ncheckpoints) then i = 1 end
    local c = coop.checkpoints[i]
    coop_log(format("going to level %d checkpoint %d", c[1], c[2]))
    mdkNewGame(c[1], c[2])
end

-- F6: next checkpoint (1-1 .. 1-8, 2-1, ...) --
comfuncs.fkey[COM_TOGL1] = function()
    coop.GoToCheckpoint(coop.CheckpointIndex() + 1)
end

-- F7: previous checkpoint --
comfuncs.fkey[COM_TOGL2] = function()
    local i = coop.CheckpointIndex()
    -- between checkpoints (or at an intro section) "previous" is the one we are past --
    if (i >= 1 and coop.checkpoints[i][1] == coop.level and coop.checkpoints[i][2] == coop.cp) then
        i = i - 1
    end
    coop.GoToCheckpoint(i)
end

-- F8: first checkpoint of the next level --
comfuncs.fkey[COM_TOGL3] = function()
    local i = coop.CheckpointIndex() + 1
    while (i <= coop.ncheckpoints and coop.checkpoints[i][1] <= (coop.level or 0)) do
        i = i + 1
    end
    coop.GoToCheckpoint(i)
end

-- F2 (was debug.lua's status display): write a numbered marker to both games' logs, with
-- our position, and save a screenshot, so a tester can say "it went wrong around mark 3" --
comfuncs.fkey[COM_SHOWSTATUS] = function()
    local p = mdkGetPlayerGob()
    coop.marks = coop.marks + 1
    if (p) then
        coop_log(format("MARK %d at (%.1f, %.1f, %.1f) room %s", coop.marks, p.position.x, p.position.y,
                        p.position.z, tostring(mdkGetCurrentRoom())))
    else
        coop_log(format("MARK %d", coop.marks))
    end
    coop_mark(coop.marks)
    chScreenShot(format("coop_mark%d.tga", coop.marks))     -- saved in the game folder
end

-- F11: log anim states, every gob and the OBJ_* numbers (bridge.log) --
comfuncs.fkey[COM_TOGL4] = function()
    coop_animdump(mdkGetPlayerGob())
    local s, g = next(coop.puppets, nil)
    while (s) do
        coop_animdump(g)
        s, g = next(coop.puppets, s)
    end
    coop_listgobs()
    if (not coop.typeslogged) then coop.LogObjTypes() coop.typeslogged = 1 end
end

-- Emote browser (dev keys): PgDn / PgUp (keypad 6 / 4) play the next / previous animation the character has
-- on loop, keypad 0 stops; the id and ANIM_* name go to bridge.log --
function coop.AnimNames()
    if (coop.animnames) then return coop.animnames end
    coop.animnames = {}
    local n, v = nextvar(nil)
    while (n) do
        if (strsub(n, 1, 5) == "ANIM_" and type(v) == "number") then coop.animnames[v] = n end
        n, v = nextvar(n)
    end
    return coop.animnames
end

coop.emotekept = {}

-- The emote wheel (hold the Emote key, Options > Co-op > Controls), clockwise from the top: the picks from the
-- browser that this character has, filled up from its other ACTIONnn animations (the three heroes have different
-- sets: Max has no THROW, Doc no ACTION05), then "All..." (EMOTE_ALL in bridge/emote.c), the list of every
-- animation it has. Rebuilt when the character changes. coop_emote_wheel(id, label, ...) takes up to 8 --
coop.emote_base = { 2, 3, 4, 16, 57, 81, 82 }
coop.emote_slots = 7
coop.EMOTE_ALL = 1000

-- "ANIM_ACTION04" -> "Action 4", "ANIM_KNOCKDOWN" -> "Knockdown", "ANIM_RUNFL" -> "Run forward left" --
coop.animmoves = { RUN = "Run", UP = "Jump", DN = "Fall", CHUTE = "Parachute" }
coop.animdirs = { F = "forward", B = "back", L = "left", R = "right", FL = "forward left", FR = "forward right",
                  BL = "back left", BR = "back right" }
coop.animwords = { GETUP = "Get up", GRABLEDGE = "Grab ledge" }

function coop.AnimLabel(id)
    local name = coop.AnimNames()[id]
    if (not name) then return format("Animation %d", id) end
    name = strsub(name, 6)
    if (coop.animwords[name]) then return coop.animwords[name] end
    local base, move = next(coop.animmoves, nil)
    while (base) do
        local rest = strsub(name, strlen(base) + 1)
        if (strsub(name, 1, strlen(base)) == base and (rest == "" or coop.animdirs[rest])) then
            if (rest == "") then return move end
            return move .. " " .. coop.animdirs[rest]
        end
        base, move = next(coop.animmoves, base)
    end
    local i = 1
    while (i <= strlen(name) and (strsub(name, i, i) < "0" or strsub(name, i, i) > "9")) do i = i + 1 end
    local word = strupper(strsub(name, 1, 1)) .. strlower(strsub(name, 2, i - 1))
    if (i <= strlen(name)) then return format("%s %d", word, tonumber(strsub(name, i))) end
    return word
end

function coop.EmoteWheel()
    local ids, n = {}, 0
    local used = {}
    local k, id = next(coop.emote_base, nil)
    while (k and n < coop.emote_slots) do
        if (coop_anim_has(id)) then n = n + 1 ids[n] = id used[id] = 1 end
        k, id = next(coop.emote_base, k)
    end
    local names = coop.AnimNames()
    id = 0
    while (id < 160 and n < coop.emote_slots) do
        if (not used[id] and names[id] and strsub(names[id], 1, 11) == "ANIM_ACTION" and coop_anim_has(id)) then
            n = n + 1 ids[n] = id
        end
        id = id + 1
    end
    local args, a = {}, 0
    local i = 1
    while (i <= n) do
        args[a + 1] = ids[i] args[a + 2] = coop.AnimLabel(ids[i]) a = a + 2
        i = i + 1
    end
    args[a + 1] = coop.EMOTE_ALL args[a + 2] = "All..." a = a + 2
    args.n = a
    call(coop_emote_wheel, args)
end

-- every frame: the wheel follows the character; its "All..." slot opens the list --
function coop.EmoteTick(p)
    if (p and mdkGetGobType(p) ~= coop.wheeltype) then
        coop.wheeltype = mdkGetGobType(p)
        coop.EmoteWheel()
        coop_log(format("emote wheel for type %d", coop.wheeltype))
    end
    if (coop_emote_all()) then coop.ShowAllEmotes(1, 1) end
end

-- the list of every animation the character has, a page at a time; choosing one plays it --
coop.allemotes_page = 14
menu.coopemotes = mdkCreateMenu(0.5, 0.16, 0.04, 0.04, 0.002, JUSTIFY_CENTER, 1)
menu.coopemotes.Cancel = function() HideMenu() end

-- open: from the wheel, always shown (after a pick has closed it, menu.current still names it) --
function coop.ShowAllEmotes(page, open)
    local m = menu.coopemotes
    local ids, n = {}, 0
    local id = 0
    while (id < 160) do
        if (coop_anim_has(id)) then n = n + 1 ids[n] = id end
        id = id + 1
    end
    local pages = floor((n + coop.allemotes_page - 1) / coop.allemotes_page)
    if (page < 1) then page = pages end
    if (page > pages) then page = 1 end
    mdkClearMenu(m)
    coop.Heading(m, format("All emotes (%d), page %d of %d", n, page, pages))
    local i = (page - 1) * coop.allemotes_page + 1
    coop.allemote_items = {}
    local k = 0
    while (i <= n and i <= page * coop.allemotes_page) do
        local item = mdkCreateMenuItem(m, coop.AnimLabel(ids[i]))
        item.anim = ids[i]
        item.Select = function(self) HideMenu() coop_emote(self.anim, 1) end
        k = k + 1
        coop.allemote_items[k] = item
        i = i + 1
    end
    local item = mdkCreateMenuItem(m, "Next page")
    item.page = page + 1
    item.Select = function(self) coop.ShowAllEmotes(self.page) end
    item = mdkCreateMenuItem(m, "Previous page")
    item.page = page - 1
    item.Select = function(self) coop.ShowAllEmotes(self.page) end
    item = mdkCreateMenuItemW(m, strings.opt_ok)
    mdkMenuItemStayInCenterPlease(item)
    item.Select = function(self) HideMenu() end
    coop_log(format("emotes: all, page %d of %d (%d)", page, pages, n))
    if (open or menu.current ~= "coopemotes") then ShowMenu("coopemotes") end
end

-- ---- Changing character at any time (the character wheel, Options > Co-op > Controls) ----
-- Each level script creates one hero (CreateKurt / CreateMax / CreateDoctor); the switch does what the
-- debug menu's "Switch Player Character" does: new hero, same spot, the old one deleted, and the share of hp left carried over. With
-- [game] char_follow on (the default) the next level load puts you back on the hero the game
-- picks; off, your choice is applied again after every load. Doc's levels with Hyde (6, 12) keep
-- the game's pair. --
coop.gamechar = nil         -- the hero the game created for this level
coop.pickchar = nil         -- the hero the player chose (OBJ type), or nil
coop.charapplied = 0        -- this level's hero has been checked against the choice
coop.hyde = nil
coop.chars = {}
coop.charcreate = { [OBJ_KURT] = CreateKurt, [OBJ_MAX] = CreateMax, [OBJ_DOC] = CreateDoctor }
coop.hydecreate = CreateHyde

function CreateKurt() coop.gamechar = OBJ_KURT coop.charcreate[OBJ_KURT]() end
function CreateMax() coop.gamechar = OBJ_MAX coop.charcreate[OBJ_MAX]() end
function CreateDoctor() coop.gamechar = OBJ_DOC coop.charcreate[OBJ_DOC]() end
function CreateHyde() coop.hyde = 1 coop.hydecreate() end

coop_char_wheel(OBJ_KURT, "Kurt", OBJ_MAX, "Max", OBJ_DOC, "Doc", 0, "Level's own")

function coop.CharFollow()
    return coop_get("game", "char_follow", "1") == "1"
end

-- called as a level starts, before its script runs
function coop.NewLevelChar()
    coop.unstick = nil
    coop.gamechar = nil
    coop.hyde = nil
    coop.charapplied = 0
    if (coop.CharFollow()) then coop.pickchar = nil end
end

-- Kurt and Max are wider than Doc: Doc against a wall, the new hero put on the same spot can be stuck in it and not
-- move at all (measured at 1-1: 0.2 back from the wall is free). The engine offers no test of a spot (line of sight
-- ignores walls; a stuck hero still falls), so the player's own moving is the test: after a switch, while a move
-- key is held and the hero has not moved, it is put on the next spot around (8 directions, 0.3 then 0.6 away) until
-- it moves. The time limit starts with the first held move key, so a player who waits before walking is not left
-- stuck --
coop.unstick_ms = 4000
coop.unstick_frames = 6
coop.unstick_spots = {}
function coop.MakeUnstickSpots()        -- Lua 3.1's cos / sin take degrees
    local r, k = 0.3, 0
    while (r < 0.7) do
        local d = 0
        while (d < 8) do
            k = k + 1
            coop.unstick_spots[k] = { r * cos(d * 45), r * sin(d * 45) }
            d = d + 1
        end
        r = r + 0.3
    end
end
coop.MakeUnstickSpots()

function coop.UnstickTick(p)
    local u = coop.unstick
    if (not u or not p) then return end
    if (u.till and coop_ms() > u.till) then coop.unstick = nil return end
    local x, y = p.position.x, p.position.y
    if ((x - u.cx) * (x - u.cx) + (y - u.cy) * (y - u.cy) > 0.0025) then
        if (u.i > 0) then coop_log(format("character: unstuck at spot %d", u.i)) end
        coop.unstick = nil
        return
    end
    if (omGetCommandValue(COM_FORWARD) <= 0 and omGetCommandValue(COM_BACKWARD) <= 0 and
        omGetCommandValue(COM_STRAFELEFT) <= 0 and omGetCommandValue(COM_STRAFERIGHT) <= 0) then
        return
    end
    if (not u.till) then u.till = coop_ms() + coop.unstick_ms end
    u.frames = u.frames + 1
    if (u.frames < coop.unstick_frames) then return end
    u.frames = 0
    u.i = u.i + 1
    local spot = coop.unstick_spots[u.i]
    if (not spot) then coop.unstick = nil return end
    u.cx, u.cy = u.x + spot[1], u.y + spot[2]
    mdkGobSetPositionXYZ(p, u.cx, u.cy, u.z)
end

function coop.SwitchChar(p, t)
    if (coop.hyde or not p or mdkGetGobType(p) == t or not coop.charcreate[t]) then return end
    local x, y, z = p.position.x, p.position.y, p.position.z
    coop_log(format("character: %d -> %d", mdkGetGobType(p), t))
    local camera = mdkGetCameraGob()
    if (camera) then omGobDelete(camera) end
    coop.charcreate[t]()
    mdkGobSetPositionXYZ(bob, x, y, z)
    coop_carry_hp(p, bob)
    omGobDelete(p)
    coop.unstick = { x = x, y = y, z = z, cx = x, cy = y, i = 0, frames = 0 }
end

function coop.CharTick(p)
    local t = coop_char_pick()
    if (t) then
        if (t == 0) then
            coop.pickchar = nil
            t = coop.gamechar
        else
            coop.pickchar = t
        end
        if (t) then coop.SwitchChar(p, t) end
        return
    end
    if (p and coop.charapplied == 0 and coop.level and coop.level >= 1) then
        coop.charapplied = 1
        if (coop.pickchar and not coop.CharFollow()) then coop.SwitchChar(p, coop.pickchar) end
    end
end

function coop.EmoteCaption()
    local id = coop.emoteid
    local mark = ""
    if (id and coop.emotekept[id]) then mark = "   [KEPT]" end
    coop_emote_caption(format("emote %s: %s%s", tostring(id), tostring(coop.AnimNames()[id]), mark),
        "PgDn / PgUp: next / prev   Home / End: +10 / -10   Insert: keep   Delete: list kept   Backspace: stop")
end

function coop.EmoteStep(dir)
    local names = coop.AnimNames()
    local id = coop.emoteid or -1
    local tries = 0
    while (tries < 160) do
        id = id + dir
        if (id > 159) then id = id - 160 end
        if (id < 0) then id = id + 160 end
        if (coop_emote(id, 1) == 1) then
            coop.emoteid = id
            coop_log(format("emote %d %s", id, tostring(names[id])))
            coop.EmoteCaption()
            return
        end
        tries = tries + 1
        if (dir > 9 or dir < -9) then dir = dir / abs(dir) end      -- a jump lands on a missing id: walk on from there
    end
end

function coop.EmoteKeep()
    local id = coop.emoteid
    if (not id or coop_emoting() ~= id) then return end
    if (coop.emotekept[id]) then coop.emotekept[id] = nil else coop.emotekept[id] = 1 end
    coop_log(format("EMOTE %s %d %s", coop.emotekept[id] and "KEEP" or "UNKEEP", id, tostring(coop.AnimNames()[id])))
    coop.EmoteCaption()
end

function coop.EmoteList()
    local out = ""
    local i = 0
    while (i < 160) do
        if (coop.emotekept[i]) then out = out .. format(" %d=%s", i, tostring(coop.AnimNames()[i])) end
        i = i + 1
    end
    coop_log("EMOTE KEPT:" .. out)
end

comfuncs.fkey[COM_LIGHTDEFAULT6] = function() coop.EmoteStep(1) end
comfuncs.fkey[COM_LIGHTDEFAULT4] = function() coop.EmoteStep(-1) end
comfuncs.fkey[COM_LIGHTDEFAULT9] = function() coop.EmoteStep(10) end
comfuncs.fkey[COM_LIGHTDEFAULT7] = function() coop.EmoteStep(-10) end
comfuncs.fkey[COM_LIGHTDEFAULT5] = function() coop.EmoteKeep() end
comfuncs.fkey[COM_LIGHTDEFAULT8] = function() coop.EmoteList() end
comfuncs.fkey[COM_LIGHTRESET] = function() coop_emote(-1) end

-- F4: teleport the player onto the next test spot: level 1's decoy item (next room, standing
-- on it picks it up), then door dr1_03 (another room) --
coop.tpspots = { "l1_r3dummy", "dr1_03", "void" }
coop.ntpspots = 3      -- Lua 3.1 has no getn
coop.tpnext = 1
comfuncs.fkey[COM_TOGLG] = function()
    local name = coop.tpspots[coop.tpnext]
    local p = mdkGetPlayerGob()
    if (name == "void") then
        -- 400 units below us: outside the level, to test falling out of the world --
        coop.tpnext = 1
        mdkGobSetPositionXYZ(p, p.position.x, p.position.y, p.position.z - 400)
        coop_log(format("teleported into the void at z %.1f", p.position.z))
        return
    end
    local g = getglobal(name)
    coop.tpnext = coop.tpnext + 1
    if (coop.tpnext > coop.ntpspots) then coop.tpnext = 1 end
    if (g) then
        mdkGobSetPositionXYZ(mdkGetPlayerGob(), g.position.x, g.position.y, g.position.z)
        coop_log(format("teleported to %s", name))
    end
end

-- F5 (was debug.lua's next-section): host spawns a Doganboy 6 units ahead, for combat tests;
-- the client gets it through the bridge --
comfuncs.fkey[COM_NEXTSECTION] = function()
    if (coop_role() ~= "host") then
        coop_log("F5: only the host spawns enemies")
        return
    end
    coop.enemies = coop.enemies + 1
    make(format("coop_dog%d", coop.enemies), OBJ_DOGANBOY)
end

-- log all OBJ_* constants once (type numbers in coop_listgobs output) --
function coop.LogObjTypes()
    local n, v = nextvar(nil)
    while (n) do
        if (strsub(n, 1, 4) == "OBJ_" and type(v) == "number") then
            coop_log(format("objtype %d %s", v, n))
        end
        n, v = nextvar(n)
    end
end

-- F3 (was debug.lua's clear console): 200 lava damage (type 0x40) to our player, to test
-- going down: lava starts the death sequence whatever the hp --
comfuncs.fkey[COM_CLEARCONSOLE] = function()
    mdkDealDamage(nil, mdkGetPlayerGob(), 200, 64, -1)
end

-- Num 1 (was debug.lua's bars on F1; F1 now holds open the Quick Guide): host spawns a Poopsy ahead, always named "coop_spawn" like a
-- spawner's spawns, to test duplicate names; the client hits coop_dog1 (else the nearest
-- synced enemy) ten times for 2 with its gun's damage type, through the normal damage
-- function (as if shot, without aiming) --
comfuncs.fkey[COM_SHOWBARS] = function()
    if (coop_role() == "host") then
        make("coop_spawn", OBJ_POOPSY)
        return
    end
    local dog = coop.FindGob("coop_dog1") or coop_nearest_enemy()
    local i = 0
    while (dog and i < 10) do
        mdkDealDamage(mdkGetPlayerGob(), dog, 2, 1, -1)
        i = i + 1
    end
end

-- F12 is Steam's screenshot key; F10: raise a Lua error on purpose, to check error logging --
comfuncs.fkey[COM_PRINTCOL] = function()
    coop_log("raising a test error")
    coop.no_such_function()
end


-- ---------------------------------------------------------------------------------------
-- Options > Co-op: players, session and gameplay settings (coop.ini through the bridge;
-- the game's menus, so it is there on the title screen and in the pause menu's Options)
-- ---------------------------------------------------------------------------------------

-- the game's BuildOptionsMenu (options.lua) with Co-op before OK --
function BuildOptionsMenu()
    mdkClearMenu(menu.options)

    item = mdkCreateMenuItemW(menu.options, strings.opt_video)
    item.Select = function(self)
        BuildVideoMenu()
        menu.SwitchDlg("optvideo")
        mdkShowMouse(1,0)
    end

    item = mdkCreateMenuItemW(menu.options, strings.opt_audio)
    item.Select = function(self)
        BuildAudioMenu()
        menu.SwitchDlg("optaudio")
        mdkShowMouse(1,0)
    end

    item = mdkCreateMenuItemW(menu.options, strings.opt_controls)
    item.Select = function(self)
        BuildControlsMenu()
        menu.SwitchDlg("optcontrols")
        mdkShowMouse(1,0)
    end

    item = mdkCreateMenuItemW(menu.options, strings.opt_game)
    item.Select = function(self)
        BuildGameMenu()
        menu.SwitchDlg("optgame")
        mdkShowMouse(1,0)
    end

    item = mdkCreateMenuItem(menu.options, "Co-op")
    item.Select = function(self)
        coop.ShowPage("coop")
    end

    item = mdkCreateMenuItemW(menu.options, strings.opt_ok)
    mdkMenuItemStayInCenterPlease(item)
    mdkMenuItemSetSponge(item, 0.03)
    item.Select = function()
        menu.options.Cancel()
    end
end

menu.coop = mdkCreateMenu(0.5, 0.25, 0.06, 0.06, 0.01, JUSTIFY_CENTER, 1)
menu.coopplayers = mdkCreateMenu(0.5, 0.16, 0.045, 0.045, 0.005, JUSTIFY_CENTER, 1)
menu.coopsession = mdkCreateMenu(0.5, 0.22, 0.05, 0.05, 0.01, JUSTIFY_CENTER, 1)
menu.coopgame = mdkCreateMenu(0.7, 0.25, 0.05, 0.05, 0.01, JUSTIFY_RIGHT, 1)
menu.coopdiff = mdkCreateMenu(0.7, 0.2, 0.045, 0.045, 0.005, JUSTIFY_RIGHT, 1)
menu.coopvoice = mdkCreateMenu(0.7, 0.25, 0.05, 0.05, 0.01, JUSTIFY_RIGHT, 1)
menu.coopui = mdkCreateMenu(0.7, 0.2, 0.045, 0.045, 0.005, JUSTIFY_RIGHT, 1)

coop.pages = {
    coop = function() coop.BuildMain() end,
    coopplayers = function() coop.BuildPlayers() end,
    coopsession = function() coop.BuildSession() end,
    coopgame = function() coop.BuildGame() end,
    coopctrl = function() coop.OpenControls() end,
    coopdiff = function() coop.BuildDifficulty() end,
    coopvoice = function() coop.BuildVoice() end,
    coopui = function() coop.BuildInterface() end,
}

function coop.ShowPage(name)
    coop_log(format("menu: %s", name))
    coop.pages[name]()
    menu.SwitchDlg(name)
    mdkShowMouse(1,0)
end

-- a heading line: plain text, not selectable --
function coop.Heading(m, text)
    local item = mdkCreateMenuItem(m, text)
    mdkMenuItemEnable(item, 0)
    return item
end

function coop.BackItem(m, page)
    local item = mdkCreateMenuItemW(m, strings.opt_ok)
    mdkMenuItemStayInCenterPlease(item)
    mdkMenuItemSetSponge(item, 0.03)
    item.back = page
    item.Select = function(self) coop.ShowPage(self.back) end
end

-- main page --
function coop.BuildMain()
    mdkClearMenu(menu.coop)
    coop.Heading(menu.coop, "Co-op")
    local item = mdkCreateMenuItem(menu.coop, "Players")
    item.Select = function(self) coop.ShowPage("coopplayers") end
    item = mdkCreateMenuItem(menu.coop, "Session")
    item.Select = function(self) coop.ShowPage("coopsession") end
    item = mdkCreateMenuItem(menu.coop, "Gameplay")
    item.Select = function(self) coop.ShowPage("coopgame") end
    item = mdkCreateMenuItem(menu.coop, "Interface")
    item.Select = function(self) coop.ShowPage("coopui") end
    item = mdkCreateMenuItem(menu.coop, "Controls")
    item.Select = function(self) coop.ShowPage("coopctrl") end
    item = mdkCreateMenuItem(menu.coop, "Voice chat")
    item.Select = function(self) coop.ShowPage("coopvoice") end
    item = mdkCreateMenuItemW(menu.coop, strings.opt_ok)
    mdkMenuItemStayInCenterPlease(item)
    mdkMenuItemSetSponge(item, 0.03)
    item.Select = function(self) menu.coop.Cancel() end
end

menu.coop.Cancel = function()
    menu.SwitchDlg("options", 0)
    mdkShowMouse(1,0)
end

-- players page: everyone connected, refreshed every second --
function coop.BuildPlayers()
    mdkClearMenu(menu.coopplayers)
    coop.Heading(menu.coopplayers, "Players")
    local me = coop_slot()
    local _, slots = coop_players()
    local s, n = 0, 0
    while (s < slots) do
        local name, ping = coop_player_info(s)
        if (name) then
            local tags = ""
            if (s == 0) then tags = " (host)" end
            if (s == me) then tags = tags .. " - you" elseif (ping > 0) then tags = tags .. format(" - %d ms", ping) end
            -- a plain item (disabled ones are drawn too faint to read); selecting it does nothing --
            mdkCreateMenuItem(menu.coopplayers, format("%d  %s%s", s + 1, name, tags))
            n = n + 1
        end
        s = s + 1
    end
    if (not me) then coop.Heading(menu.coopplayers, "Not connected to a host yet") end
    coop.players_built = coop_time()
    coop.BackItem(menu.coopplayers, "coop")
end

menu.coopplayers.Update = function()
    if (coop_time() - (coop.players_built or 0) >= 1) then coop.BuildPlayers() end
end

menu.coopplayers.Cancel = function() coop.ShowPage("coop") end

-- text entry (the save menu's way): a box at the bottom, the menu waits for Enter or Esc --
function coop.EditText(m, text, onaccept)
    m.editing = 1
    m.editbox = mdkMessageBoxCreate(0.2, 0.7, 0.6, 0.05, -1, 1)
    m.onaccept = onaccept
    mdkEditString(m.editbox, text)
    conscheme = "poo"
    mdkShowMouse(0,0)
end

function coop.EditUpdate(m, page)
    if (not m.editing) then return end
    local result = mdkGetEditResult()
    if (result == 0) then return end
    m.editing = nil
    mdkMessageBoxDestroy(m.editbox)
    mdkShowMouse(1,0)
    newconscheme = "menu"
    if (result == 1) then m.onaccept(mdkGetEditString()) end
    coop.pages[page]()
end

-- session page: name, transport, Steam lobby or UDP address, resume / save now --
function coop.BuildSession()
    local m = menu.coopsession
    mdkClearMenu(m)
    coop.Heading(m, "Session")
    if (coop.join_cancelled) then
        local retry = mdkCreateMenuItem(m, "Join host again")
        retry.Select = function()
            coop.join_cancelled = nil
            coop.join_state = nil
            coop_join_retry()
        end
    end
    local item = mdkCreateMenuItem(m, "Name: " .. coop_name())
    item.Select = function(self)
        coop.EditText(menu.coopsession, coop_name(), function(text) coop_set_name(text) end)
    end

    -- host or join: a plain item that switches (a combo box draws over its label here) --
    local role = coop_get("net", "role", "host")
    if (role == "client") then item = mdkCreateMenuItem(m, "Role: Join") else item = mdkCreateMenuItem(m, "Role: Host") end
    item.Select = function(self)
        if (coop_get("net", "role", "host") == "client") then coop_set("net", "role", "host")
        else coop_set("net", "role", "client") end
        coop.role_changed = 1
        coop.BuildSession()
        mdkMenuSetHighlighted(menu.coopsession, 2)
    end

    local transport = coop_get("net", "transport", "udp")
    item = mdkCreateMenuItem(m, "Network: " .. transport .. " (next start)")
    item.Select = function(self)
        if (coop_get("net", "transport", "udp") == "steam") then coop_set("net", "transport", "udp")
        else coop_set("net", "transport", "steam") end
        coop.BuildSession()
    end
    if (coop_transport() == "steam") then
        coop.Heading(m, coop_steam_status())
        local lobby = coop_steam_lobby()
        if (lobby ~= "") then coop.Heading(m, "Lobby ID: " .. lobby) end
        if (lobby ~= "" and coop_role() == "host") then
            item = mdkCreateMenuItem(m, "Invite Steam friends")
            item.Select = function(self) coop_steam_invite(); coop.BuildSession() end
        end
        item = mdkCreateMenuItem(m, "Join lobby by ID")
        item.Select = function(self)
            coop.EditText(menu.coopsession, "", function(text) coop_steam_join(text) end)
        end
        if (lobby == "") then
            item = mdkCreateMenuItem(m, "Host a Steam lobby")
            item.Select = function(self) coop_steam_host(); coop.BuildSession() end
        else
            item = mdkCreateMenuItem(m, "Leave Steam lobby")
            item.Select = function(self) coop_steam_leave(); coop.BuildSession() end
        end
    else
        local host, port = coop_get("net", "peer_host", "127.0.0.1"), coop_get("net", "peer_port", "27900")
        item = mdkCreateMenuItem(m, "Host address: " .. host .. ":" .. port)
        item.Select = function(self)
            local h, p = coop_get("net", "peer_host", "127.0.0.1"), coop_get("net", "peer_port", "27900")
            coop.EditText(menu.coopsession, h .. ":" .. p, function(text) coop.SetHostAddress(text) end)
        end
    end
    coop.session_status = coop_steam_status()
    coop.session_lobby = coop_steam_lobby()
    if (coop.role_changed or coop.host_note) then
        coop.Heading(m, coop.host_note or "Host or join applies when you next start the game")
    end

    if (coop_role() == "host") then
        item = mdkCreateMenuItem(m, "Start at level...")
        item.Select = function(self) coop.ShowPage("coopstart") end
        local rp = coop.ResumePoint()
        if (rp) then
            item = mdkCreateMenuItem(m, format("Resume co-op: level %d, %s", rp.level, coop.CpText(rp.level, rp.cp)))
            item.resume = rp
            item.Select = function(self)
                menu.pause.Cancel()
                coop.ResumeAt(self.resume)
            end
        end
        if (coop.level and coop.level >= 1) then
            item = mdkCreateMenuItem(m, coop.save_note or "Save now")
            item.Select = function(self)
                if (CanSaveGameNow() and coop_all_ready()) then
                    coop.SaveNow()
                    coop.save_note = "Saved"
                else
                    coop.save_note = "Cannot save here"
                end
                coop.BuildSession()
            end
        end
    end
    coop.BackItem(m, "coop")
end

-- Start at level...: pick a level and one of its checkpoints, then the host loads it and the
-- clients follow (a new co-op session, so the old one is kept). coop.start_i indexes
-- coop.checkpoints, which is in play order --
menu.coopstart = mdkCreateMenu(0.5, 0.25, 0.05, 0.05, 0.01, JUSTIFY_CENTER, 1)
coop.pages.coopstart = function() coop.BuildStart() end

-- number of the checkpoint in its level (1 = the level's start) and how many there are --
function coop.CheckpointOfLevel(i)
    local l = coop.checkpoints[i][1]
    local first, last = i, i
    while (first > 1 and coop.checkpoints[first - 1][1] == l) do first = first - 1 end
    while (last < coop.ncheckpoints and coop.checkpoints[last + 1][1] == l) do last = last + 1 end
    return i - first + 1, last - first + 1, first
end

function coop.BuildStart()
    local m = menu.coopstart
    mdkClearMenu(m)
    if (not coop.start_i) then coop.start_i = 1 end
    local i = coop.start_i
    local n, of = coop.CheckpointOfLevel(i)
    local c = coop.checkpoints[i]
    coop.Heading(m, "Start a new game at")
    local place, time, hero = coop_level_info(c[1])
    local item = mdkCreateMenuItem(m, format("Level: %d", c[1]))
    item.Select = function(self)
        -- the first checkpoint of the next level (this Lua has no upvalues: look it up again) --
        local j = coop.start_i
        local l = coop.checkpoints[j][1]
        while (j <= coop.ncheckpoints and coop.checkpoints[j][1] == l) do j = j + 1 end
        if (j > coop.ncheckpoints) then j = 1 end
        coop.start_i = j
        coop.BuildStart()
        mdkMenuSetHighlighted(menu.coopstart, 1)
    end
    item = mdkCreateMenuItem(m, format("Checkpoint: %d of %d", n, of))
    item.Select = function(self)
        local n, of, first = coop.CheckpointOfLevel(coop.start_i)
        if (n >= of) then coop.start_i = first else coop.start_i = coop.start_i + 1 end
        coop.BuildStart()
        mdkMenuSetHighlighted(menu.coopstart, 2)
    end
    if (place ~= "") then coop.Heading(m, format("%s, %s", place, time)) end
    if (hero ~= "") then coop.Heading(m, hero) end
    item = mdkCreateMenuItem(m, format("Start at level %d, checkpoint %d", c[1], n))
    item.Select = function(self)
        menu.pause.Cancel()
        coop.StartAt(coop.checkpoints[coop.start_i][1], coop.checkpoints[coop.start_i][2])
    end
    coop.BackItem(m, "coopsession")
end

menu.coopstart.Cancel = function() coop.ShowPage("coopsession") end

-- host: a new game at level l checkpoint cp --
function coop.StartAt(l, cp)
    if (coop_role() ~= "host") then
        coop_log("start at: only the host picks the level")
        return
    end
    coop_log(format("starting a new game at level %d checkpoint %d", l, cp))
    coop.sid = nil
    mdkNewGame(l, cp)
end

-- "address:port" from the text box: kept for the next start and, on a client, used at once --
function coop.SetHostAddress(text)
    local colon = strfind(text, ":")
    local host, port = text, coop_get("net", "peer_port", "27900")
    if (colon) then
        host = strsub(text, 1, colon - 1)
        port = strsub(text, colon + 1)
    end
    port = tonumber(port)
    if (not port or port < 1 or port > 65535 or not strfind(host, "^%d+%.%d+%.%d+%.%d+$")) then
        coop.host_note = "Address must look like 192.168.1.20:27900"
        return
    end
    coop.host_note = nil
    coop.join_cancelled = nil
    coop.join_state = nil
    coop_join_retry()
    if (coop_role() == "client" and coop_set_host(host, port)) then
        coop.host_note = "Connecting to " .. host .. ":" .. port
    else
        coop_set("net", "peer_host", host)
        coop_set("net", "peer_port", port)
    end
end

menu.coopsession.Update = function()
    coop.EditUpdate(menu.coopsession, "coopsession")
    if (not menu.coopsession.editing and coop_transport() == "steam" and
        (coop.session_status ~= coop_steam_status() or coop.session_lobby ~= coop_steam_lobby())) then
        coop.BuildSession()
    end
end
menu.coopsession.Cancel = function()
    if (not menu.coopsession.editing) then
        coop.save_note = nil
        coop.ShowPage("coop")
    end
end

-- gameplay page --
coop.intervals = { [0] = 60, [1] = 120, [2] = 300, [3] = 600 }

-- difficulty (this Lua is 3.1: no for loops, no getn): every setting is a [game] key; a preset is just a set of values for them, and
-- the current preset is whichever one the keys match ("Custom" when none does) --
coop.diffpresets = {
    { name = "Story",     friendly_fire = 0, enemy_hp = 75,  enemy_scale = 0,   dmg_taken = 50,  dmg_dealt = 100, down_secs = 60, revive_secs = 2 },
    { name = "Normal",    friendly_fire = 0, enemy_hp = 100, enemy_scale = 50,  dmg_taken = 100, dmg_dealt = 100, down_secs = 30, revive_secs = 3 },
    { name = "Hard",      friendly_fire = 0, enemy_hp = 125, enemy_scale = 75,  dmg_taken = 150, dmg_dealt = 100, down_secs = 20, revive_secs = 4 },
    { name = "Nightmare", friendly_fire = 1, enemy_hp = 150, enemy_scale = 100, dmg_taken = 200, dmg_dealt = 100, down_secs = 10, revive_secs = 5 },
}
coop.diffkeys = { "friendly_fire", "enemy_hp", "enemy_scale", "dmg_taken", "dmg_dealt", "down_secs", "revive_secs" }
-- the choices of each setting on the custom page, with how many there are --
coop.diffopts = {
    friendly_fire = { label = "Friendly fire",             values = { 0, 1 },                          texts = { "Off", "On" }, n = 2 },
    enemy_hp      = { label = "Enemy health",              values = { 50, 75, 100, 125, 150, 200, 300 }, texts = { "50%", "75%", "100%", "125%", "150%", "200%", "300%" }, n = 7 },
    enemy_scale   = { label = "More health per player",    values = { 0, 25, 50, 75, 100, 150, 200 },    texts = { "Off", "+25%", "+50%", "+75%", "+100%", "+150%", "+200%" }, n = 7 },
    dmg_taken     = { label = "Damage you take",           values = { 25, 50, 75, 100, 150, 200, 300 },  texts = { "25%", "50%", "75%", "100%", "150%", "200%", "300%" }, n = 7 },
    dmg_dealt     = { label = "Damage you deal",           values = { 50, 75, 100, 125, 150, 200 },      texts = { "50%", "75%", "100%", "125%", "150%", "200%" }, n = 6 },
    down_secs     = { label = "Time before bleeding out",  values = { 10, 20, 30, 45, 60, 120 },         texts = { "10 s", "20 s", "30 s", "45 s", "60 s", "2 min" }, n = 6 },
    revive_secs   = { label = "Time to revive",            values = { 1, 2, 3, 4, 5, 8 },                texts = { "1 s", "2 s", "3 s", "4 s", "5 s", "8 s" }, n = 6 },
}

function coop.DiffGet(key)
    local normal = coop.diffpresets[2][key]
    return tonumber(coop_get("game", key, normal)) or normal
end

-- index (1-based) of the preset the settings match, or nil --
function coop.DiffPresetIndex()
    local i = 1
    while (coop.diffpresets[i]) do
        local match = 1
        local k = 1
        while (coop.diffkeys[k]) do
            local key = coop.diffkeys[k]
            if (coop.DiffGet(key) ~= coop.diffpresets[i][key]) then match = nil end
            k = k + 1
        end
        if (match) then return i end
        i = i + 1
    end
    return nil
end

function coop.DiffPresetName()
    local i = coop.DiffPresetIndex()
    if (i) then return coop.diffpresets[i].name end
    return "Custom"
end

function coop.SetDiffPreset(i)
    local k = 1
    while (coop.diffkeys[k]) do
        coop_set("game", coop.diffkeys[k], coop.diffpresets[i][coop.diffkeys[k]])
        k = k + 1
    end
end

-- the combo position (0-based) of a setting's current value; one not in the list (edited
-- coop.ini) shows as the nearest choice below it --
function coop.DiffAt(key)
    local opt = coop.diffopts[key]
    local at, v, n = 0, coop.DiffGet(key), 1
    while (n <= opt.n) do
        if (opt.values[n] <= v) then at = n - 1 end
        n = n + 1
    end
    return at
end

-- after any change: every row and the Preset row show what the settings are now. The rows
-- are changed in place; rebuilding the page from inside a row's own handler is not safe. --
function coop.RefreshDifficulty()
    local k = 1
    while (coop.diffkeys[k]) do
        mdkMenuItemSetWidgitValue(coop.diffrows[coop.diffkeys[k]], coop.DiffAt(coop.diffkeys[k]))
        k = k + 1
    end
    mdkMenuItemSetWidgitValue(coop.diffpreset, (coop.DiffPresetIndex() or 5) - 1)
end

-- difficulty page: the presets, then each setting on its own --
function coop.BuildDifficulty()
    local m = menu.coopdiff
    mdkClearMenu(m)
    coop.Heading(m, "Difficulty")

    local item = mdkCreateMenuItem(m, "Preset")
    mdkMenuItemAddComboBox(item)
    local i = 1
    while (coop.diffpresets[i]) do
        mdkMenuItemAddComboString(item, coop.diffpresets[i].name)
        i = i + 1
    end
    mdkMenuItemAddComboString(item, "Custom")
    mdkMenuItemSetWidgitValue(item, (coop.DiffPresetIndex() or i) - 1)
    coop.diffpreset = item
    item.Inc = function(self)
        local n = mdkMenuItemGetWidgitValue(self) + 1
        if (coop.diffpresets[n]) then
            coop.SetDiffPreset(n)
            coop.RefreshDifficulty()
        end
    end
    item.Dec = item.Inc

    coop.diffrows = {}
    local k = 1
    while (coop.diffkeys[k]) do
        local key = coop.diffkeys[k]
        local opt = coop.diffopts[key]
        local row = mdkCreateMenuItem(m, opt.label)
        mdkMenuItemAddComboBox(row)
        local n = 1
        while (n <= opt.n) do
            mdkMenuItemAddComboString(row, opt.texts[n])
            n = n + 1
        end
        mdkMenuItemSetWidgitValue(row, coop.DiffAt(key))
        coop.diffrows[key] = row
        row.key = key
        row.opt = opt
        row.Inc = function(self)
            coop_set("game", self.key, self.opt.values[mdkMenuItemGetWidgitValue(self) + 1])
            coop.RefreshDifficulty()
        end
        row.Dec = row.Inc
        k = k + 1
    end

    coop.BackItem(m, "coopgame")
end

menu.coopdiff.Cancel = function() coop.ShowPage("coopgame") end

-- index of value in a 0-based table, or dflt --
function coop.IndexOf(t, value, dflt)
    local i = 0
    while (t[i]) do
        if (t[i] == value) then return i end
        i = i + 1
    end
    return dflt
end

function coop.BuildGame()
    local m = menu.coopgame
    mdkClearMenu(m)
    coop.Heading(m, "Gameplay")

    local item = mdkCreateMenuItem(m, "Friendly fire")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("game", "friendly_fire", "0")) or 0)
    item.Select = function(self, n) coop_set("game", "friendly_fire", n) end

    item = mdkCreateMenuItem(m, "Back to level's hero")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("game", "char_follow", "1")) or 1)
    item.Select = function(self, n) coop_set("game", "char_follow", n) end

    item = mdkCreateMenuItem(m, "Difficulty: " .. coop.DiffPresetName())
    item.Select = function(self) coop.ShowPage("coopdiff") end

    item = mdkCreateMenuItem(m, "Co-op save every")
    mdkMenuItemAddComboBox(item)
    mdkMenuItemAddComboString(item, "1 min")
    mdkMenuItemAddComboString(item, "2 min")
    mdkMenuItemAddComboString(item, "5 min")
    mdkMenuItemAddComboString(item, "10 min")
    mdkMenuItemSetWidgitValue(item, coop.IndexOf(coop.intervals, coop.save_interval, 1))
    item.Inc = function(self)
        coop.save_interval = coop.intervals[mdkMenuItemGetWidgitValue(self)]
        coop_set("save", "interval", coop.save_interval)
    end
    item.Dec = item.Inc

    item = mdkCreateMenuItem(m, "Developer keys")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("controls", "devkeys", "0")) or 0)
    item.Select = function(self, n)
        coop_set("controls", "devkeys", n)
        coop.ApplyDevKeys(n == 1)
    end

    coop.BackItem(m, "coop")
end

menu.coopgame.Cancel = function() coop.ShowPage("coop") end

-- Options > Co-op > Interface: what the mod adds to the screen and the keys, each can be switched off --
function coop.CheckItem(m, label, section, key, dflt, apply)
    local item = mdkCreateMenuItem(m, label)
    mdkMenuItemAddCheckBox(item, tonumber(coop_get(section, key, dflt)) or tonumber(dflt))
    item.sec, item.key, item.apply = section, key, apply
    item.Select = function(self, n)
        coop_set(self.sec, self.key, n)
        if (self.apply) then self.apply(n == 1) end
    end
    return item
end

function coop.BuildInterface()
    local m = menu.coopui
    mdkClearMenu(m)
    coop.Heading(m, "Interface")
    coop.CheckItem(m, "Players on screen", "hud", "show", "1", function(on) coop.hud_on = on end)
    coop.CheckItem(m, "Player outlines", "hud", "outline", "0", function(on) coop.hud_outline = on end)
    coop.CheckItem(m, "Cutscene timer", "hud", "movietimer", "1", function(on) coop.movietimer = on end)
    coop.CheckItem(m, "Friends in minigames", "features", "ghosts", "1", nil)
    coop.CheckItem(m, "Emote wheel", "features", "emotes", "1", function(on) coop.ApplyControls() end)
    coop.CheckItem(m, "Character wheel", "features", "charwheel", "1", function(on) coop.ApplyControls() end)
    coop.CheckItem(m, "Text chat", "features", "chat", "1", function(on) coop.ApplyControls() end)
    coop.CheckItem(m, "Pings", "features", "pings", "1", function(on) coop.ApplyControls() end)
    coop.CheckItem(m, "Gamepad (next start)", "pad", "enabled", "1", nil)
    coop.BackItem(m, "coop")
end

menu.coopui.Cancel = function() coop.ShowPage("coop") end

-- ---------------------------------------------------------------------------------------
-- Options > Co-op > Controls: the keyboard and mouse picture (bridge, ctrlview.c) fills the
-- left, this menu the right: page, preset, then the page's actions with their keys
-- ---------------------------------------------------------------------------------------
menu.coopctrl = mdkCreateMenu(0.78, 0.09, 0.034, 0.034, 0.004, JUSTIFY_RIGHT, 1)

coop.ctrlpage = 1
coop.ctrlnote = ""

-- rows shown without an action of their own: keys another action owns ("from"), or fixed ones --
coop.derived = {
    foot = {
        { codes = { 1004, 1006 }, group = 2, short = "Look", long = "Move the mouse to turn and look around" },
    },
    kurt = {
        { from = "jump", group = 3, short = "Chute", long = "Parachute: hold jump while falling" },
        { from = "sniper", group = 6, short = "Sniper", long = "Sniper mode on / off" },
        { from = "shoot", group = 4, short = "Fire", long = "Fire / use the selected item" },
    },
    sniper = {
        { from = "strl", group = 1, short = "Left", long = "Sidestep left" },
        { from = "strr", group = 1, short = "Right", long = "Sidestep right" },
        { from = "shoot", group = 4, short = "Fire", long = "Fire the sniper rifle" },
        { codes = { 1004, 1006 }, group = 2, short = "Aim", long = "Aim: move the mouse" },
    },
    max = {
        { from = "jump", group = 3, short = "Jet", long = "Jetpack: hold jump" },
        { from = "shoot", group = 4, short = "Fire", long = "Fire the equipped weapon" },
    },
    doc = {
        { from = "shoot", group = 4, short = "Use", long = "Use the selected item" },
        { from = "jump", group = 3, short = "Jump", long = "Jump" },
    },
    mini = {
        { from = "fwd", group = 1, short = "S1 Up", long = "Stick 1 up" },
        { from = "back", group = 1, short = "S1 Dn", long = "Stick 1 down" },
        { from = "turnl", group = 1, short = "S1 L", long = "Stick 1 left" },
        { from = "turnr", group = 1, short = "S1 R", long = "Stick 1 right" },
        { from = "lookup", group = 2, short = "S2 Up", long = "Stick 2 up (two-stick minigames)" },
        { from = "lookdn", group = 2, short = "S2 Dn", long = "Stick 2 down" },
        { from = "strl", group = 1, short = "S2 L", long = "Stick 2 left" },
        { from = "strr", group = 1, short = "S2 R", long = "Stick 2 right" },
    },
    coop = {
        { codes = { 59 }, group = 8, short = "Guide", long = "Quick guide: hold for keybinds, features and how co-op works" },
        { codes = { 1000 }, group = 8, short = "Next", long = "While you are out of the fight: watch the next teammate" },
        { codes = { 60 }, group = 8, short = "Mark", long = "Co-op marker: a note in every log and a screenshot, for bug reports" },
    },
    menus = {
        { codes = { 200 }, group = 9, short = "Up", long = "Menu up" },
        { codes = { 208 }, group = 9, short = "Down", long = "Menu down" },
        { codes = { 203 }, group = 9, short = "Left", long = "Menu left, a smaller value" },
        { codes = { 205 }, group = 9, short = "Right", long = "Menu right, a bigger value" },
        { codes = { 28, 1000 }, group = 9, short = "Select", long = "Choose the highlighted item" },
        { codes = { 1 }, group = 9, short = "Back", long = "Back out of a menu" },
        { codes = { 211, 14 }, group = 9, short = "Clear", long = "Clear a key in the key lists" },
    },
    system = {
        { codes = { 1 }, group = 7, short = "Esc",   long = "Pause menu" },
        { codes = { 68 }, group = 7, short = "Shot", long = "Screenshot (saved by the game)" },
        { codes = { 41 }, group = 7, short = "Console", long = "Developer console" },
    },
}

-- the game's own pages of keys that cannot be changed here --
coop.fixedpage = { mini = 1, menus = 1, system = 1, pad = 1 }

-- the gamepad page: button, colour group, label on the picture, what it does (act: the action whose
-- key it presses, shown beside it). Menus use the stick / D-pad as arrows, A as Enter, B / Start as Esc --
coop.padrows = {
    { btn = "LS",   group = 1, short = "Move",   long = "Move and strafe (the D-pad too); menus: up / down / left / right" },
    { btn = "RS",   group = 2, short = "Look",   long = "Look around (mouse look)" },
    { btn = "A",    group = 3, short = "Jump",   long = "Jump, jetpack, parachute; menus: select", act = "jump" },
    { btn = "B",    group = 9, short = "Back",   long = "Pause, menus: back" },
    { btn = "START", group = 9, short = "Esc",   long = "Pause, menus: back" },
    { btn = "X",    group = 8, short = "Rev",    long = "Hold beside a downed teammate", act = "revive" },
    { btn = "Y",    group = 5, short = "Inv",    long = "Open the inventory (Max: equip)", act = "kuse" },
    { btn = "BACK", group = 8, short = "Ping",   long = "Ping what you aim at", act = "ping" },
    { btn = "RT",   group = 4, short = "Fire",   long = "Fire, melee, use the selected item", act = "shoot" },
    { btn = "LT",   group = 6, short = "Sniper", long = "Sniper mode on / off (Kurt)", act = "sniper" },
    { btn = "RB",   group = 5, short = "Next",   long = "Next item (mouse wheel up)" },
    { btn = "LB",   group = 5, short = "Prev",   long = "Previous item (mouse wheel down)" },
}

function coop.FeedPad()
    coop_ctrl_padmode(1)
    local n, r = next(coop.padrows, nil)
    while (n) do
        local keys = r.btn
        if (r.btn == "LS") then keys = "Left stick / D-pad"
        elseif (r.btn == "RS") then keys = "Right stick"
        elseif (r.btn == "START") then keys = "Start"
        elseif (r.btn == "BACK") then keys = "Back" end
        local long = r.long
        if (r.act) then
            local c = coop.PadKey(r.act)
            if (c >= 0) then long = long .. "  [" .. coop.KeyName(c) .. "]" end
        end
        coop_ctrl_padbtn(r.btn, r.group, r.short)
        coop_ctrl_line(r.group, keys, long)
        n, r = next(coop.padrows, n)
    end
    coop_ctrl_hint("A pad (XInput) plays too: it presses the keys of your preset, set on the other pages      Left / Right on Page: next page")
end

function coop.KeysOfRow(r)
    if (r.from) then
        local k = coop.Keys(r.from)
        return { k[1], k[2] }
    end
    return { r.codes[1] or -1, r.codes[2] or -1 }
end

function coop.MenuName(a)
    return a.name
end

-- send the current page to the bridge's picture --
function coop.FeedControls()
    local pid = coop.pageid[coop.ctrlpage]
    coop_log("controls page: " .. pid)
    coop_ctrl_clear("Controls: " .. coop.ctrlpages[coop.ctrlpage], "Preset: " .. coop.preset_names[coop.preset] .. coop.ctrlnote)
    local i = 1
    while (coop.ctrlpages[i]) do
        local on = 0
        if (i == coop.ctrlpage) then on = 1 end
        coop_ctrl_tab(coop.ctrlpages[i], on)
        i = i + 1
    end
    if (pid == "pad") then
        coop.FeedPad()
        return
    end
    local n, a = next(coop.actions, nil)
    while (n) do
        if (a.page == pid) then
            local k = coop.Keys(a.id)
            if (k[1] >= 0) then coop_ctrl_bind(k[1], a.group, a.short) end
            if (k[2] >= 0) then coop_ctrl_bind(k[2], a.group, a.short) end
            coop_ctrl_line(a.group, coop.KeyText(k), a.long)
        end
        n, a = next(coop.actions, n)
    end
    local rows = coop.derived[pid]
    if (rows) then
        n, a = next(rows, nil)
        while (n) do
            local k = coop.KeysOfRow(a)
            if (k[1] >= 0) then coop_ctrl_bind(k[1], a.group, a.short) end
            if (k[2] >= 0) then coop_ctrl_bind(k[2], a.group, a.short) end
            if (a.codes and a.codes[1] == 1004) then
                coop_ctrl_line(a.group, "Mouse", a.long)
            else
                coop_ctrl_line(a.group, coop.KeyText(k), a.long)
            end
            n, a = next(rows, n)
        end
    end
    if (coop.rebinding) then
        coop_ctrl_hint("Press a key or a mouse button for " .. coop.MenuName(coop.ActionById(coop.rebinding)) .. "   (Esc: cancel)")
    elseif (coop.fixedpage[pid]) then
        coop_ctrl_hint("Left / Right on Page: next page      These keys follow other pages and are not set here")
    else
        coop_ctrl_hint("Left / Right on Page: next page      Select an action, press a key to bind it      Delete: clear")
    end
end

function coop.BuildControls()
    local m = menu.coopctrl
    mdkClearMenu(m)
    local pid = coop.pageid[coop.ctrlpage]

    local item = mdkCreateMenuItem(m, "Page")
    mdkMenuItemAddComboBox(item)
    local i = 1
    while (coop.ctrlpages[i]) do
        mdkMenuItemAddComboString(item, coop.ctrlpages[i])
        i = i + 1
    end
    mdkMenuItemSetWidgitValue(item, coop.ctrlpage - 1)
    item.Inc = function(self)
        coop.ctrlpage = mdkMenuItemGetWidgitValue(self) + 1
        coop.ctrl_focus = 0
        coop.ctrl_dirty = 1
    end
    item.Dec = item.Inc

    item = mdkCreateMenuItem(m, "Preset")
    mdkMenuItemAddComboBox(item)
    i = 1
    while (coop.preset_order[i]) do
        mdkMenuItemAddComboString(item, coop.preset_names[coop.preset_order[i]])
        i = i + 1
    end
    local pi = 0
    i = 1
    while (coop.preset_order[i]) do
        if (coop.preset_order[i] == coop.preset) then pi = i - 1 end
        i = i + 1
    end
    mdkMenuItemSetWidgitValue(item, pi)
    item.Inc = function(self)
        coop.SetPreset(coop.preset_order[mdkMenuItemGetWidgitValue(self) + 1])
        coop.ctrlnote = ""
        coop.ctrl_focus = 1
        coop.ctrl_dirty = 1
    end
    item.Dec = item.Inc

    local n, a = next(coop.actions, nil)
    local pos = 2
    while (n) do
        if (a.page == pid) then
            item = mdkCreateMenuItem(m, coop.MenuName(a))
            item.action = a.id
            item.pos = pos
            pos = pos + 1
            mdkMenuItemAddTextBox(item, coop.ShortKeyText(coop.Keys(a.id)), 0.14)
            item.Select = function(self) coop.BeginRebind(self.action, self.pos) end
            item.Clear = function(self) coop.ClearKeys(self.action, self.pos) end
        end
        n, a = next(coop.actions, n)
    end

    item = mdkCreateMenuItemW(m, strings.opt_ok)
    mdkMenuItemSetSponge(item, 0.03)
    item.Select = function(self) menu.coopctrl.Cancel() end
    coop.FeedControls()
end

function coop.OpenControls()
    coop.ctrlnote = ""
    coop.ctrl_open = 1
    coop.ctrl_focus = 0
    coop.BuildControls()
    coop_ctrl_show(1)
end

function coop.CloseControls()
    coop.ctrl_open = nil
    coop.rebinding = nil
    coop_ctrl_show(0)
end

-- begin: switch to Custom (a copy of what is active), then wait for the next key --
function coop.BeginRebind(id, pos)
    coop.ctrl_focus = pos
    if (coop.preset ~= "custom") then
        local from = coop.preset_names[coop.preset]
        coop.EditCustom()
        coop.ctrlnote = "  (copied from " .. from .. ")"
    end
    coop.rebinding = id
    coop.rebind_timeout = 6
    conscheme = "poo"
    mdkShowMouse(0, 0)
    coop.FeedControls()
    coop_ctrl_focus(0)
end

function coop.EndRebind()
    coop.rebinding = nil
    mdkShowMouse(1, 0)
    conscheme = "menu"
    coop.ctrl_dirty = 1
end

function coop.ClearKeys(id, pos)
    coop.ctrl_focus = pos
    if (coop.preset ~= "custom") then
        coop.EditCustom()
    end
    coop.custom[id] = { -1, -1 }
    coop.SaveCustom(id)
    coop.ApplyControls()
    coop.ctrl_dirty = 1
end

function coop.PollRebind()
    local id = coop.rebinding
    coop.rebind_timeout = coop.rebind_timeout - chGetDeltaT()
    local k = omGetKeyPress()
    if (coop.rebind_timeout <= 0) then
        k = 1
        omEnableControl()
    end
    if (k == -1) then return end
    if (k ~= 1 and coop.ActionById(id).gk and (k == 1008 or k == 1009)) then
        coop.ctrlnote = "  (the wheel cannot be used for " .. coop.MenuName(coop.ActionById(id)) .. ")"
    elseif (k ~= 1) then
        local other = coop.Conflict(id, k)
        if (other) then
            coop.RemoveKey(other, k)
            coop.SaveCustom(other)
            coop.ctrlnote = "  (" .. coop.KeyName(k) .. " taken from " .. coop.MenuName(coop.ActionById(other)) .. ")"
        else
            coop.ctrlnote = ""
        end
        coop.AddKey(id, k)
        coop.SaveCustom(id)
        coop.ApplyControls()
        coop_log(format("controls: %s = %d, %d", id, coop.custom[id][1], coop.custom[id][2]))
    end
    coop.EndRebind()
end

menu.coopctrl.Update = function()
    if (coop.rebinding) then
        coop.PollRebind()
        return
    end
    if (coop.ctrl_dirty) then
        coop.ctrl_dirty = nil
        coop.BuildControls()
        mdkMenuSetHighlighted(menu.coopctrl, coop.ctrl_focus or 0)
    end
end

menu.coopctrl.Cancel = function()
    if (coop.rebinding) then
        coop.EndRebind()
        return
    end
    coop.CloseControls()
    coop.ShowPage("coop")
end

-- the picture goes away with the menu, however the menu was left --
function coop.ControlsCheck()
    if (coop.ctrl_open and menu.current ~= "coopctrl") then coop.CloseControls() end
end

-- voice chat page: proximity voice (bridge/voice.c); the range is the host's setting, the
-- push-to-talk key is on the Controls page --
coop.voice_volumes = { [0] = 50, [1] = 100, [2] = 150, [3] = 200 }
coop.voice_ranges = { [0] = 25, [1] = 50, [2] = 100, [3] = 0 }

function coop.BuildVoice()
    local m = menu.coopvoice
    mdkClearMenu(m)
    coop.Heading(m, "Voice chat")

    local item = mdkCreateMenuItem(m, "Voice chat")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("voice", "enabled", "1")) or 1)
    item.Select = function(self, n) coop_set("voice", "enabled", n) end

    item = mdkCreateMenuItem(m, "Push to talk: " .. coop.KeyText(coop.Keys("voice")))
    item.Select = function(self)
        coop.ctrlpage = 7       -- Co-op
        coop.ShowPage("coopctrl")
    end

    item = mdkCreateMenuItem(m, "Voice volume")
    mdkMenuItemAddComboBox(item)
    mdkMenuItemAddComboString(item, "Quiet")
    mdkMenuItemAddComboString(item, "Normal")
    mdkMenuItemAddComboString(item, "Loud")
    mdkMenuItemAddComboString(item, "Louder")
    mdkMenuItemSetWidgitValue(item, coop.IndexOf(coop.voice_volumes, tonumber(coop_get("voice", "volume", "100")), 1))
    item.Inc = function(self) coop_set("voice", "volume", coop.voice_volumes[mdkMenuItemGetWidgitValue(self)]) end
    item.Dec = item.Inc

    item = mdkCreateMenuItem(m, "Hearing range (host)")
    mdkMenuItemAddComboBox(item)
    mdkMenuItemAddComboString(item, "Short")
    mdkMenuItemAddComboString(item, "Medium")
    mdkMenuItemAddComboString(item, "Long")
    mdkMenuItemAddComboString(item, "Everywhere")
    mdkMenuItemSetWidgitValue(item, coop.IndexOf(coop.voice_ranges, tonumber(coop_get("voice", "range", "50")), 1))
    item.Inc = function(self) coop_set("voice", "range", coop.voice_ranges[mdkMenuItemGetWidgitValue(self)]) end
    item.Dec = item.Inc

    coop.BackItem(m, "coop")
end

menu.coopvoice.Cancel = function() coop.ShowPage("coop") end

coop.ApplyDevKeys(coop_get("controls", "devkeys", "0") == "1")
