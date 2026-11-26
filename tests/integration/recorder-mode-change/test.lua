-- The recorder across guest mode changes (see config.mk).

-- An indexed depth switch: one file
machine.boot{model = "iicx", ram = 8192, video_sense = 6, rom = vars.ROM}
machine.floppy.drive[0].insert("../../data/systems/System_7_0_1.image")
machine.rtc.pram.validate()
machine.rtc.pram.poke(0x46, gs.bytes(0x002783a6a6000000, 8))
assert(machine.screen.depth == 1, "the JMFB powers on at 1 bpp")
recorder.start(F"${vars.WORK_DIR}/depth.mkv")
scheduler.run(49000000)
assert(machine.screen.depth == 8, F"the driver did not reach 8 bpp (${machine.screen.depth})")
assert(recorder.segments == 1, F"a depth change made ${recorder.segments} files")
local frames = recorder.frames
local paths = recorder.stop()
assert(#paths == 1, "one file")
local info = recorder.inspect(paths[1])
echo(F"depth.mkv: ${info.frames} frames, ${info.keyframes} keyframes, ${info.size} bytes")
assert(info.frames == frames and info.decode_errors == 0, "the file holds every frame")
assert(info.width == 640 and info.height == 480 and info.bits == 8, "640x480 palettised")
assert(info.keyframes >= 2, "the depth change starts the picture afresh: a keyframe")
recorder.extract(paths[1], F"${vars.WORK_DIR}/depth-last.png")
machine.screen.match(F"${vars.WORK_DIR}/depth-last.png")

-- A mode larger than the canvas: the next segment
machine.boot{model = "q700", ram = 8192, rom = "../../data/roms/q700-q900-420dbff3.rom", video_sense = 0}
assert(machine.screen.width == 640, "the DAFB powers on at 640x480")
local base = F"${vars.WORK_DIR}/size.mkv"
recorder.start(base)
scheduler.run(60000000)
assert(machine.screen.width == 1152 and machine.screen.height == 870, "the driver set the two-page mode")
assert(recorder.segments == 2, F"${recorder.segments} files, want 2")
frames = recorder.frames
paths = recorder.stop()
assert(#paths == 2, F"stop returned ${#paths} paths")
assert(paths[1] == base, "the first file is the path given")
assert(paths[2] == F"${vars.WORK_DIR}/size-2.mkv", F"the second is name-2.mkv, not ${paths[2]}")
local a, b = recorder.inspect(paths[1]), recorder.inspect(paths[2])
echo(F"size.mkv: ${a.width}x${a.height}, ${a.frames} frames; size-2.mkv: ${b.width}x${b.height}, ${b.frames} frames")
assert(a.width == 640 and a.height == 480, "the first segment has the power-on mode")
assert(b.width == 1152 and b.height == 870, "the second has the larger mode")
assert(a.frames + b.frames == frames, F"${a.frames} + ${b.frames} frames, recorded ${frames}")
assert(a.decode_errors == 0 and b.decode_errors == 0, "both files decode")
recorder.extract(paths[2], F"${vars.WORK_DIR}/size-last.png")
machine.screen.match(F"${vars.WORK_DIR}/size-last.png")

quit()
