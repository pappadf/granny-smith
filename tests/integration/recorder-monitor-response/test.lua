-- A recording is the picture on the monitor (see config.mk).

machine.boot{model = "iicx", ram = 8192, video_sense = 0, rom = vars.ROM}
machine.floppy.drive[0].insert("../../data/systems/System_7_0_1.image")
machine.rtc.pram.validate()
machine.rtc.pram.poke(0x46, gs.bytes(0x002783a7a7000000, 8))
recorder.start(F"${vars.WORK_DIR}/kong.mkv")
-- Into the Welcome splash at 8 bpp (iicx-video-modes' budget)
scheduler.run(49000000)
assert(machine.screen.width == 1152 and machine.screen.height == 870, "the 21\" mode")
assert(machine.screen.depth == 8, "8 bpp")
local paths = recorder.stop()
assert(#paths == 1, "one file")
local info = recorder.inspect(paths[1])
echo(F"kong.mkv: ${info.frames} frames, ${info.keyframes} keyframes, ${info.size} bytes")
assert(info.decode_errors == 0, "every frame decodes")
local last = F"${vars.WORK_DIR}/last.png"
recorder.extract(paths[1], last)
-- Not the bus's bytes: Kong's pre-distortion is cancelled in the recording
assert(not machine.screen.matches(last), "the recording shows screen.save's tint: no response curve applied")
-- The bus's bytes through the monitor's curve, computed per pixel apart from the recorder
local monitor = F"${vars.WORK_DIR}/monitor.png"
recorder.snapshot(monitor)
local diff = files.path_compare(last, monitor)
assert(diff == -1, F"the decoded frame differs from screen.save through crt_response at byte ${diff}")

quit()
