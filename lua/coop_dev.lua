-------------------------------------------------------------------------------
--  coop_dev.lua
--
--  Dev harness for MDK2 HD co-op. Appended to %LOCALAPPDATA%\MDK2HD\settings.lua
--  by tools/inject.sh; runs after mdk2.lua so every game function is defined.
--  Lua 3.1: no 'local function', closures need %upvalues.
-------------------------------------------------------------------------------

-- BioWare debug menu (F9 in game), normally behind '$if features' --
dofile('debug')

omMakeCommand(COM_TOGL1, "F6", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL2, "F7", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL3, "F8", CON_BUTTON_FALL, 0, 0)

coop = {
    offset = 3,         -- puppet sits this far from the player on x until the peer is heard
}

-- bridge functions come from dinput8.dll; without it the puppet just follows locally --
if (coop_init) then
    coop_init()
end

-- each frame: send our position, put the puppet where the peer says its player is --
function coop.PuppetOnUpdate(gob)
    local p = mdkGetPlayerGob()
    if (not p or p == gob) then return end
    local x, y, z, yaw
    if (coop_send) then
        coop_send(p.position.x, p.position.y, p.position.z, 0)   -- TODO orientation: p.orientation is nil
        x, y, z, yaw = coop_recv()
    end
    if (x) then
        mdkGobSetPositionXYZ(gob, x, y, z)
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

-- F8: give the puppet the game's own Kurt AI instead of following --
comfuncs.fkey[COM_TOGL3] = function()
    if (coop.p2) then
        mdkSetLuaEvent(coop.p2, "OnUpdate", nil)
        mdkKurtInitAI(coop.p2)
        mdkKurtAIHeadToGob(coop.p2, mdkGetPlayerGob())
    end
end
