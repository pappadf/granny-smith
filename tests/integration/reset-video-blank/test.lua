-- Converted from test.script by scripts/script-to-lua.py
-- Reset-video conformance -- see config.mk.

local function warm(model, rom, budget, redraw)
    machine.boot{model = model, ram = 8192, rom = F"../../data/roms/${rom}"}
    scheduler.run(budget)
    local before = machine.screen.checksum()
    machine.reset()
    -- The first frame after /RESET is the blanked raster, not the old picture.
    local blank = machine.screen.checksum()
    assert(blank ~= before, F"${model}: machine.reset still shows the pre-reset frame")
    -- ...and the ROM's own video set-up brings the picture back.
    scheduler.run(redraw)
    local later = machine.screen.checksum()
    assert(later ~= blank, F"${model}: the display stayed blanked after the ROM reprogrammed it")
    echo(F"reset-video-blank ${model}: ok")
end

warm("iicx", "iix-iicx-se30-97221136.rom", 30000000, 20000000)
warm("iifx", "iifx-4147dd77.rom", 40000000, 20000000)
warm("q700", "q700-q900-420dbff3.rom", 60000000, 40000000)
-- The 840AV's ROM redraws after ~99 M instructions: its start-up waits on Cuda,
-- which answers at the real part's ~65 us per byte.
warm("q840av", "q840av-q660av-5bf10fd1.rom", 300000000, 130000000)

quit()
