-- machine.sound.frames is in sample frames on every engine (see config.mk).
-- Each check boots a model, records from power-on, runs past the startup
-- sound and compares the engine's own count with the capture sink's.

local function check_frames(tag, model, rom, budget)
    machine.boot{model = model, rom = rom}
    assert(machine.sound.frames == 0, F"${tag}: frames ${machine.sound.frames} before the machine ran")
    machine.sound.capture.start()
    scheduler.run(budget)
    machine.sound.capture.stop()
    echo(F"${tag}: sound frames=${machine.sound.frames} captured=${machine.sound.capture.frames}")
    assert(machine.sound.capture.frames > 0, F"${tag}: the startup sound pushed nothing")
    assert(machine.sound.frames == machine.sound.capture.frames, F"${tag}: machine.sound.frames ${machine.sound.frames} is not the ${machine.sound.capture.frames} frames rendered")
end

check_frames("plus", "plus", "../../data/roms/plus-v3-4d1f8172.rom", 20000000)
-- The PDM AWACS engine renders half-buffers of up to 2048 frames each
check_frames("pm6100", "pm6100", "../../data/roms/pm6100-pm7100-pm8100-9feb69b3.rom", 150000000)
-- The AV Singer engine: the chime plays out-enabled frames ~10-67 (~0.6 s)
check_frames("q840av", "q840av", "../../data/roms/q840av-q660av-5bf10fd1.rom", 200000000)

quit()
