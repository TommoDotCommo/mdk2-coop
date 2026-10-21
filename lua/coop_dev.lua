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

-- the client switches its spawners off (the host's make the real enemies), but some make
-- items (health, ammo respawns: mdk2.lua's item spawners), which the client needs too --
coop.setspawned_orig = mdkSpawnerSetSpawnedObject
function mdkSpawnerSetSpawnedObject(spawner, objtype, a, b, c, d, e, f, g)
    if (objtype and objtype >= 300 and objtype <= 399) then coop_keep_spawner(spawner) end
    return coop.setspawned_orig(spawner, objtype, a, b, c, d, e, f, g)
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

-- Modern controls ([controls] modern, Options > Co-op): Space jumps (and works Max's jetpack
-- and Kurt's parachute, which share the jump button), right mouse toggles Kurt's sniper mode.
-- The game's defaults are the other way round (right mouse 1001 = jump/jet/chute, Space 57 =
-- sniper toggle). Applied after settings.lua, so they win over the saved bindings; changing
-- controls in the options menu saves them. --
function coop.ApplyControls(modern)
    local jump, snipe = 1001, 57
    if (modern) then jump, snipe = 57, 1001 end
    omBindCommandI(COM_JUMP, jump)
    omBindCommandI(COM_JET, jump)
    omBindCommandI(COM_CHUTE, jump)
    omBindCommandI(COM_SMTOGGLE, snipe)
end
coop.ApplyControls(coop_get("controls", "modern", "1") == "1")

-- The game's quicksave (F6) and quickload (F7) are off in co-op: a quickload restores this
-- game's own save, which the partner cannot follow (it showed up as level(l, -1) and the
-- client crashed loading its own save); F6 / F7 are the checkpoint keys instead --
omMakeCommand(COM_SAVEINSTANT, "", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_LOADINSTANT, "", CON_BUTTON_FALL, 0, 0)

-- Developer keys ([controls] devkeys, Options > Co-op > Gameplay; off for players, on in
-- the test installs): F1 hit / spawn, F3 lava, F4 teleport, F5 Doganboy, F6 / F7 / F8
-- checkpoints and levels for everyone, F9 BioWare debug menu, F10 test error, F11 anim log.
-- F2 (marker in every log, and a screenshot) stays on: it is how players report a problem. --
coop.devkeys = {
    { COM_SHOWBARS, "F1" }, { COM_CLEARCONSOLE, "F3" }, { COM_TOGLG, "F4" }, { COM_NEXTSECTION, "F5" },
    { COM_TOGL1, "F6" }, { COM_TOGL2, "F7" }, { COM_TOGL3, "F8" }, { COM_DEBUGMENUTOG, "F9" },
    { COM_PRINTCOL, "F10" }, { COM_TOGL4, "F11" },
}

function coop.ApplyDevKeys(on)
    local i, k = next(coop.devkeys, nil)
    while (i) do
        if (on) then omMakeCommand(k[1], k[2], CON_BUTTON_FALL, 0, 0)
        else omMakeCommand(k[1], "", CON_BUTTON_FALL, 0, 0) end
        i, k = next(coop.devkeys, i)
    end
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
    local gob = coop.FindGob(name)
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
            coop.MakeSave(extra2, netid, dtype, damage)
        elseif (kind == EV_SPAWN) then
            coop.Spawn(name, netid, objtype, x, y, z, damage)
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
    elseif (coop.level == l and coop.cp == cp and same ~= 1) then
        coop_log(format("host started level %d checkpoint %d: already here", l, cp))
    else
        coop_log(format("host started level %d checkpoint %d: following", l, cp))
        coop.following = l * 1000 + cp
        mdkNewGame(l, cp)
    end
end

-- every frame, called by the bridge after the scene update (menus and title included):
-- send our player, apply the others' events, put each puppet where its player is --
function coop.Tick()
    -- host autostart (tools/play.sh <level> <cp>): once the title screen has been up a while
    -- and coop.autostart_players others have joined (or a minute has gone by), load the start
    -- checkpoint; retried until a level is running --
    if (coop.autostart and coop.level == 0) then
        coop.ticks = (coop.ticks or 0) + 1
        local joined = coop_players()
        if (coop.ticks >= (coop.nextstart or 600) and
            (joined >= (coop.autostart_players or 0) or coop.ticks >= 3600)) then
            coop.nextstart = coop.ticks + 600
            coop_log(format("autostart: level %d checkpoint %d, %d others here", coop.autostart[1],
                            coop.autostart[2], joined))
            mdkNewGame(coop.autostart[1], coop.autostart[2])
        end
    elseif (coop.autostart and coop.level and coop.level >= 1) then
        coop.autostart = nil
    end
    local p = mdkGetPlayerGob()
    if (coop.PuppetSlot(p)) then p = nil end
    coop_send(p)
    coop_recv()
    coop.HandleEvents()
    coop.ReviveTick(p)
    coop.WaitTick()
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
function coop.HudTick()
    local show = 0
    if (coop.hud_on and coop.level and coop.level >= 1 and not coop.waiting and
        not (gamecontrol and gamecontrol.showmenu == 1)) then
        show = 1
    end
    coop_hud(show, mdkGetCameraGob())
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
        end
        return
    end
    local x, y, z = coop_peer(s)
    if (not x) then return end
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
function coop.SpawnPuppet(p, s)
    local ptype = mdkGetGobType(p)
    local peer = coop_peer_type(s)
    if (peer and peer ~= ptype) then
        coop_log(format("player %d plays type %d, we play %d: puppet uses ours", s + 1, peer, ptype))
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
    -- checkpoint -1 is a reload from the autosave (after a death): tell the client which
    -- checkpoint that is instead; it cannot load our save --
    if (cp < 0) then cp = mdkGetCurrentCheckPoint() end
    if (cp < 0) then announce = 0 end
    coop.following = nil
    coop.puppets = {}
    coop.level = l
    coop.cp = cp
    coop_level(l, cp, announce, save)
    -- host: how far the session has got (not when a save brought us back here) --
    if (announce == 1 and save == 0) then coop_progress(l, cp) end
    -- a client that loaded a save has a world from back then: the host sends its own again --
    if (save > 0 and coop_role() ~= "host") then coop_ready() end
    coop.ScheduleSave(10)
end

-- ---------------------------------------------------------------------------------------
-- Co-op saves (bridge/saves.c). The host calls one every coop.save_interval seconds and a
-- few seconds after each checkpoint; every game in the level makes its own full save under
-- the same number. Load Game on the host's title screen offers "Resume co-op".
-- ---------------------------------------------------------------------------------------

coop.save_interval = tonumber(coop_get("save", "interval", "120")) or 120

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
        coop_progress(coop.level, cp)
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
    coop_save_announce(no, coop.level, coop.cp)
    coop.MakeSave(coop_id(), no, coop.level, coop.cp)
end

-- make our copy of save no of host hostid's session; recorded a second later, once the game
-- has written it --
function coop.MakeSave(hostid, no, l, cp)
    if (not CanSaveGameNow()) then
        coop_log(format("co-op save %d skipped here: the game cannot save right now", no))
        return
    end
    local file = coop_save_file(no)
    mdkStartInstantSave(file, format("Co-op %d-%d", l, cp))
    coop.pending_save = { host = hostid, no = no, level = l, cp = cp, file = file, at = coop_time() + 1 }
end

function coop.PendingSaveTick()
    local ps = coop.pending_save
    if (not ps or coop_time() < ps.at) then return end
    coop.pending_save = nil
    if (mdkSaveFileExists(ps.file) == 1) then
        coop_saved(ps.host, ps.no, ps.level, ps.cp, ps.file)
    else
        coop_log(format("co-op save %d: %s was not written", ps.no, ps.file))
    end
end

-- host: load co-op save `no` (level l, checkpoint cp) from file; level() then asks every client
-- to load its copy --
function coop.Resume(file, no, l, cp)
    coop_log(format("resuming co-op save %d (level %d checkpoint %d)", no, l, cp))
    coop.loading = { no = no, level = l, cp = cp }
    mdkStartInstantLoad(file)
end

-- host: where to resume: the newest co-op save, unless the session got further since (a level
-- start or checkpoint after it, e.g. where the game cannot save): then that checkpoint.
-- Returns { file, no, level, cp } for a save, { level, cp } for a checkpoint, or nil --
function coop.ResumePoint()
    local file, no, l, cp = coop_save_find(coop_id(), 0, 0)
    if (file and mdkSaveFileExists(file) ~= 1) then file = nil end
    local pl, pcp, pafter = coop_progress_get()
    if (pl and (not file or no <= pafter)) then
        return { level = pl, cp = pcp }
    end
    if (file) then return { file = file, no = no, level = l, cp = cp } end
    return nil
end

function coop.ResumeAt(rp)
    if (rp.file) then
        coop.Resume(rp.file, rp.no, rp.level, rp.cp)
    else
        coop_log(format("resuming co-op at level %d checkpoint %d (no save since)", rp.level, rp.cp))
        mdkNewGame(rp.level, rp.cp)
    end
end

function coop.ResumeLatest()
    local rp = coop.ResumePoint()
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

function coop.AddResumeItem()
    local rp = coop.ResumePoint()
    if (not rp) then return end
    local item = mdkCreateMenuItem(menu.pcload, format("Resume co-op %d-%d", rp.level, rp.cp))
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

-- ---------------------------------------------------------------------------------------
-- Client: lost the host. Nothing in our world moves without the host, so the game waits
-- behind a message (input goes to the menu) until the host is back in a level; a host that
-- restarted asks us to load our copy of the save it resumes.
-- ---------------------------------------------------------------------------------------

menu.coopwait = mdkCreateMenu(0.5, 0.42, 0.05, 0.05, 0, JUSTIFY_CENTER, 1)
menu.coopwait.Cancel = function()
    if (coop_transport() == "steam" and coop_steam_lobby() == "") then coop.ShowPage("coopsession") end
end

function coop.WaitTick()
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

-- F1 (was debug.lua's bars): host spawns a Poopsy ahead, always named "coop_spawn" like a
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

coop.pages = {
    coop = function() coop.BuildMain() end,
    coopplayers = function() coop.BuildPlayers() end,
    coopsession = function() coop.BuildSession() end,
    coopgame = function() coop.BuildGame() end,
    coopdiff = function() coop.BuildDifficulty() end,
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
        local rp = coop.ResumePoint()
        if (rp) then
            item = mdkCreateMenuItem(m, format("Resume co-op %d-%d", rp.level, rp.cp))
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
coop.presets = {
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
    local normal = coop.presets[2][key]
    return tonumber(coop_get("game", key, normal)) or normal
end

-- index (1-based) of the preset the settings match, or nil --
function coop.PresetIndex()
    local i = 1
    while (coop.presets[i]) do
        local match = 1
        local k = 1
        while (coop.diffkeys[k]) do
            local key = coop.diffkeys[k]
            if (coop.DiffGet(key) ~= coop.presets[i][key]) then match = nil end
            k = k + 1
        end
        if (match) then return i end
        i = i + 1
    end
    return nil
end

function coop.PresetName()
    local i = coop.PresetIndex()
    if (i) then return coop.presets[i].name end
    return "Custom"
end

function coop.SetPreset(i)
    local k = 1
    while (coop.diffkeys[k]) do
        coop_set("game", coop.diffkeys[k], coop.presets[i][coop.diffkeys[k]])
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
    mdkMenuItemSetWidgitValue(coop.diffpreset, (coop.PresetIndex() or 5) - 1)
end

-- difficulty page: the presets, then each setting on its own --
function coop.BuildDifficulty()
    local m = menu.coopdiff
    mdkClearMenu(m)
    coop.Heading(m, "Difficulty")

    local item = mdkCreateMenuItem(m, "Preset")
    mdkMenuItemAddComboBox(item)
    local i = 1
    while (coop.presets[i]) do
        mdkMenuItemAddComboString(item, coop.presets[i].name)
        i = i + 1
    end
    mdkMenuItemAddComboString(item, "Custom")
    mdkMenuItemSetWidgitValue(item, (coop.PresetIndex() or i) - 1)
    coop.diffpreset = item
    item.Inc = function(self)
        local n = mdkMenuItemGetWidgitValue(self) + 1
        if (coop.presets[n]) then
            coop.SetPreset(n)
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

    item = mdkCreateMenuItem(m, "Difficulty: " .. coop.PresetName())
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

    item = mdkCreateMenuItem(m, "Modern controls")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("controls", "modern", "1")) or 1)
    item.Select = function(self, n)
        coop_set("controls", "modern", n)
        coop.ApplyControls(n == 1)
    end

    item = mdkCreateMenuItem(m, "Players on screen")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("hud", "show", "1")) or 1)
    item.Select = function(self, n)
        coop_set("hud", "show", n)
        coop.hud_on = (n == 1)
    end

    item = mdkCreateMenuItem(m, "Developer keys")
    mdkMenuItemAddCheckBox(item, tonumber(coop_get("controls", "devkeys", "0")) or 0)
    item.Select = function(self, n)
        coop_set("controls", "devkeys", n)
        coop.ApplyDevKeys(n == 1)
    end

    coop.BackItem(m, "coop")
end

menu.coopgame.Cancel = function() coop.ShowPage("coop") end

coop.ApplyDevKeys(coop_get("controls", "devkeys", "0") == "1")
