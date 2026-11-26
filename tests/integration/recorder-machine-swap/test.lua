-- A recording across a new machine (see config.mk).

machine.rtc.time = 3000000000
scheduler.run(3000000)
local cp = F"${vars.WORK_DIR}/swap.gscp"
checkpoint.save(cp)

local base = F"${vars.WORK_DIR}/swap.mkv"
recorder.start(base)
scheduler.run(3000000)
local units = recorder.units
-- A checkpoint load: the same machine shape, a cut in the same file
assert(checkpoint.load(cp), "checkpoint.load")
assert(recorder.active and recorder.segments == 1, "a fitting checkpoint load stays in the file")
assert(recorder.units == units, "the load itself completes no frame-units")
scheduler.run(3000000)
assert(recorder.units > units, "the clock carries on after the load")
-- A boot: always the next segment
machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
assert(recorder.active and recorder.segments == 2, F"machine.boot made ${recorder.segments} segments, want 2")
scheduler.run(3000000)
local frames = recorder.frames
assert(recorder.units == frames, "full rate: a frame per frame-unit throughout")
local paths = recorder.stop()
assert(#paths == 2 and paths[2] == F"${vars.WORK_DIR}/swap-2.mkv", "two files, the second name-2.mkv")
local a, b = recorder.inspect(paths[1]), recorder.inspect(paths[2])
echo(F"swap.mkv: ${a.frames} frames, ${a.keyframes} keyframes; swap-2.mkv: ${b.frames} frames, ${b.keyframes} keyframes")
assert(a.frames + b.frames == frames, F"${a.frames} + ${b.frames} frames, recorded ${frames}")
assert(a.keyframes == 2, "the cut is a keyframe")
assert(b.keyframes == 1, "the new segment opens on a keyframe")
assert(a.decode_errors == 0 and b.decode_errors == 0, "both decode")
-- Each file's sound covers its frames exactly
for _, i in ipairs({a, b}) do
    local want = (i.frames * 16625103 * 22255 + 500000000) // 1000000000
    assert(i.audio_frames == want, F"${i.audio_frames} sample frames for ${i.frames} frames, want ${want}")
end

-- Shutting the machine down mid-recording finalises the file
local last = F"${vars.WORK_DIR}/shutdown.mkv"
recorder.start(last)
scheduler.run(1000000)
machine.shutdown()
assert(not recorder.active, "the recording ended with the machine")
assert(recorder.path == last, F"recorder.path names the last file (${recorder.path})")
local c = recorder.inspect(last)
assert(c.frames > 0 and not c.truncated and c.duration_ms > 0, "a finalised file: sized, with a duration")

-- A run ends with a machine (a headless session with none exits non-zero)
machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
quit()
