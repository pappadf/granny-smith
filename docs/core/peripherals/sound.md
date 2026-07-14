# Sound Subsystem

This document explains the Macintosh Plus audio model and the emulator's current WebAudio implementation. It focuses on the **AudioWorklet ring-buffer path with micro rate trim and silence-aware depth management** that is active today.

Since the stage-0 audio generalization (proposal-sound-support-all-models), the
platform boundary is a single parameterized stream shared by all machines —
`platform_audio_open` / `platform_audio_push` / `platform_audio_set_rate`,
carrying interleaved **int16** frames (mono or stereo) at a runtime-settable
source rate — fronted core-side by `audio_out.c`, which also hosts the
deterministic capture sink for golden-WAV tests (§4a). The Plus PWM path
described here and the ASC producer (stage 1: `asc.c`'s sample-rate drain
event renders FIFO/wavetable frames, batches them, and pushes with the
board's speaker mix — SE/30 sums both channels, IIx/IIcx take channel A)
share that stream; see [asc.md](asc.md) for the chip model.

---

## 1. Macintosh Plus Sound Hardware (Summary)

References: 
- [1] Guide to the Macintosh Family Hardware (Second Edition)
- [2] Inside Macintosh Volume IV (1986)
- See also the large comment header in `src/core/peripherals/sound.c`

Key facts:

* The sound circuitry shares timing with video: 370 sound buffer *words* are scanned per video frame.
* Nominal vertical scan (VBL) frequency: ≈ 60.147 Hz (sometimes cited ~60.15 Hz).
* Effective sample rate: 370 samples / frame × 60.147 frames / second ≈ **22,254.5 Hz** (exact: 22.2545 kHz)
* Audio format: mono, 8‑bit PWM from the high byte of each 16‑bit word; low byte used for disk speed info.
* Buffer organization: 370 words (each word: [sound byte | disk speed byte]).
* Buffer locations:
  - Main sound buffer: top of RAM - 0x300 [2]
  - Alternate sound buffer: SoundBase - 0x5C00 [1] (switched via `sound_use_buffer()`)
* Volume: 3‑bit value written to VIA data register A (low-order bits), range 0–7.
* Sound on/off: Bit 7 of VIA data register B.

Detailed video timing (from source comments):
* Visible display: 342 horizontal scan lines, 15,367.65 µs
* Vertical blanking: 28 scan lines, 1,258.17 µs
* Full frame: 370 scan lines, 16,625.8 µs → 60.15 Hz
* Pixel clock: 15.6672 MHz (≈0.064 µs per pixel)
* Horizontal timing: 512 visible pixels (32.68 µs) + 192 blanking pixels (12.25 µs) = 704 pixels/line (44.93 µs) → 22.25 kHz horizontal scan rate, matching sound sample rate

Implications for emulation:

* Each vertical blank (VBL) must yield exactly **370 samples** to maintain the historically accurate rate.
* If the emulated VBL cadence drifts from the host display / scheduling cadence, long‑term sample *rate* mismatch appears (even if short‑term buffering hides it) and manifests as recurring underruns or time drift.

---

## 2. Sample Production in the Emulator

Function: `sound_vbl(sound_t* sound)` in `src/core/peripherals/sound.c`.

Per host VBL:
1. Copy 370 sound bytes from the emulated RAM sound buffer into a local 370‑byte array (order adjusted for the System 6 offset `VBL_OFFSET = 90`).
  - System 6 starts at an offset of 90 words into the sound buffer, determined by machine type byte from ROM (offset $08 from ROM base at $02AE). The offset is either 0x57 (87 decimal) or 0x5A (90 decimal) depending on machine type; the emulator uses 90.
  - The following code from the original ROM illustrates this logic:

    ```assembly
    ; System 6 sound buffer offset selection
    e03e5bd6  2078  MOVEA.L   $02AE,A0         ; rom base
    e03e5bda  4a28  TST.B     $8(A0)           ; machine type byte
    e03e5bde  41fa  LEA       *-$E0,A0
    e03e5be2  6706  BEQ.b     6
    e03e5be4  30bc  MOVE.W    #$57,(A0)
    e03e5be8  6004  BRA.b     4
    e03e5bea  30bc  MOVE.W    #$5A,(A0)
    ```
   - Sound data is in big-endian 16-bit words; high (even address) byte contains the sound sample, low (odd address) byte contains disk-speed information.
2. The 8-bit offset-binary bytes are expanded to signed int16 mono frames (`(b - 128) << 8`) and forwarded unconditionally via `audio_out_push(frames, 370, volume)` → `platform_audio_push`. Silence detection and depth management happen in the platform layer (`em_audio.c`), not here.

Volume (0–7) is passed through unchanged; scaling to linear 0.0–1.0 happens in JS.

Note: On cold boot the emulator initializes the mixer volume to 4 (a reasonable audible level) so the boot chime is audible even before the guest sets the VIA volume register.

---

## 3. Data Flow Overview

### 3.1 Current (AudioWorklet + MessagePort)

```
sound_vbl()                                     (C)
  -> audio_out_push(frames, 370, vol)           (C, core — capture-sink tap)
    -> platform_audio_push()                    (C, platform boundary)
      -> gs_audio_push(ptr,nframes,vol)      (EM_JS, proxied to main thread)
        - Copies int16 frames from the WASM heap
        - Detects silence (all samples equal)
        - Posts {buf, vol, sil} to the worklet via MessagePort (transfer)

AudioWorkletProcessor ('gs-audio-worklet') onmessage:
  - {rate}: stream restart — flush ring, re-gate, gain ramp from 0
  - {buf}: appends frames to its internal Int16Array ring
           (on overflow drops oldest); tracks consecutive silent pushes

AudioWorkletProcessor.process() executes per render quantum (~128 frames @ output rate):
  - If not yet "started" and available < target (~83 ms) -> output silence (gate)
  - Micro rate trim: PI controller computes stepAdj to keep ring depth near target
  - If have < 2 source frames: count underrun, output 0
  - Resample (linear interpolation, per channel) + one-pole LPF + volume ramp
  - Advance fractional source position & consume ring frames traversed
  - Silence-aware depth trim: after >= 8 consecutive silent pushes,
    trim ring depth back to the target
```

Ring buffer semantics:
* Capacity: 65,536 frames (power-of-two), interleaved int16, worklet-internal.
* Indices are in frames; depth (source domain): `(writeIdx - readIdx) & mask`.
* Underrun counter — incremented for any starvation (`have < 2`).
* Consecutive silent push count — incremented on each silent chunk (flagged by
  the push side), reset to 0 on non-silent. Identifies sustained silence.

**Single-writer discipline**: the push side only sends messages; the worklet is
the sole owner of both ring indices (it advances writeIdx in `onmessage` and
readIdx in `process()`, which the worklet thread serializes). Earlier designs
where two threads shared the read index caused audible glitches.

---

## 4. WebAudio Integration (Current Worklet Implementation)

Implemented via `EM_JS` in `src/platform/wasm/em_audio.c` (compiled into `build/main.mjs`). All calls are proxied to the main thread (AudioContext / AudioWorklet are main-thread-only under `PROXY_TO_PTHREAD`):

* `gs_audio_init()` (from `em_audio_init()` at startup)
  - Creates / reuses `AudioContext`.
  - Registers the `gs-audio-worklet` `AudioWorkletProcessor` class from a blob.
  - The worklet *node* is created lazily by the first `open`.
* `gs_audio_open(rate, channels)` (from `platform_audio_open`)
  - Same channel count: an in-place rate change (worklet stream restart).
  - Different channel count (or first call): (re)creates the node with
    `outputChannelCount: [channels]` and the given source rate.
* `gs_audio_set_rate(rate)` (from `platform_audio_set_rate`)
  - Posts `{rate}` to the worklet: flush ring, re-gate at target depth, brief
    gain ramp — a stream restart, per the rate-switching design.
* `gs_audio_push(ptr, nframes, volume)` (from `platform_audio_push`)
  - Copies `nframes × channels` int16 samples from the WASM heap (must copy —
    the heap can grow/move), detects silence (all samples equal), and posts
    the buffer to the worklet via MessagePort with transfer; never blocks.

Autoplay unlock: Modern browsers (especially Safari) start `AudioContext` in a suspended state until a user gesture. `gs_audio_push` attempts a `resume()` on every push, and the web2 UI resumes the context on the first user gesture.

## 4a. Platform Audio API & Deterministic Capture (stage 0)

The platform boundary (`src/platform/*/platform.h`) is three functions, shared
by every machine:

```c
void platform_audio_open(uint32_t src_rate_hz, int channels /*1|2*/);
void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7);
void platform_audio_set_rate(uint32_t src_rate_hz);   // e.g. ascClockRate
```

Producers do not call these directly — they go through `audio_out.c`
(`audio_out_open/push/set_rate`), a thin core-side front that forwards to the
platform sink and hosts the **capture sink** for deterministic audio testing
(the audio analog of the `machine.screen.match` screenshot machinery):

* `machine.sound.capture.start` / `machine.sound.capture.stop [file.wav]` —
  record the producer output **at guest rate** (pre-resampling, pre-volume)
  between two points in a test script; bounded by instruction budgets, hence
  bit-reproducible on every host and in CI.
* `machine.sound.match <golden>.wav` — sample-exact comparison (PCM int16 WAV;
  channels + rate must match, zero tolerance on sample data). On mismatch it
  reports the first divergent sample index (and its timestamp) and writes the
  capture next to the golden as `<golden>.actual.wav` so it can be auditioned
  and diffed.

Goldens are regenerated by passing a path to `capture.stop` (then auditioned
once before committing); see `tests/integration/plus-boot-beep/`.

In headless builds the `platform_audio_*` functions are no-op stubs — the
capture sink sits ahead of the platform boundary, so headless tests exercise
the exact frames the browser worklet would receive.

---

## 5. Resampling & Filtering

We must convert from source rate ≈ 22,254.5 Hz to the AudioContext output rate (commonly 44,100 or 48,000 Hz). Current method: **linear interpolation**.

Pseudo:
```
step     = srcRate / dstRate  // ≈ 0.504 for 44.1k, ≈ 0.463 for 48k
pos = 0
for each output frame:
   i0 = floor(pos)
   i1 = i0 + 1
   frac = pos - i0
   s0 = (src[i0]-128)/128
   s1 = (src[i1]-128)/128
   sample = s0 + (s1 - s0) * frac
   pos += step
```

Properties (worklet path):
* Linear interpolation executed sample-by-sample inside `process()`.
* Followed by a one-pole low-pass smoothing filter (`lpfY += a*(x - lpfY)`) with cutoff ~8 kHz to soften PWM edges.
* Volume ramp applied per frame for click-free volume changes.

---

## 6. Buffering & Latency Strategy

Policy: **target latency = 5 VBLs ≈ 1850 source samples ≈ 83 ms**.

### 6.1 Start Gate

The processor outputs silence until ring depth ≥ targetSamples, then flips `started=true`. This prevents early thin playback with insufficient buffering.

### 6.2 Micro Rate Trim (PI Controller)

A proportional-integral controller adjusts the resampling step to keep ring depth hovering at the target:

```
error = avail - target          // + => overfull, - => draining
errI  = 0.995 * errI + 0.005 * error   // slow integral
adj   = (error/target) * Kp + (errI/target) * Ki
adj   = clamp(adj, -0.002, +0.002)     // ±2000 ppm
stepAdj = baseStep * (1 + adj)
```

Parameters: `Kp = 0.005`, `Ki = 0.001`, clamp ±0.002 (±2000 ppm). The wider ±2000 ppm window (vs the earlier ±500 ppm) allows the worklet to compensate for up to ~0.2% speed mismatch between the emulator and real-time, which is common in practice.

### 6.3 Silence-Aware Depth Trim

**Problem**: When the emulator runs faster than real-time, it produces more VBLs (and thus more audio buffers) than real time elapses. During silence, this excess accumulates in the ring buffer. When real sound eventually arrives, it sits behind a wall of silence and plays with noticeable delay.

**Solution**: Two coordinated mechanisms prevent this:

1. **Push side** (`gs_audio_push`): Flags each chunk as silent/non-silent; the worklet tracks the consecutive-silent count (reset on any non-silent push). All buffers (including silence) are always written to the ring to maintain stream continuity for the worklet's resampler and LPF.

2. **Worklet** (`process()`): After the normal output loop, if ≥ 8 consecutive silent pushes (~133ms of sustained silence), trims ring depth back to targetFrames by advancing readIdx. This is safe because the entire ring content is guaranteed to be silence after that many consecutive silent VBLs (target depth ≈ 5 VBLs worth, and 8 > 5).

**Why trim in the worklet, not the push side**: The worklet owns readIdx. Manipulating readIdx from another thread races with the worklet's read-modify-write of readIdx during `process()`, causing the worklet to overwrite the other side's changes — which manifested as audible glitches in earlier SharedArrayBuffer-based designs.

**Why always write (never skip)**: Earlier designs skipped silent buffers on the push side. This created gaps in the sample stream, causing the worklet's LPF and resampler state to lose continuity. The result was clicks/hiccups at silence-to-sound transitions (e.g., the boot beep).

---

## 7. Underrun Detection & Recovery

Worklet logic:
* Underrun condition: `have < 2` source samples available during the output loop.
* On underrun: increment state[4] and output zero for that output frame.
* All underruns are counted equally — there is no longer a distinction between "silent" and "non-silent" underruns.

---

## 8. Drift Between Host VBL and Macintosh VBL

Potential mismatch:
* Host display timer (e.g. 59.94 Hz, 59 Hz, or 60.00 Hz) vs required 60.147 Hz.
* If host provides fewer VBL ticks per second than needed, raw sample production runs *slow* relative to real Macintosh time → cumulative deficit.
* The micro rate trim (§6.2) compensates for up to ±0.2% mismatch. Beyond that, the ring gradually drains or fills.

Additional protection:
* If the emulator runs *fast* during silence: the depth trim (§6.3) prevents unbounded latency growth.
* If the emulator runs *fast* during active audio: the ring overfills, but the push side drops oldest samples on overflow (capacity limit). The rate trim also speeds up consumption.

---

## 9. Silence Handling

* Detection: All samples in a chunk are identical (any DC level) → classified as silent.
* All chunks (silent or not) are always written to the ring buffer. This maintains continuous sample flow for the worklet's resampler and LPF.
* The consecutive silent push counter (worklet-side, driven by the push flag) is the signaling mechanism. It is:
  - Incremented on each silent push.
  - Reset to 0 on any non-silent push.
  - Read by the worklet to decide whether depth trimming is safe.
* The threshold of 8 consecutive silent pushes (~133ms at 60.15 Hz) ensures the entire ring content (target depth ~83ms) is silence before trimming.

---

## 10. Volume Handling

* Raw 3-bit (0–7) volume stored in state[2] by the push side.
* Worklet maps to linear gain: `targetGain = min(max(volume,0),7) / 7`.
* Gain is ramped smoothly over each render quantum (`gainStep = (target - current) / frames`) to avoid clicks on volume changes.
* Future work: Implement a perceptually closer curve or replicate original analog characteristics if documented (e.g., approximate dB steps).

---

## 11. Logging & Diagnostics

Worklet path currently has minimal logging (can be expanded):
* Node creation: `[audio] worklet open rate=... ch=... targetLatency=83.0ms`.
* Underruns, silent-push count and ring depth live inside the worklet
  (`this.underruns`, `this.silentCount`, `(wIdx-rIdx)&mask`); expose them via a
  port message if diagnostics are needed from the page.

---

## 12. Current Limitations

| Area | Current Approach | Limitation |
|------|------------------|-----------|
| Resampling | Linear + micro step trim | Modest HF loss vs polyphase |
| Scheduling | AudioWorklet pull (MessagePort-fed ring) | Page still needs COOP/COEP for pthreads |
| Latency | Fixed 5 VBL (~83 ms) | Not dynamically lowered under perfect conditions |
| Jitter Resilience | Rate trim + silence depth trim | Non-silent starvation still audible gap |
| Drift Correction | PI rate trim ±2000 ppm | ~0.2% max; larger drifts cause underrun/overflow |
| Filtering | One-pole LPF @ ~8 kHz | Not brick-wall; still PWM residue |
| Volume Curve | Linear 0–1 | Not perceptually uniform |
| Threading | Worklet | Adds complexity vs simple main-thread fallback |

---

## 13. Planned / Potential Improvements

1. **Enhanced Diagnostics**: Periodic depth / underrun / ppm adjustment logs.
2. **Configurable Target Latency**: User-selectable 3–6 VBL trade-off or dynamic tightening when stable.
3. **Better Resampler**: Polyphase FIR or windowed sinc for cleaner HF; optional toggle.
4. **Perceptual Volume Curve**: dB-ish mapping (e.g. approximate -30 dB to 0 dB across 0–7).
5. **Multi-Stage Filtering**: Higher order low-pass or noise shaping to tame PWM spectral edges.
6. **Fallback Auto-Select**: Detect absence of SAB and transparently revert to a main-thread scheduler.

---

## 14. Troubleshooting Quick Reference

| Symptom | Likely Cause | Mitigation |
|---------|--------------|-----------|
| No sound at boot | AudioContext suspended (no user gesture) | Click/press key before or during boot |
| Frequent early underruns then stabilize | Startup before buffer primed | Normal; start gate handles this |
| Repeating underruns during active sound | Ring draining; host too slow | Rate trim should compensate up to ±0.2% |
| Sound increasingly delayed over time | Silence accumulating in ring | Verify silence depth trim is active (state[5] counting up during silence) |
| Click/hiccup at silence-to-sound transitions | Stream discontinuity | Ensure all buffers are written (never skipped) |

---

## 15. Security / Cross-Origin Considerations

The audio data path itself uses MessagePort (no SharedArrayBuffer), but the emulator runs with pthreads (`PROXY_TO_PTHREAD`), which requires cross-origin isolation (COOP/COEP) for the page as a whole. Use the integrated dev server which sets these headers:

* `make run` → runs `scripts/dev_server.py` (serves on http://localhost:8080 with COOP/COEP)

Autoplay policies: A user gesture is required to start audio in many browsers. Use the Run button or press a key/click inside the page; the UI resumes the `AudioContext` automatically on first gesture.

Testing: headless golden-WAV capture tests (§4a) exercise the producer path deterministically; see `tests/integration/plus-boot-beep/`.

---

## 16. Glossary

* **VBL (Vertical Blank / Vertical Retrace)**: Interval between screen refreshes; synchronizes sound buffer stepping on the Mac Plus.
* **Underrun**: No audio scheduled at the time it needs to play, causing a gap.
* **Drift**: Long-term divergence between emulated sample clock and wall-clock due to mismatched frame rates.
* **PI Controller**: Proportional-Integral feedback loop used for micro rate trim.
* **Silence Depth Trim**: Worklet-side mechanism to discard excess silent data from the ring buffer during sustained silence, preventing latency accumulation.
* **Single-Writer Discipline**: Design principle ensuring each shared state slot is written by only one thread/context.

---

## 17. High-Level Pseudocode (Current Implementation)

```pseudo
=== Push side (gs_audio_push, main thread) ===

  copy nframes × channels int16 samples from the WASM heap
  detect silence (all samples equal)
  post {buf, vol, sil} to the worklet port (transfer)

=== Worklet onmessage (audio thread) ===

  {rate}: srcRate = rate; flush ring; started = false; gain -> 0
  {buf}:  silentCount = sil ? silentCount+1 : 0
          if overflow: drop oldest frames from ring
          append frames; advance writeIdx

=== Worklet (process(), audio thread) ===

  avail = (w - r) & mask                       // in frames
  if !started and avail < targetFrames: output zeros; return

  // PI rate trim
  error = avail - targetFrames
  errI = 0.995 * errI + 0.005 * error
  adj = clamp(Kp * error/target + Ki * errI/target, ±2000ppm)
  stepAdj = baseStep * (1 + adj)

  // Output loop
  for each output frame:
    if have < 2: underruns++; output 0; continue
    per channel: linear interpolate two source samples,
                 one-pole LPF, volume ramp
    advance frac += stepAdj; consume passed source frames
  store readIdx

  // Silence depth trim
  if silentCount >= 8:
    depth = (w2 - r2) & mask
    if depth > targetFrames:
      advance readIdx by (depth - targetFrames)
```

---

## 18. Summary

The emulator streams audio through an AudioWorklet pull model with a worklet-internal int16 frame ring fed over MessagePort. Key mechanisms:

* **Fixed latency target** (~83ms, ≈5 Plus VBLs of source frames).
* **PI micro rate trim** (±2000 ppm) keeps ring depth stable despite emulator speed variations.
* **Silence-aware depth trim** by the worklet prevents latency accumulation when the emulator runs fast during silence.
* **Always-write discipline** ensures stream continuity — silent buffers are never skipped at the push point, preventing glitches at silence-to-sound transitions.
* **Single-writer discipline** — push side owns writeIdx, worklet owns readIdx — eliminates cross-thread races.

Linear interpolation plus a one-pole LPF provide basic quality at low CPU cost.

---

## 19. Checkpointing & State Persistence

The sound subsystem supports state checkpointing for save/restore functionality:

Structure layout considerations:
* The `sound_t` struct is organized with plain data first (volume, enabled flag) followed by pointers (buffer, memory map reference).
* This layout enables efficient checkpointing via `offsetof(sound_t, buffer)` to identify the contiguous plain-data portion.

Checkpoint operations:
* **Save**: `sound_checkpoint()` writes only the plain-data fields (volume and enabled state) using `offsetof` to calculate size. Buffer pointer and memory map reference are NOT serialized.
* **Restore**: `sound_init()` loads the plain-data portion from checkpoint if provided. The actual buffer contents are NOT restored here—RAM is checkpointed separately by the memory subsystem and already contains both main and alternate sound buffers.
* **Post-restore**: VIA device outputs will re-drive buffer selection, volume, and enable state after device initialization completes.

Design rationale:
* Pointers would be invalid after restore (different address space).
* Buffer contents live in emulated RAM and are handled by `memory_map_checkpoint`.
* Only sound-specific state (volume/enabled) needs explicit serialization.

---

For questions or to propose changes, open an issue or extend this document with new experiments, depth traces, or quality metrics.
