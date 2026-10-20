-------------------------------------------------------------------------------
--  coop_dev.lua
--
--  Dev harness for MDK2 HD co-op. Installed as <game dir>\coop\coop.lua and run by
--  dinput8.dll from coop_init(), which a one-line hook in settings.lua calls after
--  mdk2.lua, so every game function is defined. Needs the bridge DLL.
--  Lua 3.1: no 'local function', closures need %upvalues.
-------------------------------------------------------------------------------

coop = {
    offset = 3,         -- puppet sits this far from the player on x until the peer is heard
    mirror = 0,         -- height above the peer's position, for loopback testing
    marks = 0,          -- F2 markers written to bridge.log
    enemies = 0,        -- Doganboys spawned with F5
}

-- event kinds, as in bridge.h --
EV_HIT = 1
EV_APPLY = 2
EV_HURT = 3
EV_SPAWN = 4
EV_LEVEL = 8

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

-- Controls: Space jumps (and works Max's jetpack and Kurt's parachute, which share the jump
-- button), right mouse toggles Kurt's sniper mode. The game's defaults are the other way round
-- (right mouse 1001 = jump/jet/chute, Space 57 = sniper toggle). Applied after settings.lua,
-- so they win over the saved bindings; changing controls in the options menu saves them. --
omBindCommandI(COM_JUMP, 57)
omBindCommandI(COM_JET, 57)
omBindCommandI(COM_CHUTE, 57)
omBindCommandI(COM_SMTOGGLE, 1001)

omMakeCommand(COM_TOGL1, "F6", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL2, "F7", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL3, "F8", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL4, "F11", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGLG, "F4", CON_BUTTON_FALL, 0, 0)

-- a gob by the name the peer uses: our player, a named gob, or an "n<id>" network name --
function coop.FindGob(name)
    if (name == "@player") then
        return mdkGetPlayerGob()
    end
    local gob = getglobal(name)
    if (gob and type(gob) ~= "number" and type(gob) ~= "string" and coop_alive(gob)) then
        return gob
    end
    return coop_gob(name)
end

-- client: bind a host actor to our gob of the same name, creating it if we lack one --
function coop.Spawn(name, netid, objtype, x, y, z)
    local gob = coop.FindGob(name)
    if (not gob) then
        gob = mdkCreateObjectLua(name, objtype, mdkGetScene(), nil, nil, x,y,z, 1,0,0,0, nil,0,0,0,0)
    end
    if (gob) then
        coop_apply(EV_SPAWN, gob, netid)
    else
        coop_log(format("spawn %s type %d failed", name, objtype))
    end
end

-- carry out events from the peer --
function coop.HandleEvents()
    local kind, name, damage, dtype, part, netid, objtype, x, y, z = coop_event()
    while (kind) do
        if (kind == EV_LEVEL) then
            -- the host started level dtype at checkpoint damage: follow, unless our own copy
            -- of the script already took us there --
            if (damage < 0) then
                -- a reload from the host's autosave: ours does not match, and loading it
                -- crashed the client (mdkGobCreateInstant) --
                coop_log(format("host reloaded level %d from its save: not followed", dtype))
            elseif (coop.level == dtype and coop.cp == damage) then
                coop_log(format("host started level %d checkpoint %d: already here", dtype, damage))
            else
                coop_log(format("host started level %d checkpoint %d: following", dtype, damage))
                coop.following = dtype * 1000 + damage
                mdkNewGame(dtype, damage)
            end
        elseif (kind == EV_SPAWN) then
            coop.Spawn(name, netid, objtype, x, y, z)
        else
            local target = coop.FindGob(name)
            if (target) then
                coop_apply(kind, target, damage, dtype, part)
            else
                coop_log(format("event %d: no gob named '%s'", kind, name))
            end
        end
        kind, name, damage, dtype, part, netid, objtype, x, y, z = coop_event()
    end
end

-- every frame, called by the bridge after the scene update (menus and title included):
-- send our player, apply the peer's events, put the puppet where the peer's player is --
function coop.Tick()
    -- host autostart (tools/play.sh <level> <cp>): once the title screen has been up a while,
    -- load the start checkpoint; retried until a level is running --
    if (coop.autostart and coop.level == 0) then
        coop.ticks = (coop.ticks or 0) + 1
        if (coop.ticks >= (coop.nextstart or 600)) then
            coop.nextstart = coop.ticks + 600
            coop_log(format("autostart: level %d checkpoint %d", coop.autostart[1], coop.autostart[2]))
            mdkNewGame(coop.autostart[1], coop.autostart[2])
        end
    elseif (coop.autostart and coop.level and coop.level >= 1) then
        coop.autostart = nil
    end
    local p = mdkGetPlayerGob()
    if (p == coop.p2) then p = nil end
    coop_send(p)
    local x, y, z = coop_recv()
    coop.HandleEvents()
    if (p and not coop.p2 and coop.level and coop.level >= 1) then
        coop.SpawnPuppet()      -- the player only exists a little after level() returns
    end
    local gob = coop.p2
    if (not gob or not p) then return end
    if (x) then
        x, y = coop.KeepApart(p, x, y)
        mdkGobSetPositionXYZ(gob, x, y, z + coop.mirror)
        coop_face(gob)
    else
        mdkGobSetPositionXYZ(gob, p.position.x + coop.offset, p.position.y, p.position.z)
    end
end

-- The puppet is solid and moved every frame; placed inside our player (both spawn on the
-- same point) it shoves us across the room (25 units into a wall at level 1's start).
-- Keep it at least coop.apart units away horizontally (z is up). --
coop.apart = 3
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

-- the puppet is the same character as our player: each level fixes who you play and only
-- loads that model, so another type could be missing --
function coop.SpawnPuppet()
    local p = mdkGetPlayerGob()
    if (not p) then return end
    local ptype = mdkGetGobType(p)
    local peer = coop_peer_type()
    if (peer and peer ~= ptype) then
        coop_log(format("partner plays type %d, we play %d: puppet uses ours", peer, ptype))
    end
    coop.p2 = mdkCreateObjectLua("coop_p2", ptype, mdkGetScene(), nil, nil, 0,0,0, 1,0,0,0, nil,0,0,0,0)
    mdkGobSetPositionXYZ(coop.p2, p.position.x + coop.offset, p.position.y, p.position.z)
    mdkSetLuaEvent(coop.p2, "OnUpdate", coop.PuppetOnUpdate)

end

-- every level load (menu, script, save, debug) ends in level(): reset the bridge for the
-- new level and, on the host, ask the client to follow; Tick adds the puppet --
coop.level_orig = level
function level(l, cp, sec)
    coop.level_orig(l, cp, sec)
    -- only the host's level changes count: both games run the same scripts, so script-driven
    -- loads (new game: 0 -> 1,9 -> 1,12 -> 1,1) happen on both and must not echo --
    local announce = 0
    if (coop_role() == "host" and l >= 1) then announce = 1 end
    -- checkpoint -1 is a reload from the autosave (after a death): tell the client which
    -- checkpoint that is instead; it cannot load our save --
    if (cp < 0) then cp = mdkGetCurrentCheckPoint() end
    if (cp < 0) then announce = 0 end
    coop.following = nil
    coop.p2 = nil
    coop.level = l
    coop.cp = cp
    coop_level(l, cp, announce)
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
    if (coop.p2) then coop_animdump(coop.p2) end
    coop_listgobs()
    if (not coop.typeslogged) then coop.LogObjTypes() coop.typeslogged = 1 end
end

-- F4: teleport the player onto the next test spot: level 1's decoy item (next room, standing
-- on it picks it up), then door dr1_03 (another room) --
coop.tpspots = { "l1_r3dummy", "dr1_03" }
coop.ntpspots = 2      -- Lua 3.1 has no getn
coop.tpnext = 1
comfuncs.fkey[COM_TOGLG] = function()
    local name = coop.tpspots[coop.tpnext]
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

-- F1 (was debug.lua's bars): host spawns a Poopsy ahead, always named "coop_spawn" like a
-- spawner's spawns, to test duplicate names; the client hits coop_dog1 ten times for 2 with
-- its gun's damage type, through the normal damage function (as if shot, without aiming) --
comfuncs.fkey[COM_SHOWBARS] = function()
    if (coop_role() == "host") then
        make("coop_spawn", OBJ_POOPSY)
        return
    end
    local dog = coop.FindGob("coop_dog1")
    local i = 0
    while (dog and i < 10) do
        mdkDealDamage(mdkGetPlayerGob(), dog, 2, 1, -1)
        i = i + 1
    end
end
