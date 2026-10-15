-- Converted from test.script by scripts/script-to-lua.py
-- Reset-level conformance -- see config.mk.
--
-- levels(model, ram_kb, rom, budget, ticks): `budget` instructions are enough
-- for a fresh machine of this model to reach the Mac OS start-up (Ticks
-- counting) when `ticks` is true; otherwise the row checks the vector and the
-- stores only (Open Firmware ROMs, a 601 whose start-up is far longer).

-- A store's snapshot, or none on a machine that does not have that store.
local function pram() return gs.try(function() return machine.rtc.pram.snapshot() end, nil) end
local function nvram() return gs.try(function() return machine.nvram.snapshot() end, nil) end
local function ticks() return debug.mac.globals.read("Ticks") end

-- Run `budget` instructions in tenths and report whether the machine really
-- rebooted: Ticks must fall below `before` at some point (the ROM re-ran and
-- cleared it) and be counting again at the end.  A machine that kept running
-- instead only ever reads more than `before`.  (Comparing the final count with
-- a fresh boot's is not enough: a warm start skips the RAM test and reaches
-- the OS sooner.)
local function rebooted(budget, before)
    local lowest = 0xFFFFFFFF
    local i = 0
    while i < 10 do
        scheduler.run(budget, "/", 10)
        local t = ticks()
        if t < lowest then lowest = t end
        i = i + 1
    end
    local t = ticks()
    return lowest < before and t >= 0x10 and t < 0x10000
end

local function levels(model, ram_kb, rom, budget, check_ticks)
    machine.boot{model = model, ram = ram_kb, rom = F"../../data/roms/${rom}"}
    local pc0 = machine.cpu.pc
    local pram_fresh = pram()
    local nv_fresh = nvram()
    scheduler.run(budget)
    local t_fresh = 0
    if check_ticks then
        t_fresh = ticks()
        assert(t_fresh >= 0x10 and t_fresh < 0x10000, F"${model}: a fresh machine did not reach the start-up in the budget")
    end

    -- --- level 2: machine.reset.  The CPU at its vector, the stores kept.
    local pram1 = pram()
    local nv1 = nvram()
    if model ~= "lisa" and model ~= "macxl" then
        local t_before = 0
        if check_ticks then t_before = ticks() end
        machine.reset()
        assert(machine.cpu.pc == pc0, F"${model}: machine.reset did not land on the reset vector")
        assert(pram() == pram1, F"${model}: I2: PRAM lost on machine.reset")
        assert(nvram() == nv1, F"${model}: I2: NVRAM lost on machine.reset")
        if check_ticks then
            assert(rebooted(budget, t_before), F"${model}: machine.reset did not reboot the machine")
        else
            scheduler.run(budget)
        end
    end

    -- --- level 3: machine.restart.  The same, cold; nothing is rebuilt -- the
    -- stores below are the same chips.
    local pram2 = pram()
    local nv2 = nvram()
    local t_before2 = 0
    if check_ticks then t_before2 = ticks() end
    machine.restart()
    assert(machine.cpu.pc == pc0, F"${model}: machine.restart did not land on the reset vector")
    assert(pram() == pram2, F"${model}: I2: PRAM lost on machine.restart")
    assert(nvram() == nv2, F"${model}: I2: NVRAM lost on machine.restart")
    if check_ticks then
        assert(rebooted(budget, t_before2), F"${model}: machine.restart did not reboot the machine")
    else
        scheduler.run(budget)
    end

    -- --- A new machine, from the same document, inherits no store (R1).
    machine.boot{model = model, ram = ram_kb, rom = F"../../data/roms/${rom}"}
    assert(machine.id == model, F"${model}: machine.boot built another model")
    assert(machine.cpu.pc == pc0, F"${model}: a new machine is not at its reset vector")
    assert(pram() == pram_fresh, F"${model}: R1: a new machine inherited PRAM")
    assert(nvram() == nv_fresh, F"${model}: R1: a new machine inherited NVRAM")
    echo(F"reset-levels ${model}: ok")
end

levels("plus", 4096, "plus-v3-4d1f8172.rom", 30000000, true)
levels("se30", 8192, "iix-iicx-se30-97221136.rom", 20000000, true)
levels("iicx", 8192, "iix-iicx-se30-97221136.rom", 20000000, true)
levels("iix", 8192, "iix-iicx-se30-97221136.rom", 20000000, true)
levels("iifx", 8192, "iifx-4147dd77.rom", 40000000, true)
levels("iici", 8192, "iici-368cadfe.rom", 35000000, true)
levels("iisi", 9216, "iisi-36b7fb6c.rom", 30000000, true)
levels("q700", 8192, "q700-q900-420dbff3.rom", 35000000, true)
levels("q900", 8192, "q700-q900-420dbff3.rom", 35000000, true)
levels("q950", 8192, "q950-3dc27823.rom", 35000000, true)
levels("q840av", 8192, "q840av-q660av-5bf10fd1.rom", 60000000, true)
levels("q660av", 8192, "q840av-q660av-5bf10fd1.rom", 50000000, true)
levels("pm6100", 16384, "pm6100-pm7100-pm8100-9feb69b3.rom", 2000000, false)
levels("pm7100", 16384, "pm6100-pm7100-pm8100-9feb69b3.rom", 250000000, true)
levels("pm8100", 16384, "pm6100-pm7100-pm8100-9feb69b3.rom", 2000000, false)
levels("pm7500", 32768, "pm7500-pm8500-pm9500-96cd923d.rom", 2000000, false)
levels("pm8500", 32768, "pm7500-pm8500-pm9500-96cd923d.rom", 2000000, false)
levels("pm9500", 32768, "pm7500-pm8500-pm9500-96cd923d.rom", 2000000, false)
levels("ans500", 32768, "ans500-ans700-962f6c13.rom", 2000000, false)
levels("ans700", 49152, "ans500-ans700-962f6c13.rom", 2000000, false)
levels("pmg3dt", 65536, "pmg3dt-pmg3mt-78f57389.rom", 2000000, false)
levels("pmg3mt", 65536, "pmg3dt-pmg3mt-78f57389.rom", 2000000, false)
levels("lisa", 2048, "lisa2-revh-098917b2.rom", 2000000, false)
levels("macxl", 2048, "macxl-3a-094c82f0.rom", 2000000, false)
quit()
