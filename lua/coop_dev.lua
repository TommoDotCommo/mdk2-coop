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

-- Controls: Space jumps (and works Max's jetpack and Kurt's parachute, which share the jump
-- button), right mouse toggles Kurt's sniper mode. The game's defaults are the other way round
-- (right mouse 1001 = jump/jet/chute, Space 57 = sniper toggle). Applied after settings.lua,
-- so they win over the saved bindings; changing controls in the options menu saves them. --
omBindCommandI(COM_JUMP, 57)
omBindCommandI(COM_JET, 57)
omBindCommandI(COM_CHUTE, 57)
omBindCommandI(COM_SMTOGGLE, 1001)

-- The game's quicksave (F6) and quickload (F7) are off in co-op: a quickload restores this
-- game's own save, which the partner cannot follow (it showed up as level(l, -1) and the
-- client crashed loading its own save); F6 / F7 are the checkpoint keys instead --
omMakeCommand(COM_SAVEINSTANT, "", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_LOADINSTANT, "", CON_BUTTON_FALL, 0, 0)

omMakeCommand(COM_TOGL1, "F6", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL2, "F7", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL3, "F8", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGL4, "F11", CON_BUTTON_FALL, 0, 0)
omMakeCommand(COM_TOGLG, "F4", CON_BUTTON_FALL, 0, 0)

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
    coop.WaitTick()
    coop.PendingSaveTick()
    if (coop_role() == "host") then coop.SaveTick() end
    if (not p or not coop.level or coop.level < 1) then return end
    local me = coop_slot()
    local _, slots = coop_players()
    local s = 0
    while (s < slots) do
        if (s ~= me) then coop.UpdatePuppet(p, s) end
        s = s + 1
    end
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
    x, y = coop.KeepApart(p, x, y)
    mdkGobSetPositionXYZ(gob, x, y, z + coop.mirror)
    coop_face(gob, s)
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

coop.save_interval = 120

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
menu.coopwait.Cancel = function() end      -- Esc does not close it

function coop.WaitTick()
    if (coop_role() == "host" or not coop.level or coop.level < 1) then
        if (coop.waiting) then coop.StopWaiting() end
        return
    end
    local silent, hlevel = coop_host_status()
    local why = nil
    if (silent > 2000) then
        why = "Connection to the host lost - reconnecting"
    elseif (hlevel < 1) then
        why = "The host left the level - waiting for the host"
    end
    if (why and why ~= coop.waiting) then
        coop_log(format("waiting: %s", why))
        mdkClearMenu(menu.coopwait)
        mdkCreateMenuItem(menu.coopwait, why)
        mdkCreateMenuItem(menu.coopwait, "Your game is kept and resumes by itself")
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
omMakeCommand(COM_PRINTCOL, "F10", CON_BUTTON_FALL, 0, 0)   -- a debug.lua command we unbound above
comfuncs.fkey[COM_PRINTCOL] = function()
    coop_log("raising a test error")
    coop.no_such_function()
end
