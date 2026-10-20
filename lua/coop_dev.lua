-------------------------------------------------------------------------------
--  coop_dev.lua
--
--  Dev harness for MDK2 HD co-op. Appended to %LOCALAPPDATA%\MDK2HD\settings.lua
--  by tools/inject.sh; runs after mdk2.lua so every game function is defined.
--  Lua 3.1: no 'local function', closures need %upvalues.
-------------------------------------------------------------------------------

coop = {
    offset = 3,         -- puppet sits this far from the player on x until the peer is heard
    mirror = 0,         -- height above the peer's position (F8), for loopback testing
}

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


-- bridge functions come from dinput8.dll; without it the puppet just follows locally --
if (coop_init) then
    coop_init()
end

-- each frame: send our position, put the puppet where the peer says its player is --
function coop.PuppetOnUpdate(gob)
    local p = mdkGetPlayerGob()
    if (not p or p == gob) then return end
    local x, y, z
    if (coop_send) then
        coop_send(p)
        x, y, z = coop_recv()
    end
    if (x) then
        mdkGobSetPositionXYZ(gob, x, y, z + coop.mirror)
        coop_face(gob)
    else
        mdkGobSetPositionXYZ(gob, p.position.x + coop.offset, p.position.y, p.position.z)
    end
end

-- F6: spawn player 2 puppet next to the player --
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

-- F11: log the player's and puppet's active animation states (bridge.log) --
comfuncs.fkey[COM_TOGL4] = function()
    if (coop_animdump) then
        coop_animdump(mdkGetPlayerGob())
        if (coop.p2) then coop_animdump(coop.p2) end
    end
end
