-- Converted from test.script by scripts/script-to-lua.py
-- Default configurations boot (see config.mk).

local mac = dofile("../lib/mac.lua")

-- Boot `model` from `rom` with its default configuration and run up to `n`
-- instructions: the storage the default document names, a screen, pixels.
-- `n` is a ceiling: the row polls in tenths of it and stops at the first
-- drawn frame.
local function default_boots(model, rom, n)
    machine.boot{model = model, rom = F"../../data/roms/${rom}"}
    assert(machine.id == model, F"${model}: not built")
    local want = #catalog.default_config(model).storage
    assert(#machine.storage == want, F"${model}: ${#machine.storage} storage devices, the default document has ${want}")
    local step = n // 10
    local spent = 0
    while spent < n do
        scheduler.run(step)
        spent = spent + step
        -- (try(): a machine early in its bring-up may not have a screen yet.)
        local w = gs.try(function() return machine.screen.width end, 0)
        local sum = gs.try(function() return machine.screen.checksum() end, 0)
        if w > 0 and sum ~= 0 then break end
    end
    assert(machine.screen.width > 0, F"${model}: no screen")
    assert(machine.screen.checksum() ~= 0, F"${model}: nothing drawn after ${n} instructions")
    mac.report_perf(F"default-configs/${model}", spent)
end

default_boots("plus", "plus-v3-4d1f8172.rom", 20000000)
default_boots("se30", "iix-iicx-se30-97221136.rom", 40000000)
default_boots("iix", "iix-iicx-se30-97221136.rom", 40000000)
default_boots("iicx", "iix-iicx-se30-97221136.rom", 40000000)
default_boots("iifx", "iifx-4147dd77.rom", 60000000)
default_boots("iici", "iici-368cadfe.rom", 40000000)
default_boots("iisi", "iisi-36b7fb6c.rom", 40000000)
default_boots("q700", "q700-q900-420dbff3.rom", 120000000)
default_boots("q900", "q700-q900-420dbff3.rom", 120000000)
default_boots("q950", "q950-3dc27823.rom", 120000000)
default_boots("q840av", "q840av-q660av-5bf10fd1.rom", 200000000)
default_boots("q660av", "q840av-q660av-5bf10fd1.rom", 200000000)
default_boots("lisa", "lisa2-revh-098917b2.rom", 20000000)
default_boots("macxl", "macxl-3a-094c82f0.rom", 20000000)

-- The Lisa's memory boards: RAM starts at $100000 less the slot-2 board, as
-- the boot ROM's sizing reports it (MINMEM, $2A4) -- a 512 KB board below
-- $100000 for 512 KB to 1.5 MB, two 1 MB boards from 0 for 2 MB.
local function lisa_minmem(kb, want)
    machine.boot{model = "lisa", ram = kb, rom = "../../data/roms/lisa2-revh-098917b2.rom"}
    scheduler.run(30000000)
    assert(machine.memory.peek.l(0x2A4) == want, F"lisa ${kb} KB: MINMEM ${machine.memory.peek.l(0x2A4):08x}")
end
lisa_minmem(512, 0x80000)
lisa_minmem(1024, 0x80000)
lisa_minmem(1536, 0x80000)
lisa_minmem(2048, 0)
default_boots("pm6100", "pm6100-pm7100-pm8100-9feb69b3.rom", 400000000)
default_boots("pm7100", "pm6100-pm7100-pm8100-9feb69b3.rom", 400000000)
default_boots("pm8100", "pm6100-pm7100-pm8100-9feb69b3.rom", 400000000)
default_boots("pm7500", "pm7500-pm8500-pm9500-96cd923d.rom", 400000000)
default_boots("pm8500", "pm7500-pm8500-pm9500-96cd923d.rom", 400000000)
default_boots("pm9500", "pm7500-pm8500-pm9500-96cd923d.rom", 400000000)
default_boots("ans500", "ans500-ans700-962f6c13.rom", 1500000000)
default_boots("ans700", "ans500-ans700-962f6c13.rom", 1500000000)
default_boots("pmg3dt", "pmg3dt-pmg3mt-78f57389.rom", 900000000)
default_boots("pmg3mt", "pmg3dt-pmg3mt-78f57389.rom", 900000000)
quit()
