-- The screen recorder on a Plus boot (see config.mk).  --record started the
-- recording before the first frame; the pin below lands before any frame
-- runs, so the boot is the same every time.

machine.rtc.time = 3000000000
assert(recorder.active, "--record did not start a recording")
assert(recorder.block, "headless records with block = true")
assert(recorder.threaded, "the default codes on the recorder thread")

-- ~25 s of emulated time: the ROM, the floppy question mark
scheduler.run(20000000)
local frames, units = recorder.frames, recorder.units
assert(frames == units, F"full rate: ${frames} frames for ${units} frame-units")
machine.screen.save(F"${vars.WORK_DIR}/end.png")
local paths = recorder.stop()
assert(#paths == 1, F"one file, got ${#paths}")
assert(not recorder.active, "still recording after stop")

local info = recorder.inspect(paths[1])
echo(F"boot.mkv: ${info.frames} frames, ${info.keyframes} keyframes, ${info.size} bytes " ..
     F"(video ${info.video_bytes}, audio ${info.audio_bytes}), crc ${info.crc32:08x}")
assert(info.frames == frames, F"the file holds ${info.frames} frames, recorded ${frames}")
assert(info.decode_errors == 0, "a packet did not decode")
assert(info.width == 512 and info.height == 342 and info.bits == 8, "512x342, 8 bpp palettised")
assert(info.frame_period_ns == 16625103, "one frame per frame-unit")
assert(info.audio and info.audio_rate == 22255 and info.audio_channels == 1, "22,255 Hz mono FLAC")
-- The sound covers the video exactly: round(frames x period x rate)
local want_audio = (frames * 16625103 * 22255 + 500000000) // 1000000000
assert(info.audio_frames == want_audio, F"${info.audio_frames} sample frames, want ${want_audio}")
-- The golden: the whole file, byte for byte
assert(info.crc32 == 0x2b1f4691, F"file CRC ${info.crc32:08x} is not the golden")

-- The last frame, decoded in tree, is the screen
recorder.extract(paths[1], F"${vars.WORK_DIR}/last.png")
machine.screen.match(F"${vars.WORK_DIR}/last.png")

-- The same boot, coded inline on the emulator thread: the same bytes
machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
machine.rtc.time = 3000000000
recorder.threaded = false
recorder.start(F"${vars.WORK_DIR}/boot-inline.mkv")
scheduler.run(20000000)
recorder.stop()
recorder.threaded = true
local diff = files.path_compare(paths[1], F"${vars.WORK_DIR}/boot-inline.mkv")
assert(diff == -1, F"inline and threaded recordings differ at byte ${diff}")

-- The boot beep lands at its own time: the frame-units before the first
-- sample pushed are held silence, and nothing more (suite-plus's plus-beep
-- window: 1M instructions, 15112 frames)
machine.boot{model = "plus", ram = 1024, rom = vars.ROM}
local beep = F"${vars.WORK_DIR}/beep.mkv"
recorder.start(beep)
machine.sound.capture.start()
while machine.sound.capture.frames == 0 do
    scheduler.run(1000)
end
local silent_units = recorder.units -- completed before the frame-unit of the first push
scheduler.run(1000000 - scheduler.instr_count)
local wav = F"${vars.WORK_DIR}/beep.wav"
machine.sound.capture.stop(wav)
assert(machine.sound.capture.frames == 15112, F"the beep window is ${machine.sound.capture.frames} frames")
local volume = machine.sound.volume
recorder.stop()
local at = recorder.find(beep, wav, volume)
local want = (silent_units * 16625103 * 22255 + 500000000) // 1000000000
echo(F"beep: first push after ${silent_units} silent frame-units; found at sample ${at}, want ${want} (volume ${volume})")
assert(at == want, F"the beep is at sample ${at}, its time is sample ${want}")

quit()
