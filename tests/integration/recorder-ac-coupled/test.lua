-- The recorder's AC-coupled sound (see config.mk).

-- The Plus's main sound buffer: 370 words at the top of its 1 MB less $300;
-- each word's high byte is the PWM sample (offset binary, 128 = 0)
local SOUND_BUF = 0x100000 - 0x300

-- Fill the buffer with one sample byte: a constant level of (b - 128) << 8
local function level(b)
    for i = 0, 369 do
        machine.memory.poke.w(SOUND_BUF + 2 * i, b << 8)
    end
end

-- One session: the buffer at 0 when the recording starts, then left at
-- -23296 ($25); suspended once the level has decayed, resumed, stopped.
-- The two files' inspections.
local function session(tag, threaded, coupled)
    machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
    machine.rtc.time = 3000000000
    scheduler.run(20000000) -- the floppy question mark; the boot beep is over
    machine.sound.mute(false) -- the PWM output on, whatever the ROM left
    level(0x80)
    recorder.threaded = threaded
    recorder.ac_coupled = coupled
    local base = F"${vars.WORK_DIR}/${tag}.mkv"
    recorder.start(base)
    assert(not gs.ok(function() recorder.ac_coupled = not coupled end), "ac_coupled is fixed while recording")
    scheduler.run(1000000)
    level(0x25) -- the guest leaves the buffer far from 128
    scheduler.run(3000000)
    recorder.suspend()
    scheduler.run(1000000) -- the machine runs on, nothing is recorded
    recorder.resume()
    scheduler.run(3000000)
    local volume = machine.sound.volume
    local paths = recorder.stop()
    assert(#paths == 2, F"${tag}: two segments, got ${#paths}")
    local a, b = recorder.inspect(paths[1]), recorder.inspect(paths[2])
    for _, i in ipairs({a, b}) do
        assert(i.audio and i.decode_errors == 0, F"${tag}: the sound decodes")
        echo(F"${tag}: ${i.audio_frames} sample frames, first ${i.audio_first}, last ${i.audio_last}, " ..
             F"min ${i.audio_min}, max ${i.audio_max}, mean ${i.audio_mean} (volume ${volume})")
    end
    return a, b, volume
end

assert(recorder.ac_coupled, "recordings are AC-coupled by default")

local a, b, volume = session("threaded", true, true)
local dc = -23296 * volume // 7 -- the raw level, through the guest's volume
assert(volume > 0, "the Plus plays at a volume")
-- No click at the start (the buffer sat at 0) and none where the level moves
-- that the real machine's output capacitor would not make: a step that decays
assert(a.audio_first == 0, F"the recording starts at ${a.audio_first}, not 0")
assert(a.audio_min <= dc + 2 and a.audio_min >= dc - 2, F"the step reaches ${a.audio_min}, the level ${dc}")
assert(a.audio_max == 0, F"a step down never rises above 0 (max ${a.audio_max})")
-- No DC: the level has decayed to silence by the suspend, and the mean is
-- the step's decay alone (A x tau over the segment), a few percent of A
assert(a.audio_last == 0, F"the first segment ends at ${a.audio_last}, not 0")
assert(math.abs(a.audio_mean) < math.abs(dc) // 10, F"the first segment's mean ${a.audio_mean} is a DC")
-- The join: the second segment carries on where the first left off -- no step
assert(b.audio_first == a.audio_last, F"the join steps from ${a.audio_last} to ${b.audio_first}")
assert(b.audio_min == 0 and b.audio_max == 0, "the second segment is silent: the level is DC")

-- Inline: the same bytes
local ia, ib = session("inline", false, true)
assert(ia.crc32 == a.crc32 and ib.crc32 == b.crc32, "threaded and inline files are the same bytes")

-- Raw: the DAC's level is recorded as it is, DC and all
local ra, rb = session("raw", true, false)
assert(ra.audio_first == 0 and ra.audio_last == dc, F"raw: the level ${ra.audio_last}, want ${dc}")
assert(rb.audio_min == dc and rb.audio_max == dc and rb.audio_mean == dc, "raw: the second segment is the DC level")

recorder.ac_coupled = true
recorder.threaded = true
quit()
