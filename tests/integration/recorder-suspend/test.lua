-- Suspending and resuming a recording (see config.mk).

-- The sample frames that cover `frames` full-rate frames of the Plus's 22,255 Hz stream
local function audio_for(frames)
    return (frames * 16625103 * 22255 + 500000000) // 1000000000
end

-- One session: record, suspend, run on, resume, record, stop.  The files' CRCs.
local function session(tag, threaded)
    machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
    machine.rtc.time = 3000000000
    recorder.threaded = threaded
    local base = F"${vars.WORK_DIR}/${tag}.mkv"
    recorder.start(base)
    scheduler.run(4000000)
    local frames1 = recorder.frames
    assert(frames1 > 0, F"${tag}: frames before the suspend")

    recorder.suspend()
    assert(recorder.active and recorder.suspended, F"${tag}: suspended, still recording")
    recorder.suspend() -- a second suspend changes nothing
    -- The first file is whole now, before anything else happens
    local a = recorder.inspect(base)
    echo(F"${tag}: ${base}: ${a.frames} frames, ${a.keyframes} keyframes, ${a.size} bytes")
    assert(not a.truncated and a.duration_ms > 0, F"${tag}: the suspended file is finalised")
    assert(a.frames == frames1, F"${tag}: ${a.frames} frames in the file, ${frames1} recorded")
    assert(a.decode_errors == 0, F"${tag}: the suspended file decodes")
    assert(a.audio_frames == audio_for(a.frames), F"${tag}: its sound covers its frames")

    -- Suspended: the machine runs on, nothing is recorded
    local units = recorder.units
    scheduler.run(2000000)
    assert(recorder.frames == frames1 and recorder.units == units, F"${tag}: nothing recorded while suspended")
    assert(recorder.segments == 1, F"${tag}: no new file while suspended")
    -- A boot while suspended opens nothing either
    machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
    machine.rtc.time = 3000000000
    assert(recorder.active and recorder.suspended and recorder.segments == 1, F"${tag}: a boot keeps it suspended")
    scheduler.run(1000000)

    local p2 = recorder.resume()
    assert(p2 == F"${vars.WORK_DIR}/${tag}-2.mkv", F"${tag}: resumed into ${p2}")
    assert(not recorder.suspended and recorder.segments == 2, F"${tag}: resumed into the second segment")
    assert(recorder.resume() == p2, F"${tag}: resume when not suspended changes nothing")
    scheduler.run(3000000)
    local frames = recorder.frames
    local paths = recorder.stop()
    assert(#paths == 2 and paths[1] == base and paths[2] == p2, F"${tag}: the two files")
    local b = recorder.inspect(p2)
    echo(F"${tag}: ${p2}: ${b.frames} frames, ${b.keyframes} keyframes, ${b.size} bytes")
    assert(a.frames + b.frames == frames, F"${tag}: ${a.frames} + ${b.frames} frames, recorded ${frames}")
    assert(b.keyframes >= 1 and b.decode_errors == 0 and not b.truncated, F"${tag}: the second file opens on a keyframe")
    assert(b.audio_frames == audio_for(b.frames), F"${tag}: the second file's sound covers its frames")
    return a.crc32, b.crc32
end

local ta, tb = session("threaded", true)
local ia, ib = session("inline", false)
assert(ta == ia and tb == ib, "threaded and inline files are the same bytes")
recorder.threaded = true

-- Stopping a suspended recording returns its one, already finalised, file
machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
local only = F"${vars.WORK_DIR}/stop-suspended.mkv"
recorder.start(only)
scheduler.run(1000000)
recorder.suspend()
local paths = recorder.stop()
assert(not recorder.active and #paths == 1 and paths[1] == only, "stop while suspended: the one file")
local c = recorder.inspect(only)
assert(c.frames > 0 and not c.truncated and c.duration_ms > 0, "a finalised file")

-- Neither verb works without a recording
assert(not gs.ok(function() return recorder.suspend() end), "suspend refuses when not recording")
assert(not gs.ok(function() return recorder.resume() end), "resume refuses when not recording")
quit()
