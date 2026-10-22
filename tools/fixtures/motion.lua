-- Test-only, injected with command.lua. Lua 3.1; never installed in player packages.
-- Observe actual engine transforms on every frame so short jumps are not missed by polling.
if (not coop.qa_motion_original) then coop.qa_motion_original = coop.Tick end
function coop.QAMotionStart(tag, key, hold, duration)
    local p = mdkGetPlayerGob()
    if (not p or coop.qa_motion) then
        coop_log("qa-motion " .. tag .. " unavailable")
        return
    end
    coop.qa_motion = { tag = tag, started = coop_time(), duration = duration,
        level = coop.level, objtype = mdkGetGobType(p), x = p.position.x,
        y = p.position.y, z = p.position.z, planar = 0, rise = 0, fall = 0,
        dx = 0, dy = 0, frames = 0, bad = 0 }
    if (key >= 0 and not coop.qa_motion_no_input) then coop_press(key, hold) end
end
function coop.Tick()
    coop.qa_motion_original()
    local m = coop.qa_motion
    if (not m) then return end
    local p = mdkGetPlayerGob()
    if (not p or coop.level ~= m.level or coop_life(coop_slot()) ~= 0 or coop.waiting or
        (gamecontrol and gamecontrol.showmenu == 1)) then
        m.bad = 1
    elseif (p) then
        m.frames = m.frames + 1
        m.dx = p.position.x - m.x
        m.dy = p.position.y - m.y
        local planar = sqrt(m.dx * m.dx + m.dy * m.dy)
        local dz = p.position.z - m.z
        if (planar > m.planar) then m.planar = planar end
        if (dz > m.rise) then m.rise = dz end
        if (-dz > m.fall) then m.fall = -dz end
    end
    if (coop_time() - m.started >= m.duration) then
        coop_log(format("qa-motion %s type %d frames %d bad %d planar %.4f rise %.4f fall %.4f dx %.4f dy %.4f",
            m.tag, m.objtype, m.frames, m.bad, m.planar, m.rise, m.fall, m.dx, m.dy))
        coop.qa_motion = nil
    end
end
