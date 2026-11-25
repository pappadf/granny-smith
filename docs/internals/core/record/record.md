# Screen recorder

The recorder, [`src/core/record/`](../../../../src/core/record/), writes the
emulated screen and sound to a lossless Matroska file on the guest's clock:
ZMBV video and FLAC audio [1][2], one output frame per first, second or
fourth frame-unit. The emulator-thread half — the frame-unit hook, the
capture, the audio sink, segments — is
[`record.c`](../../../../src/core/record/record.c) behind
[`record.h`](../../../../src/core/record/record.h); the recorder thread —
the coder and the ring around it — is
[`record_thread.c`](../../../../src/core/record/record_thread.c); the two
share the record formats of
[`record_internal.h`](../../../../src/core/record/record_internal.h). The
machine-free pieces are the audio timeline
([`record_audio.c`](../../../../src/core/record/record_audio.c)) and the
pixel work ([`record_pixels.c`](../../../../src/core/record/record_pixels.c));
the `recorder` object is
[`record_class.c`](../../../../src/core/record/record_class.c). The
container is the Matroska muxer behind a small muxer interface
([`mkv_mux.c`](../../../../src/core/record/mkv_mux.c),
[`mkv_mux.h`](../../../../src/core/record/mkv_mux.h)) and a reader for the
subset it writes ([`mkv_read.c`](../../../../src/core/record/mkv_read.c),
[`mkv_read.h`](../../../../src/core/record/mkv_read.h)). The codecs are in
the codec library ([../../codec/codec.md](../../codec/codec.md) §1.4). The
browser's MP4 export (§3.8) reads recordings back with the same reader and
decoders, built into a module of their own
([`recdec_main.c`](../../../../src/platform/wasm/recdec_main.c)).

## 1. Responsibilities & design

- **Time is the guest's.** The scheduler calls the recorder through a hook
  (`scheduler_set_frame_hook`) at the end of `scheduler_run_frame`, when
  `saved.frame_cycles_left` has just reached zero: a frame-unit's whole
  budget ran. A frame cut short (a breakpoint, a small `scheduler.run`)
  calls nothing; the call that finishes it does. The recorder counts these
  calls (`units`) and takes a frame on every first, second or fourth
  (`rate` full, half, quarter). A paused machine completes no frame-units
  and records nothing; turbo plays back at the guest's speed. The hook and
  the audio sink are installed only while a recording runs, so recording
  off costs one NULL test in `scheduler_run_frame` and one in
  `audio_out_push`.
- **The emulator thread only copies.** At a frame it takes the band of rows
  that changed (XOR against its shadow copy of the framebuffer), the CLUT
  and the two response tables when they changed, and each audio push, into
  a single-producer, single-consumer byte ring (the mailbox record ring,
  `mailbox_ring.h`, 8 MB, or twice the largest record when that is more;
  allocated at start, freed at stop).
- **The recorder thread does the rest**: it rebuilds the frames, applies the
  response tables, codes ZMBV and FLAC, and muxes. Inline
  (`recorder.threaded = false`, headless `--recorder=inline`) the same coder
  runs on the emulator thread as each record is published (also what a
  build without threads falls back to); with `block = true` the two give
  byte-identical files.
- **The picture is the monitor's.** Both response tables apply (the card's
  `dac_lut`, then the monitor's `crt_response`, display.h's order): for
  indexed pixels through the palette, per pixel for direct colour. A
  decoded frame therefore equals the canvas and differs from
  `machine.screen.save` on a monitor with a curve (the Kong 21").
- **A recording is process state.** It spans machine swaps, never enters a
  checkpoint, and is finalised when the active machine goes with nothing in
  its place (`quit`, `machine.shutdown`).
- **A file can be finished without ending the recording.** Suspending
  closes the file being written, whole, and records nothing until resumed;
  resuming continues in the next segment. The browser does this when the
  page is hidden, so a tab killed while hidden leaves a complete file.

## 2. Key types & files

| | |
|---|---|
| `recorder_settings_t`, `recorder_state_t` | the settings a recording starts with; its state for the attributes |
| `recorder_start`, `recorder_stop`, `recorder_machine_attached`, `recorder_machine_gone` | the lifecycle (`record.c`); the last two from `system.c` |
| `recorder_suspend`, `recorder_resume`, `recorder_suspended` | close the file and hold; continue in the next segment (§3.6); from `em_main.c` on page hide and show |
| `recorder_set_platform_defaults` | a platform's defaults: whether a full ring blocks, the directory for a recording with no path |
| `rec_seg_hdr_t`, `rec_frame_hdr_t`, `rec_repeat_hdr_t`, `rec_audio_hdr_t` | the records: SEGMENT, FRAME, REPEAT, AUDIO (and CLOSE and STOP, no payload) |
| `rec_coder_t` | the coder: the open segment's muxer, ZMBV and FLAC encoders, the raw and stored pictures, the audio timeline |
| `rec_worker_t` | the ring, the recorder thread (or none, inline), the full-ring policy; `rec_worker_sync` waits until every record published has been coded |
| `rec_timeline_t` | the audio timeline (§3.4) |
| `muxer_t`, `muxer_ops_t` | a container writer: queue a packet of a track, close (finalise and free), bytes so far |
| `mux_video_t`, `mux_audio_t` | the tracks: ZMBV frame size, display size, frame period and bit count; FLAC rate, channels, depth and STREAMINFO |
| `mkv_reader_t`, `mkv_track_t`, `mkv_packet_t` | the reader: tracks, then packets (track, timestamp, keyframe, bytes) in file order |
| `mkv_read_open`, `mkv_read_open_file` | open a path, or a stream the caller opened (the export module's, whose reads call back into the page) |
| `recdec_open`, `recdec_next`, `recdec_rgba`, `recdec_pcm` | the export module's C API (`recdec_main.c`): a recording's facts and frame count, then one decoded frame (RGBA) or FLAC block at a time |

## 3. Behaviour/algorithms

### 3.1 Capture on the emulator thread

At a frame (`capture`):

1. The active display (`system_display`), its pixels made current
   (`display_sync_pixels`) for a card that builds them on demand. No display
   is a repeat. A display presented by the GPU (`presented_externally`: the
   Voodoo2 and Rage 128 under their WebGPU takeovers) makes that a readback
   from the GPU worker, so at full rate it is taken on every other
   frame-unit only, the frames between recorded as repeats: at most half
   rate, in a file whose frame period is still the full rate's. The state's
   `external` (and the events') says so. Headless never presents
   externally.
2. Whether the mode fits the open segment (§3.5); if not, the next segment
   starts here. A new mode (size, format, row bytes) reallocates the shadow
   and sets the resync flag.
3. The CLUT (indexed modes) and both tables are compared with their shadows;
   a change goes into the frame's record. After a resync all three go.
4. One pass over the framebuffer: `rec_band_scan` finds the first and last
   rows that differ from the shadow (only the row's pixel bytes, not the
   stride's padding), then `rec_band_take` writes the band into the ring —
   XORed with the shadow, or whole rows after a resync — and updates the
   shadow. The XOR is ZMBV's inter delta as it is for 8 bpp modes.
5. Nothing changed: a pending-repeat counter goes up and nothing is written.
   The repeats go out as one REPEAT record before the next record of any
   kind (so audio never overtakes the frames before it), or once a second's
   worth have gathered.

The audio sink (`recorder_audio_push`, installed with
`audio_out_set_record_sink`) copies each push — samples, channels, the
stream's rate, the guest's volume — into an AUDIO record.

### 3.2 Falling behind

A record that does not fit: with `block = false` (the core default, the
browser's) the frame becomes a repeat, the shadow still advances, the
resync flag is set so the next frame that fits goes whole (and is coded as
a keyframe), and `dropped` counts it. A frame needs 64 KB of the ring free
besides itself, so the small audio records are admitted while any room is
left; an audio push that finds none is lost and the timeline holds across
the gap. With `block = true` (headless's default: `recorder_set_platform_defaults`
from `headless_main.c`) the emulator thread waits for room instead, and the
output is a pure function of the guest's execution. SEGMENT and STOP
records always wait. A record over half the ring grows it first, once the
recorder thread has emptied it (a mode change to a very large screen).

The recorder thread lowers its DEFLATE effort as the ring fills — level 3,
level 1 past half, stored blocks past three quarters — except with
`block = true`, where it stays at 3 so inline and threaded output agree.

### 3.3 The coder

Per FRAME record the coder folds the band into its raw copy of the guest's
frame (XOR, or a copy for whole rows), then builds the stored rows on the
canvas:

| Guest format | Class | Stored as |
|---|---|---|
| 1, 2, 4, 8 bpp | `INDEXED` | ZMBV 8 bpp: one index per byte; the palette is the CLUT (white/black at 1 bpp) through the composite table |
| 16 bpp x-5-5-5 / 5-6-5, no table | `D15` / `D16` | ZMBV 15 / 16 bpp: the big-endian words swapped |
| any direct colour with a table, 32 bpp, or `format = "rgb"` | `D32` | ZMBV 32 bpp, bytes B, G, R, 0: per pixel through the table (indexed pixels through the palette) |

An 8 bpp mode that fills the canvas passes the record's XOR band to the
encoder as its delta; everything else converts the changed rows. A mode
smaller than the canvas is centred on black (in an indexed recording, the
palette index nearest black; a palette change that moves it redraws the
border). Keyframes: the first frame, a resync, a new mode, a table change
in direct colour, and every 600 changed frames (a screen that has not
changed since the last keyframe gets none). REPEAT codes unchanged frames
(about ten bytes each at 512x342).

Before each frame the coder brings the audio to that frame's end (§3.4);
FLAC frames of 4,096 samples go to the muxer as they fill. Video packet *k*
is at *k* × the frame period (16,625,103 ns × the divisor); audio packets
at their first sample's time.

### 3.4 The audio timeline

Pushes carry no timestamp. The timeline (`record_audio.c`) counts the
sample frames it has been given and compares, at each output frame
boundary, with the target round(*units* × 16,625,103 × *rate* / 10⁹). The
lead *e* = (emitted + pending) − target:

- **Idle** — no push for more than the window, or none yet: a short stream
  is held (the last value repeated) up to the target exactly, so a sound
  that starts later starts at its own time (to within the frame-unit it was
  pushed in).
- **Running** — *e* stays inside ± the window: held when short, the oldest
  pending samples dropped when long. A chip a fraction of a hertz off its
  declared rate (the Plus pushes 370 samples per frame-unit, 22,255.5 Hz,
  into a 22,255 Hz stream) is corrected a sample at a time once its lead
  reaches the edge — about 1,500 s into continuous sound for the Plus.
- The **window** is two frame-units of samples, or twice the largest push
  seen: a chip that pushes in chunks is never mistaken for drift.

Samples leave the timeline once they are a window behind the target, where
no correction reaches them. At the end of a segment the stream is held or
cut to exactly the target, so a file's sound covers its frames exactly;
across segments the surplus carries into the next file. On the way in,
each push goes to the segment's channel count (mono to both sides, stereo
averaged), through the guest's volume — gain *v* / 7, linear, ramped from
the previous push's volume across the push that changes it — and, when the
stream's rate is not the segment's, through a linear-interpolation
resampler. A machine with no stream (rate 0) or `recorder.audio = false`
records no audio track. The FLAC STREAMINFO is fixed before any audio
exists (block size 4,096, unknown frame sizes, length and MD5).

### 3.5 Segments and new machines

A segment's canvas (the screen's size at its start, or `recorder.canvas`
when larger) and pixel class are fixed. A mode fits when it is no larger
than the canvas and its class is the segment's (anything fits a 32 bpp
segment). Indexed depth changes therefore stay in one file; a larger mode,
or direct colour in an indexed recording, finalises the file and continues
in the next, `name-2.mkv`, `name-3.mkv`, with a `segment` event.

`system_swap_in` tells the recorder when a new machine becomes active
(`recorder_machine_attached`, beside `machine_booted`):

- **`checkpoint.load`** (restored) that fits — the screen fits the canvas
  and class, the audio stream has the same rate and channels — is a cut in
  the same file: the shadow is discarded, so the next frame goes whole as a
  keyframe, and the audio holds across the join.
- **`machine.boot`**, or a restore that does not fit, is the next segment.
- The frame-unit count carries on; `machine.restart` builds nothing and is
  invisible.

`system_destroy` of the active machine with nothing replacing it calls
`recorder_machine_gone`, which finalises.

### 3.6 Suspending, stopping and failures

`recorder_suspend` writes the repeats owed and a CLOSE record, then waits
(`rec_worker_sync`) until the coder has taken it: CLOSE is the end of a
segment without the end of the recording — the audio brought to the last
frame's end, the last FLAC block, Cues, sizes and Duration, the file
closed — and the timeline's surplus is kept for the next. While suspended
the frame-unit hook and the audio sink return at once, so `units`,
`frames` and `seconds` stand still, and a machine attach opens nothing.
`recorder_resume` starts the next segment for the display there is then
(`name-N.mkv`, a `segment` event), whatever machine it belongs to; a stop
while suspended returns the files written. In the browser,
`em_main.c`'s visibility callback suspends on hidden (before the
background checkpoint, on the same signal) and resumes on visible — only
a recording it suspended itself, so one suspended from the terminal stays
so.

STOP brings the audio to the last frame's end, flushes the last FLAC
block, and closes the muxer (Cues, sizes, Duration); `recorder.stop`
returns once the recorder thread has done so. A write that fails (a full
disk) finalises what exists and marks the coder failed; the emulator thread
notices at the next frame-unit, ends the recording, and emits `error` then
`stopped`. Inline, a file that cannot be opened fails `recorder.start`
itself.

### 3.7 The container

- **Layout.** The EBML header (DocType `matroska`); a Segment whose size
  is written as unknown and patched at close; a 64-byte Void that close
  overwrites with a SeekHead (Info, Tracks, Cues); Info (TimestampScale
  1 ms, the muxing and writing app, an 11-byte Void that close turns into
  the Duration); Tracks — video `V_MS/VFW/FOURCC` with a
  `BITMAPINFOHEADER` CodecPrivate (FourCC `ZMBV`), `DefaultDuration` the
  frame period, pixel and display size; audio `A_FLAC` with CodecPrivate
  `fLaC` plus the STREAMINFO block, sampling frequency, channels, bit depth
  16 — then Clusters of SimpleBlocks, and the Cues at close.
- **Time.** A block's timestamp is its pts rounded to the millisecond, so
  frame *k* of a track with period *P* ns is at round(*kP* / 10⁶) ms; the
  track's constant rate is in `DefaultDuration`, which is how players read
  it (1203/20 fps for the 16,625,103 ns VBL period, 1203/40 and 1203/80 at
  half and quarter rate).
- **Interleaving.** Packets wait per track until every track has one; the
  earliest goes out first, video before audio at the same millisecond. A
  queue that grows past 16 MB or 1024 packets is written out regardless.
  The timeline's holds keep audio coming through silence, so video is
  never held back for long.
- **Clusters.** One is built in memory (a 1 MB buffer, grown for a larger
  frame) and written whole, its size known, when the next block is a video
  keyframe, is a second or more after the cluster's start, or follows 1 MB
  of cluster, or would need a relative timestamp the block's 16 bits
  cannot hold. Each cluster that opens on a video keyframe gets a cue. The
  file is flushed after every cluster, so a crash loses at most the last.
- **The reader** walks front to back and needs neither SeekHead nor Cues;
  a cluster or other element that runs past the end of the file ends the
  walk, and the file counts as cut. It handles the writer's subset, plus
  BlockGroups; not lacing or unknown-size clusters.

### 3.8 The MP4 export (browser)

A recording is lossless and plays in VLC, mpv, ffplay and on YouTube, but
not in a browser, QuickTime Player or a phone. The web app turns a finished
`.mkv` into an `.mp4` with H.264 video and AAC audio, encoded by the
browser through WebCodecs, in a worker apart from the emulator
(`app/web2/src/recorder/`; the page and the dialog are in
[docs/guide/web.md](../../../guide/web.md), "Screen recording").

- **Reading.** The worker opens the `.mkv` from OPFS itself and reads it
  through a small, non-threaded Emscripten module, `build/recdec.js` +
  `recdec.wasm` (`make recdec-module`, part of `make`), built from this
  reader, the codec library's ZMBV and FLAC decoders and
  `src/platform/wasm/recdec_main.c` — the pattern of the LaserWriter
  interpreter's module. Its objects are compiled without `-pthread`. The
  module has no filesystem: `mkv_read_open_file` reads a `fopencookie`
  stream whose reads call `Module.recdecRead(source, at, len)`, which the
  worker answers with synchronous slices of the OPFS `File`
  (`FileReaderSync`), so a recording of any size is read a cluster at a
  time and the emulator's own handle on the file is never contended.
  `recdec_open` scans the packets once to count the frames (the dialog's
  progress), then rewinds. A frame is handed over as RGBA, the palette
  applied (`codec_zmbv_dec_rgb24`), so it equals what
  `recorder.extract` writes.
- **Picture.** The recording's own size by default; 2×, 3× or 4×
  nearest-neighbour. Non-square pixels (`DisplayWidth`/`DisplayHeight`
  of another shape) are widened or heightened to square, to the nearest
  whole pixel. H.264 here is 4:2:0, so an odd width or height gets one
  black column or row (right, bottom). Each frame becomes an RGBX
  `VideoFrame` at *k* × the frame period. The encoder: H.264 High (then
  Main, then Constrained Baseline, if the browser lacks High) at the level
  the frame size and rate need, variable bitrate at 0.2 bits per pixel per
  frame, `latencyMode: "quality"`, a keyframe every two seconds; if the
  default (often a hardware encoder) refuses the configuration, the probe
  is repeated with `hardwareAcceleration: "prefer-software"`.
- **Sound.** The FLAC blocks are resampled to 48 kHz by a windowed-sinc
  resampler (16 zero crossings each side, Blackman window, 256
  interpolated phases, each normalised to unit gain; the output position
  computed in integers, so nothing drifts; the output is round(*n* × 48,000
  / rate) frames) and given to an `AudioEncoder` as planar float: AAC-LC
  (`mp4a.40.2`) at 128 kb/s mono or 192 kb/s stereo. Without an AAC
  encoder (Chrome and Edge on Linux, Playwright's Chromium) the sound is
  Opus at the same rate and bitrate; without either the export is video
  only, and the dialog says so.
- **Pacing.** The decode loop waits while more than eight frames (or
  audio blocks) are queued in an encoder.
- **Container.** The app's own MP4 writer (`mp4Writer.ts`, no
  dependency): `ftyp`, `moov`, `mdat`; `avc1` with the encoder's `avcC`
  and a 1:1 `pasp`; `mp4a` with an `esds` carrying the encoder's
  AudioSpecificConfig, or `Opus` with a `dOps`. The video's time scale is
  1203 Hz with samples of 20, 40 or 80 — the recording's rate exactly — so
  each track's `stts` is one entry and the file is constant-rate by
  construction. Consecutive samples of a track form a chunk (`stsc`,
  `stco`, `co64` past 4 GB); `stss` lists the keyframes. An encoder that
  reorders frames (B-frames) gets a `ctts` and an edit list starting at the
  first frame. The audio's edit list skips Opus' pre-skip (from the
  OpusHead) or an AAC encoder's priming when it announces it with a
  negative first timestamp, and ends at the sound's own length, so picture
  and sound end together. Samples go to a scratch file in OPFS
  (`/opfs/upload/.scratch/…`) as they are encoded; at the end the final
  file is written index first (`<name>.mp4` beside the recording), so it
  streams, and the scratch file is removed.
- **Segments.** A recording that spans several files exports each to its
  own `.mp4`, one after the other. Cancel closes the encoders and removes
  every file the export wrote.
- **Support** is probed at run time, never inferred from a version:
  `VideoEncoder.isConfigSupported` and `AudioEncoder.isConfigSupported`
  with the exact configurations. Without an H.264 encoder the export is not
  offered: "Export MP4…" is disabled, its tooltip naming the ffmpeg
  command of the user guide.

Headless has no export: ffmpeg does the same job
([docs/user/sound-and-video.md](../../../user/sound-and-video.md) §5).

## 4. Object-model / shell surface

`recorder` is a root object beside `checkpoint` (`OBJ_ORDER_RECORDER`),
registered by `core_init`.

| Member | Kind | Meaning |
|---|---|---|
| `start([path])` | method | Begin recording to `path` (`.mkv`); returns the path. No path: the platform's default directory, `<dir>/<machine>-<YYYYmmdd-HHMMSS>.mkv` — the core's default is the browser's `/opfs/recordings`; headless has none and requires a path. Error if already recording or no machine with a screen |
| `stop()` | method | Finalise; returns the paths written (one per segment) once the file is closed |
| `rate` | attr, rw | `full`, `half` or `quarter`; fixed while recording |
| `audio` | attr, rw | Include sound (default true); fixed while recording |
| `format` | attr, rw | `auto` or `rgb`; fixed while recording |
| `canvas` | attr, rw | `"auto"` or `"WxH"`; fixed while recording |
| `block` | attr, rw | Wait instead of dropping (headless default true, core/browser false); may change mid-recording |
| `threaded` | attr, rw, advanced | Code on the recorder thread (false: inline); fixed while recording |
| `active`, `path` | attr, ro | State; `path` is the current file, or the last one written |
| `frames`, `seconds` | attr, ro | Output frames, every segment, and the emulated seconds they cover |
| `bytes` | attr, ro | File bytes so far, every segment |
| `dropped` | attr, ro | Frames recorded as a repeat because the recorder was behind |
| `units`, `segments` | attr, ro, advanced | Frame-units completed since start (not counting suspended ones); files so far |
| `suspend()` | method, advanced | Finalise the file being written and record nothing until `resume`; returns once the file is closed. No-op when already suspended |
| `resume()` | method, advanced | Continue a suspended recording in the next segment; returns its path. No-op when not suspended |
| `suspended` | attr, ro, advanced | True while suspended |
| `external` | attr, ro, advanced | True while the screen is presented by the GPU: taken at half rate at most (§3.1) |
| `inspect(path)` | method, advanced | A recording read end to end: frames, keyframes, size, bits, frame period, video and audio bytes, audio rate, channels and sample frames, decode errors, truncation, the file's size and CRC-32 |
| `extract(path, out, [index], [count])` | method, advanced | A frame (`.png`; `index` counts from the end when negative, default the last) or the sound (`.wav`, from sample `index`, `count` of them) decoded in tree |
| `find(path, wav, [volume])` | method, advanced | The sample frame at which a WAV's samples appear exactly in a recording's sound (a capture taken before the guest's volume is scaled by `volume` first), or -1 |
| `snapshot(path)` | method, advanced | The screen now as a recording shows it: `screen.save`'s pixels through the monitor's curve, computed per pixel apart from the recorder |

`machine.screen.save` and `machine.sound.capture` are unchanged.

**Events.** `{"event":"recorder", "state", "path", "segment", "frames",
"seconds", "bytes", "dropped", "external"[, "message"]}` (`GS_EVENT_STATE`),
with `state` one of `started`, `recording` (at most once a second of host
time while frames are recorded), `segment` (a new file: `path` and
`segment` name it), `suspended` (the file `path` is closed), `error` (with
`message`) and `stopped`. The web app's use of them is in
[docs/guide/web.md](../../../guide/web.md), "Screen recording".

**Headless.** `--record=<path>` starts a recording before the first frame
(after the machine and media are set up, before any script);
`--recorder=inline` makes `threaded` false.

## 5. Checkpointing

None: a recording is process state and is not checkpointed. A checkpoint
load during a recording is a cut (§3.5).

## 6. Testing

- `tests/unit/suites/record_capture`: an hour of a chip at 22,254.54 Hz
  into a 22,255 Hz timeline stays inside the window at every boundary, is
  corrected a held sample at a time and never dropped, and ends on exactly
  round(*T* × *rate*) samples; a silent gap is held and the sound after it
  starts at its own sample; a silent start does not displace the first
  sound; a fast chip's surplus is dropped; the volume ramp's values; a
  half-rate stereo stream resampled and averaged into mono; the band scan
  and XOR; sub-byte widening, 16 bpp swapping, the composite table through
  the palette and per pixel; records through the ring, inline and threaded
  with blocking, give byte-identical files that decode to what went in
  (palette, border, the curve arriving, held silence); segments; a 32 bpp
  table change is a keyframe; a full ring refuses a record under the drop
  policy; a CLOSE leaves a whole file once `rec_worker_sync` returns, and
  the next SEGMENT continues on a keyframe.
- `tests/unit/suites/record_mkv`: known packets on a video and an audio
  track round-trip exactly, in time order, at round(*kP* / 10⁶) ms, in
  clusters that open on keyframes and span under a second (one carrying a
  1.5 MB frame), with a cue per keyframe cluster, the segment size and the
  duration; a video-only file; a file cut halfway into a cluster returns
  exactly the earlier clusters' packets; a file whose muxer was never
  closed reads back up to its last complete cluster.
- `tests/integration/recorder-plus-boot` (unit tier): `--record` a Plus
  boot; the file's CRC-32 is a golden, its sound covers its frames, its
  last decoded frame matches the screen, the same boot coded inline is the
  same bytes, and the boot beep lands at the sample its frame-unit puts it.
- `tests/integration/recorder-pause` (unit tier): odd run sizes with stops
  and host waits, at all three rates: frames = frame-units ÷ divisor
  exactly, and the files' frame periods.
- `tests/integration/recorder-machine-swap` (unit tier): a fitting
  checkpoint load is a keyframe cut in the same file, a boot the next
  segment, a shut-down finalises.
- `tests/integration/recorder-suspend` (unit tier): a suspend closes a
  whole file (not cut, with a duration, its sound covering its frames)
  before returning; nothing is recorded while suspended, across a run and a
  boot; resume continues in `name-2.mkv`; a stop while suspended returns the
  one file; threaded and inline are the same bytes.
- `tests/e2e/web2-specs/recorder.spec.ts`: the toolbar's record button on
  a Plus, the status bar's indicator, stop, the file in OPFS (EBML magic,
  decoded by `recorder.inspect` without errors) and its download; a page
  made hidden finalises the file and, shown again, continues in
  `name-2.mkv`.
- `app/web2/tests/unit/recdec.test.ts` (vitest, Node): a Plus boot
  recorded by `gs-headless --record` at test time, read by the export
  module: its facts, every frame equal byte for byte to `recorder.extract`'s
  PNGs, its sound to the extracted WAV, a cut file to its last cluster.
- `app/web2/tests/unit/mp4Writer.test.ts`, `exportFrame.test.ts`: the MP4
  writer's boxes and sample tables from canned chunks, read back; the
  export sizes, padding and scaling; the resampler's length, a sine's level
  and frequency, streaming in pieces.
- `app/web2/tests/unit/mp4Export.ffmpeg.test.ts` (vitest, Node, with
  ffmpeg): the whole export with stand-in encoders that run ffmpeg
  (libx264 with B-frames, AAC, libopus) — ffmpeg decodes the result without
  a warning, at 1203/20 fps with every frame, the sound as long as the
  picture and the boot beep at its time; and the writer given an
  ffmpeg-made H.264 + AAC stream decodes to the same frames and samples as
  ffmpeg's own file.
- `tests/e2e/web2-specs/recorder-export.spec.ts`: "Export MP4…" disabled
  with its reason where WebCodecs has no H.264 encoder, in the toast and the
  context menu; where it has one, a real export through the dialog,
  downloaded, checked with ffprobe and played in the page's `<video>`.
- `tests/integration/recorder-mode-change` (matrix): a IIcx's 1 → 8 bpp
  boot stays one file; a Quadra 700's 640x480 → 1152x870 two-page mode
  makes `name-2.mkv`.
- `tests/integration/recorder-monitor-response` (matrix): on the IIcx's
  Kong 21", a decoded frame equals `recorder.snapshot` (screen.save through
  `crt_response`) and not screen.save.
- `tests/integration/recorder-audio` (extended): plus-musicworks' session
  recorded; its golden music window is found sample for sample in the
  decoded FLAC, within the window of its emulated time.

## 7. Known debts

- The MP4 export's AAC path has been exercised with ffmpeg's encoder only
  (Playwright's Chromium has no AAC encoder): whether a browser's AAC
  encoder reports its priming (a negative first timestamp) is untested, and
  one that does not leaves the sound up to its priming (about 20–45 ms)
  late. Safari's AAC encoder has been reported to silence the first 1,024
  samples. Both are for a check in Chrome, Edge and Safari.
- The MP4's edit lists are in milliseconds (the movie time scale is
  1,000), so the sound's end is cut to within a millisecond.
- The GPU takeovers' readback cost while recording (§3.1, half rate at
  most) is not measured: headless Chromium's software WebGPU loses its
  device at the first canvas present, so no takeover runs with the machine
  running there. Whether recording those cards should instead be refused
  in the browser is open.
- Hiding the page suspends only through `visibilitychange`; there is no
  `pagehide` hook (Emscripten offers none on the emulator thread, and a
  closing tab fires `visibilitychange` hidden first).
- The 64x64 block size is untuned; on the Plus session an unchanged frame
  is 10 bytes and a frame with one changed block about 36, so a mostly
  static 256 s session is about 0.31 MB of video payload.
- Channel-count changes inside a segment (a chip reopening the stream) are
  mixed or duplicated rather than starting a segment; only a machine
  attach checks them.

## 8. See also

- [../../codec/codec.md](../../codec/codec.md) — the codec library, with
  ZMBV, FLAC and the streaming zlib under ZMBV.
- [../scheduler/scheduler.md](../scheduler/scheduler.md) — the frame-unit.

## References

1. *Matroska Media Container Format Specifications*, RFC 9559, IETF, 2024.
2. *Free Lossless Audio Codec (FLAC)*, RFC 9639, IETF, 2024.
