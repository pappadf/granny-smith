-- Converted from test.script by scripts/script-to-lua.py
-- Restart-video conformance -- see config.mk.

local function cold(model, rom, budget)
    machine.boot{model = model, ram = 8192, rom = F"../../data/roms/${rom}"}
    scheduler.run(budget)
    local before = machine.screen.checksum()
    machine.restart()
    local after = gs.try(function() return machine.screen.checksum() end, nil)
    assert(after ~= before, F"${model}: machine.restart still shows the pre-restart frame")
    echo(F"restart-video-cold ${model}: ok")
end

cold("iicx", "iix-iicx-se30-97221136.rom", 30000000)
cold("iifx", "iifx-4147dd77.rom", 40000000)
cold("q700", "q700-q900-420dbff3.rom", 60000000)
cold("q840av", "q840av-q660av-5bf10fd1.rom", 300000000)

quit()
