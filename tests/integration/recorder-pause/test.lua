-- The recorder on the guest's clock (see config.mk).

-- Busy-wait `ms` of host time with the machine stopped: nothing may record
local function host_wait(ms)
    local t0 = scheduler.host_wall_ns
    while scheduler.host_wall_ns - t0 < ms * 1000000 do
    end
end

local rates = {{"full", 1}, {"half", 2}, {"quarter", 4}}
for _, r in ipairs(rates) do
    local name, div = r[1], r[2]
    -- A fresh machine stands at a frame-unit boundary at cycle 0
    machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
    machine.rtc.time = 3000000000
    assert(scheduler.cycles == 0, "a fresh machine has run no cycles")
    -- One frame-unit's cycles, as scheduler_run_frame sizes it
    local frame_instr = math.floor((1.0 / 60.15) * scheduler.frequency / scheduler.cpi)
    local frame_cycles = frame_instr * scheduler.cpi
    recorder.rate = name
    local path = F"${vars.WORK_DIR}/pause-${name}.mkv"
    recorder.start(path)
    -- Runs that end mid-frame, with the machine stopped and the host waiting
    local budgets = {123457, 2000003, 77, 1000000, 333333, 4999999, 1, 650001}
    for i, b in ipairs(budgets) do
        scheduler.run(b)
        local before = recorder.frames
        host_wait(30)
        assert(recorder.frames == before, F"${name}: frames advanced while stopped")
        local units = recorder.units
        assert(recorder.frames == units // div, F"${name}: ${recorder.frames} frames for ${units} frame-units")
        -- The frame-units are the completed ones on the guest's clock
        local whole = scheduler.cycles // frame_cycles
        assert(math.abs(units - whole) <= 1, F"${name}: ${units} frame-units, the cycles say ${whole}")
    end
    local frames = recorder.frames
    local seconds = recorder.seconds
    local paths = recorder.stop()
    local info = recorder.inspect(paths[1])
    echo(F"${name}: ${recorder.units} frame-units, ${frames} frames, ${seconds} s, ${info.size} bytes")
    assert(info.frames == frames, F"${name}: the file holds ${info.frames} frames, recorded ${frames}")
    assert(info.frame_period_ns == 16625103 * div, F"${name}: frame period ${info.frame_period_ns}")
    assert(math.abs(seconds - frames * div * 0.016625103) < 1e-6, F"${name}: seconds ${seconds}")
end
recorder.rate = "full"

quit()
