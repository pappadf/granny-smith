---
name: screen-recording
description: >
  Record the emulated machine's screen and sound to a lossless video file
  (Matroska, ZMBV + FLAC) from the headless emulator or the browser, and
  turn a recording into an MP4. Covers the `recorder` object and every
  option, the headless flags, inspecting and extracting from a recording,
  determinism, and the pitfalls.
triggers:
  - record the screen
  - screen recording
  - record a video
  - make a video of the emulator
  - capture video
  - record sound and video
  - recorder.start
  - --record
  - mkv
  - export mp4
  - convert recording to mp4
  - upload to youtube
  - frame rate of a recording
  - extract a frame from a recording
---

# Screen recording — Agent Skill

## Overview

The `recorder` root object records the guest's screen and sound to a
Matroska file: ZMBV video (lossless, at the guest's own depth) and FLAC
audio (lossless, 16-bit, at the guest's sample rate).

A recording follows the **guest's** clock, not the host's:

- One frame per VBL period of emulated time (16,625,103 ns; 60.15 Hz).
- A paused machine records nothing.
- Turbo records the same video, only sooner; playback is at normal speed.
- The frame rate is constant: 1203/20, 1203/40 or 1203/80 fps.

Design and internals: `docs/internals/core/record/record.md`. User
documentation: `docs/user/sound-and-video.md`, §5. File formats:
`docs/reference/formats/zmbv.md`, `flac.md`, `matroska.md`.

## Quick start (headless)

Build first: `make -f Makefile.headless`.

Record a whole run from the first frame:

```bash
build/headless/gs-headless rom=tests/data/roms/plus-v3-4d1f8172.rom model=plus ram=1024 --record=tmp/boot.mkv script=tmp/run.lua
```

`--record=PATH` is `recorder.start(PATH)` before any script runs; the file
is finalised by `recorder.stop()`, by `quit()`, or when the machine goes
away. The path must end in `.mkv`.

To choose options, start the recording from the script instead, because
the options are fixed once a recording runs:

```lua
recorder.rate = "half"            -- before start
recorder.audio = true
local path = recorder.start(F"${vars.WORK_DIR}/demo.mkv")
scheduler.run(5000000)
echo(F"frames ${recorder.frames}, ${recorder.seconds} s, ${recorder.bytes} bytes, dropped ${recorder.dropped}")
local paths = recorder.stop()     -- returns when every file is closed
quit()
```

In the headless shell or daemon the same members are typed as commands
(`recorder.start tmp/demo.mkv`, `recorder.stop`, `recorder.rate = "half"`);
see the `headless-debug` skill for the shell.

## The `recorder` object

### Methods

| Method | What it does |
|---|---|
| `start([path])` | Begin recording. Returns the path. `path` must end in `.mkv`. With no path the name is `<dir>/<model>-<YYYYmmdd-HHMMSS>.mkv`; the browser's dir is `/opfs/recordings`, headless has none and refuses an empty path. |
| `stop()` | Finalise. Blocks until the file is closed. Returns the list of files written (more than one when the recording went on in segments). |
| `suspend()` | Finalise the current file and record nothing until `resume`. The browser does this when the page is hidden. |
| `resume()` | Continue a suspended recording in the next segment; returns its path. |
| `inspect(path)` | Read a recording end to end; returns a map (see "Checking a recording"). |
| `extract(path, out, [index], [count])` | `out.png`: decode one frame (`index`, negative counts from the end; default the last). `out.wav`: the sound, or `count` sample frames from `index`. |
| `find(path, wav, [volume])` | The sample offset at which a WAV's samples appear exactly in the recording's sound (`volume` 0..7 is the guest volume the recording applied). |
| `snapshot(png)` | Save the screen as a recording shows it: `screen.save`'s pixels through the monitor's response curve. |

### Options (read/write)

`rate`, `audio`, `format` and `canvas` are refused while a recording is
active: set them before `start`.

| Attribute | Values | Default | Meaning |
|---|---|---|---|
| `rate` | `"full"`, `"half"`, `"quarter"` | `full` | Output frames per guest frame: 60.15, 30.075 or 15.0375 fps. No other rates exist. |
| `audio` | boolean | `true` | Record the sound too (when the machine has a sound stream). |
| `format` | `"auto"`, `"rgb"` | `auto` | `auto`: 8-bit palettised when the screen is indexed, else direct colour. `rgb`: 32 bpp throughout, which avoids a new segment when the depth changes from indexed to direct colour, at several times the size. |
| `canvas` | `"auto"`, `"WxH"` | `auto` | The stored frame size. `auto` is the screen's size at start. A mode smaller than the canvas is centred on black; a mode larger than it starts a new segment. A `WxH` smaller than the screen is raised to the screen's size. |
| `block` | boolean | headless `true`, browser `false` | When the recorder falls behind: `true` waits for it (nothing dropped, deterministic file), `false` repeats the previous frame and counts it in `dropped`. Can be changed while recording. |
| `threaded` | boolean | `true` | Code on a recorder thread. `false` codes on the emulator thread (`--recorder=inline`); the file is byte-identical. |
| `ac_coupled` | boolean | `true` | Record the sound AC-coupled, as it leaves the machine (a 15 Hz DC blocker, like the output capacitor: a constant level is silent). `false` (`--recorder=raw`) records the DAC's raw output, as `machine.sound.capture` takes it -- what `find` against a capture golden needs. |

### State (read-only)

| Attribute | Meaning |
|---|---|
| `active` | True while recording. |
| `path` | The file being written, or the last one written. |
| `frames` | Output frames recorded, all segments. |
| `units` | Guest frames completed since the start (`frames` = `units` / 1, 2 or 4). |
| `seconds` | Emulated seconds the recorded frames cover. |
| `bytes` | File size so far, all segments. |
| `dropped` | Frames recorded as a repeat because the recorder was behind. |
| `segments` | Files written so far. |
| `suspended` | True between `suspend` and `resume`. |
| `external` | True when the GPU presents the screen (a WebGPU takeover): it is read back, at half rate at most. |

### Headless flags

| Flag | Meaning |
|---|---|
| `--record=PATH` | `recorder.start(PATH)` after the machine and media are set up, before any script. |
| `--recorder=inline` | `recorder.threaded = false`. |
| `--recorder=raw` | `recorder.ac_coupled = false` (before `--record` starts). |

### Events

The recorder emits `recorder` state events:
`{"event":"recorder","state":…,"path","segment","frames","seconds","bytes","dropped","external"}`
with `state` one of `started`, `recording` (at most once per host second),
`segment`, `suspended`, `error` (carries `message`, always followed by
`stopped`), `stopped`.

## When a recording goes on in a new file

One recording can produce several files, `name.mkv`, `name-2.mkv`, …;
`stop()` and `segments` list them.

| Event | Result |
|---|---|
| Mode change that fits the canvas and the stored format (for example 1 → 8 bpp) | A keyframe in the same file. |
| A larger resolution, or direct colour after starting indexed with `format = "auto"` | New segment. |
| `checkpoint.load` of a machine that fits | A keyframe cut in the same file. |
| `machine.boot` (a different machine) | New segment. |
| `suspend` then `resume` (the browser's hidden page) | New segment. |
| `machine.shutdown`, `quit()` | The file is finalised. |

To keep one file across a depth change, set `format = "rgb"`; across a
resolution change, set `canvas` to the largest size up front.

## Checking a recording

`recorder.inspect(path)` decodes the whole file with the in-tree reader
and returns:

`frames`, `keyframes`, `width`, `height`, `bits`, `frame_period_ns`,
`last_ms`, `duration_ms`, `video_bytes`, `audio` (boolean), `audio_rate`,
`audio_channels`, `audio_packets`, `audio_frames`, `audio_bytes`,
`audio_first`, `audio_last`, `audio_min`, `audio_max`, `audio_mean` (the
sound's level: first and last sample, extremes, mean -- its DC),
`decode_errors`, `truncated`, `size`, `crc32`.

```lua
local i = recorder.inspect(paths[1])
assert(i.decode_errors == 0 and not i.truncated)
recorder.extract(paths[1], F"${vars.WORK_DIR}/last.png", -1)   -- last frame
recorder.extract(paths[1], F"${vars.WORK_DIR}/sound.wav")      -- all sound
machine.screen.match(F"${vars.WORK_DIR}/last.png")             -- equals the screen
```

`frame_period_ns` is 16625103, 33250206 or 66500412 for full, half and
quarter rate.

With ffmpeg (6.x reads these files):

```bash
ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate,sample_rate -of csv=p=0 tmp/demo.mkv
```

```bash
ffmpeg -v warning -i tmp/demo.mkv -f null -
```

The second prints nothing for a clean file.

## Making an MP4

The `.mkv` plays in VLC, mpv and ffplay, and YouTube accepts it directly.
Browsers, QuickTime Player and phones need an MP4.

- **Browser:** **Export MP4…** in the notice shown when a recording stops,
  or right-click an `.mkv` in the Files tab. H.264 video with AAC sound
  (Opus where the browser has no AAC encoder, no sound where it has
  neither); size 1× (default) to 4×, nearest neighbour. The `.mp4` is
  written beside the recording.
- **Headless, or any `.mkv`:** ffmpeg. This scales four times with square
  pixels, which also keeps YouTube from choosing its lowest quality:

```bash
ffmpeg -i tmp/demo.mkv -vf scale=iw*4:ih*4:flags=neighbor -c:v libx264 -crf 16 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart tmp/demo.mp4
```

H.264 needs even dimensions, so use an even scale factor for odd-sized
screens or add `pad=ceil(iw/2)*2:ceil(ih/2)*2`.

## Browser

- The **Record** button (●) on the display toolbar opens a menu: **Start
  Recording**, the three rates, **Record Sound**. While recording it is a
  red stop button. The status bar shows the emulated time, the size and
  the dropped count.
- Files go to `/opfs/recordings/`. Stopping offers **Download**, **Export
  MP4…** and **Show in Files**.
- From the page's terminal or a test: `recorder.start` / `recorder.stop`
  through `gsEval`, exactly as headless.
- Hiding the tab finalises the file; showing it again continues in the
  next segment.
- e2e examples: `tests/e2e/web2-specs/recorder.spec.ts`,
  `recorder-export.spec.ts`.

## Determinism

With `block = true` (the headless default) the file is a pure function of
the guest's execution: byte-identical across turbo and real-time pacing,
threaded and inline coding. `tests/integration/recorder-plus-boot` holds a
golden CRC-32 of a whole file.

For a reproducible file the guest itself must be reproducible: pin the
clock (`machine.rtc.time = 3000000000`) before the first frame, and drive
input from the script.

With `block = false` the bytes depend on host timing once the recorder
falls behind (frames are dropped and the compression level falls), though
every frame that is not dropped still decodes exactly.

## Pitfalls

- **Options after start are refused.** `rate`, `audio`, `format`,
  `canvas` and `ac_coupled` must be set before `start`; with `--record=` the
  recording is already running when the script begins, so use
  `recorder.start` in the script for anything but the defaults (or the
  `--recorder=` flags, which apply before it).
- **`find` against a capture needs a raw recording.** The recorded sound
  is AC-coupled by default and no longer equals `machine.sound.capture`'s
  raw DAC samples; set `recorder.ac_coupled = false` (or
  `--recorder=raw`) for a recording a capture golden must be found in.
- **Headless needs a path.** `recorder.start()` with no argument fails
  with "a path is required".
- **`.mkv` only.** Any other extension is refused.
- **Headless does not pace.** `scheduler.mode = "paced"` does not slow a
  headless run to real time; the recording is the same either way.
- **`scheduler.run(n)` is a cycle budget**, not an exact instruction
  count; split runs by budget when comparing recordings.
- **Quarter and half rate drop the tail.** The file ends at the last whole
  output frame, so up to three guest frames at the end are not in it.
- **Sound placement.** Silence between sounds is held on the guest's
  timeline; the stream stays within two guest frames of emulated time and
  ends each segment on the exact sample count.
- **The monitor's response is always applied**, so a decoded frame equals
  `recorder.snapshot`, and equals `screen.save` only on monitors whose
  response is the identity.
- **Sizes to expect:** an unchanged frame costs 10 bytes; a 256 s Plus
  session is about 0.6 MB with sound, 134 s of 8-bit Marathon about
  1.3 MB.

## Tests to copy from

| Test | Shows |
|---|---|
| `tests/integration/recorder-plus-boot` | `--record`, golden CRC, last frame equals the screen, inline equals threaded, boot beep placement |
| `tests/integration/recorder-pause` | Frame counts at the three rates |
| `tests/integration/recorder-mode-change` | Same-file keyframe versus new segment |
| `tests/integration/recorder-machine-swap` | `checkpoint.load`, `machine.boot`, `machine.shutdown` while recording |
| `tests/integration/recorder-suspend` | `suspend` / `resume` |
| `tests/integration/recorder-monitor-response` | `snapshot` against a decoded frame |
| `tests/integration/recorder-audio` | `find` against a sound capture (`--recorder=raw`) |
| `tests/integration/recorder-ac-coupled` | The coupled sound: no DC, no step at a suspend/resume join; raw keeps the level |
