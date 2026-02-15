# Sound Subsystem

This document explains the Macintosh Plus audio model and the emulator's current WebAudio implementation. It focuses on the **AudioWorklet + SharedArrayBuffer ring buffer path with micro rate trim and silence-aware depth management** that is active today.

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
2. All buffers are forwarded unconditionally via `platform_play_8bit_pwm(buf, 370, volume)`. Silence detection and depth management happen in the platform layer (`em_audio.c`), not here.

Volume (0–7) is passed through unchanged; scaling to linear 0.0–1.0 happens in JS.

Note: On cold boot the emulator initializes the mixer volume to 4 (a reasonable audible level) so the boot chime is audible even before the guest sets the VIA volume register.

---

## 3. Data Flow Overview

### 3.1 Current (AudioWorklet Pull Model)

```
sound_vbl()                                   (C)
  -> platform_play_8bit_pwm()                 (C)
    -> mplus_audio_push(ptr,len,vol)       (EM_JS)
      - Detects silence (all bytes equal)
      - Updates consecutive silent push counter (state[5])
      - Copies 370 bytes to ring buffer (SharedArrayBuffer)
      - On overflow only: drops oldest to make room

AudioWorkletProcessor.process() executes per render quantum (~128 frames @ output rate):
  - Reads current write/read indices (state[0], state[1])
  - If not yet "started" and available < target (5 VBL) -> output silence (gate)
  - Micro rate trim: PI controller computes stepAdj to keep ring depth near target
  - If have < 2 source samples: count underrun (state[4]++), output 0
  - Resample (linear interpolation) + one-pole LPF + volume ramp
  - Advance fractional source position & consume ring samples traversed
  - Store readIdx
  - Silence-aware depth trim: if state[5] >= 8 (sustained silence),
    trim ring depth back to targetSamples by advancing readIdx
```

Ring buffer semantics:
* Capacity: 128 KiB (power-of-two) storing raw 8-bit source samples.
* Indices: `writeIdx` = state[0], `readIdx` = state[1] (masked). One slot left empty to distinguish full vs empty.
* Depth (source domain): `(writeIdx - readIdx) & mask`.
* Underrun counter: state[4] — incremented for any starvation (`have < 2`).
* Consecutive silent push count: state[5] — incremented by push side on each silent chunk, reset to 0 on non-silent. Used by worklet to identify sustained silence periods.
* State[6], state[7]: reserved (unused).

**Single-writer discipline**: The push side (main thread) only writes state[0] (writeIdx) and state[5] (silent count). The worklet (audio thread) is the sole writer of state[1] (readIdx). This avoids race conditions — earlier designs where both sides wrote readIdx caused audible glitches.

---

## 4. WebAudio Integration (Current Worklet Implementation)

Implemented via `EM_JS` in `src/platform/wasm/em_audio.c` (compiled into `build/main.mjs`):

* `mplus_audio_init()`
  - Creates / reuses `AudioContext`.
  - Allocates `SharedArrayBuffer` for data (128 KiB) + state (8 × Int32).
  - Dynamically generates & registers an `AudioWorkletProcessor` class.
  - Connects the node to destination.
* `mplus_audio_push(ptr,len,volume)`
  - Detects silence (all bytes equal) and updates consecutive silent push counter (state[5]).
  - Copies the 370 bytes from WASM heap into ring buffer. On overflow drops oldest to make room; never blocks.
  - Stores volume in state[2].
* Processor state layout (Int32 array):
  - 0: writeIdx (written by push side)
  - 1: readIdx (written by worklet only)
  - 2: volume (0–7, written by push side)
  - 3: flags (bit0 paused — currently unused)
  - 4: underrun counter (incremented by worklet)
  - 5: consecutiveSilentPushes (written by push side, read by worklet)
  - 6: reserved (unused)
  - 7: reserved (unused)

Fallback: If `SharedArrayBuffer` is unavailable (missing COOP/COEP), initialization logs a warning and audio is disabled. There is currently no automatic fallback. Serve via the provided dev server to enable SAB.

Autoplay unlock: Modern browsers (especially Safari) start `AudioContext` in a suspended state until a user gesture. The Web UI (`web/index.html`) proactively resumes the context on the Run button click and on the first pointer/keyboard/touch gesture.

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

1. **Push side** (`mplus_audio_push`): Tracks consecutive silent pushes in state[5]. Resets to 0 on any non-silent push. All buffers (including silence) are always written to the ring to maintain stream continuity for the worklet's resampler and LPF.

2. **Worklet** (`process()`): After the normal output loop, checks state[5]. If ≥ 8 consecutive silent pushes (~133ms of sustained silence), trims ring depth back to targetSamples by advancing readIdx. This is safe because the entire ring content is guaranteed to be silence after that many consecutive silent VBLs (targetSamples ≈ 5 VBLs worth, and 8 > 5).

**Why trim in the worklet, not the push side**: The worklet is the sole consumer (writer of readIdx). Manipulating readIdx from the push side races with the worklet's read-modify-write of readIdx during `process()`, causing the worklet to overwrite the push side's changes — which manifested as audible glitches.

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

* Detection: All 370 bytes in a chunk are identical → classified as silent.
* All chunks (silent or not) are always written to the ring buffer. This maintains continuous sample flow for the worklet's resampler and LPF.
* The consecutive silent push counter (state[5]) is the signaling mechanism. It is:
  - Incremented atomically on each silent push.
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
* Initialization: `[audio] worklet init targetLatency=83.xms`.
* Underruns visible by inspecting state[4] (via Atomics from JS).
* Consecutive silent push count visible via state[5].
* For profiling, read `(state[0]-state[1])&mask` for current ring depth.

---

## 12. Current Limitations

| Area | Current Approach | Limitation |
|------|------------------|-----------|
| Resampling | Linear + micro step trim | Modest HF loss vs polyphase |
| Scheduling | AudioWorklet pull (ring buffer) | Requires SAB (COOP/COEP) |
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

The current audio path requires SharedArrayBuffer and therefore cross-origin isolation (COOP/COEP) to be enabled by the server. Use the integrated dev server which sets these headers:

* `make run` → runs `scripts/dev_server.py` (serves on http://localhost:8080 with COOP/COEP)
* Avoid serving with `python -m http.server` or opening `file://index.html`; SAB will be unavailable and audio will be disabled.

Autoplay policies: A user gesture is required to start audio in many browsers. Use the Run button or press a key/click inside the page; the UI resumes the `AudioContext` automatically on first gesture.

Testing: Type `beep` in the emulator terminal to play a short tone (implemented as a shell helper command in `em_audio.c`).

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
=== Push side (mplus_audio_push, main thread) ===

  detect silence (all bytes equal)
  state[5] = silent ? state[5]+1 : 0    // consecutive silent count
  if overflow: drop oldest from ring
  copy 370 bytes into ring
  state[0] = new writeIdx

=== Worklet (process(), audio thread) ===

  avail = (w - r) & mask
  if !started and avail < targetSamples: output zeros; return

  // PI rate trim
  error = avail - targetSamples
  errI = 0.995 * errI + 0.005 * error
  adj = clamp(Kp * error/target + Ki * errI/target, ±2000ppm)
  stepAdj = baseStep * (1 + adj)

  // Output loop
  for each output frame:
    if have < 2: underruns++; output 0; continue
    linear interpolate two source bytes -> sample
    one-pole LPF; volume ramp
    advance frac += stepAdj; consume passed source samples
  store readIdx

  // Silence depth trim
  if state[5] >= 8:
    depth = (w2 - r2) & mask
    if depth > targetSamples:
      advance readIdx by (depth - targetSamples)
```

---

## 18. Summary

The emulator streams audio through an AudioWorklet pull model with a SharedArrayBuffer ring buffer. Key mechanisms:

* **Fixed 5-VBL latency target** (~83ms, 1850 source samples).
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
