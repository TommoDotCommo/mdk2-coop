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
    mirror = 0,         -- height above the peer's position (F8), for loopback testing
    enemies = 0,        -- Doganboys spawned with F5
}

-- event kinds, as in bridge.h --
EV_HIT = 1
EV_APPLY = 2
EV_HURT = 3
EV_SPAWN = 4

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
    if (gob and type(gob) ~= "number" and type(gob) ~= "string") then
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
        if (kind == EV_SPAWN) then
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

-- each frame: send our position, put the puppet where the peer says its player is --
function coop.PuppetOnUpdate(gob)
    local p = mdkGetPlayerGob()
    if (not p or p == gob) then return end
    local room = mdkGetCurrentRoom()
    if (room ~= coop.room) then
        coop_log(format("room %s at (%.1f, %.1f, %.1f)", tostring(room), p.position.x, p.position.y, p.position.z))
        coop.room = room
    end
    coop_send(p)
    local x, y, z = coop_recv()
    coop.HandleEvents()
    if (x) then
        mdkGobSetPositionXYZ(gob, x, y, z + coop.mirror)
        coop_face(gob)
    else
        mdkGobSetPositionXYZ(gob, p.position.x + coop.offset, p.position.y, p.position.z)
    end
end

-- F6: spawn player 2 puppet next to the player (starts sync) --
comfuncs.fkey[COM_TOGL1] = function()
    local p = mdkGetPlayerGob()
    if (not p) then return end
    coop.p2 = mdkCreateObjectLua("coop_p2", OBJ_KURT, mdkGetScene(), nil, nil, 0,0,0, 1,0,0,0, nil,0,0,0,0)
    mdkGobSetPositionXYZ(coop.p2, p.position.x + coop.offset, p.position.y, p.position.z)
    mdkSetLuaEvent(coop.p2, "OnUpdate", coop.PuppetOnUpdate)
end

-- F7: jump to level 1 checkpoint 2 (on foot, skips the skydive) --
comfuncs.fkey[COM_TOGL2] = function()
    mdkNewGame(1, 2)
end

-- F8: toggle drawing the puppet 3 units above the peer's position (z is up); with coop.ini
-- looped back to our own port the puppet then mirrors the local player, always on camera --
comfuncs.fkey[COM_TOGL3] = function()
    if (coop.mirror == 0) then coop.mirror = 3 else coop.mirror = 0 end
end

-- F11: log anim states, every gob and the OBJ_* numbers (bridge.log) --
comfuncs.fkey[COM_TOGL4] = function()
    coop_animdump(mdkGetPlayerGob())
    if (coop.p2) then coop_animdump(coop.p2) end
    coop_listgobs()
    if (not coop.typeslogged) then coop.LogObjTypes() coop.typeslogged = 1 end
end

-- F4: teleport the player onto coop.tpname (level 1's decoy item by default: next room, and
-- standing on it picks it up), for room and pickup tests --
coop.tpname = "l1_r3dummy"
comfuncs.fkey[COM_TOGLG] = function()
    local g = getglobal(coop.tpname)
    if (g) then
        mdkGobSetPositionXYZ(mdkGetPlayerGob(), g.position.x, g.position.y, g.position.z)
    else
        mdkGobSetPositionXYZ(mdkGetPlayerGob(), 133, 46, -40)
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
