-- The recorder's sound against plus-musicworks' golden (see config.mk).
-- The navigation is plus-musicworks' own, step for step, so the guest runs
-- the same instructions and its capture window is the same music.wav.

-- Pinned before the first frame runs (--record started the recording).
machine.rtc.time = 3000000000
assert(recorder.active and recorder.audio, "--record records the sound")
assert(not recorder.ac_coupled, "--recorder=raw records the DAC's output")

-- Boot the 400K MFS floppy to the Finder desktop.
scheduler.run(72000000)
machine.screen.match("../plus-musicworks/finder.png")

-- Open "MusicWorks Disk" (icon at top right).
machine.adb.mouse.move(465, 45)
scheduler.run(2300000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(200000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(18000000)

-- Open the "Samples" folder inside the disk window.
machine.adb.mouse.move(228, 90)
scheduler.run(2300000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(200000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(18000000)
machine.screen.match("../plus-musicworks/samples.png")

-- Open "Brandenburg #3" (first icon in the Samples window) — launches
-- MusicWorks from the same floppy and loads the document.
machine.adb.mouse.move(103, 210)
scheduler.run(2300000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(200000)
machine.adb.mouse.click(true)
scheduler.run(200000)
machine.adb.mouse.click(false)
scheduler.run(72000000)
machine.screen.match("../plus-musicworks/musicworks-loaded.png")

-- Click PLAY. MusicWorks first spends ~4.5 s initializing its PWM buffers
-- (constant full-scale DC — silent through the AC-coupled speaker); the run
-- below skips past that so the golden is the music itself.
machine.adb.mouse.move(310, 33)
scheduler.run(2300000)
machine.adb.mouse.click(true)
scheduler.run(300000)
machine.adb.mouse.click(false)
scheduler.run(3500000)

-- The golden's window, as plus-musicworks captures it; where it starts
-- on the recording's clock
local start_units = recorder.units
machine.sound.capture.start()
scheduler.run(5900000)
machine.sound.capture.stop()
assert(machine.sound.capture.frames == 167674, "unexpected capture length")
machine.sound.match("../plus-musicworks/music.wav")
local volume = machine.sound.volume
local paths = recorder.stop()

local info = recorder.inspect(paths[1])
echo(F"musicworks.mkv: ${info.frames} frames, ${info.size} bytes (video ${info.video_bytes}, audio ${info.audio_bytes}), " ..
     F"${info.audio_frames} sample frames")
assert(info.decode_errors == 0, "every packet decodes")
local want_audio = (info.frames * 16625103 * 22255 + 500000000) // 1000000000
assert(info.audio_frames == want_audio, F"${info.audio_frames} sample frames for ${info.frames} frames, want ${want_audio}")
-- The music, sample for sample, at its time
assert(volume == 7, F"MusicWorks plays at volume ${volume}, not 7")
local at = recorder.find(paths[1], "../plus-musicworks/music.wav")
local want = (start_units * 16625103 * 22255 + 500000000) // 1000000000
echo(F"music.wav found at sample ${at}; the window starts at frame-unit ${start_units}, sample ${want}")
assert(at >= 0, "the golden music window is not in the recording")
-- Placed on emulated time, within the timeline's window (two frame-units)
assert(math.abs(at - want) <= 742, F"the music is at sample ${at}, its time is sample ${want}")

quit()
