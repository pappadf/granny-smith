# Singer codec + PSC sound frame engine (`src/machines/av/singer.c`)

The AV family's sound datapath: the PSC's dedicated sound DMA engine and
the Singer codec, modelled as a cadenced frame engine — the structural
twin of the VDC field engine.  Register contract:
`local/gs-docs/840av_660av/docs/singer.md`; tick gating and overrun
semantics: `local/gs-docs/dsp3210-plaintalk/` (B2/B3).

## The frame engine

A scheduler event fires at each frame boundary of the absolute formula
`sample(t) = floor(t · rate / 1e9)`, `frame = sample / sndSize` — the
same formula behind the PSC's free-running `sndPhase`, so the play
position the guest polls and the interrupts it receives can never drift
apart.  Rate = `pSndRate` (24/32/48 kHz), `sndSize` from $218 (240 is
the shipped driver's constant; the ROM chime uses 960); both re-read at
each boundary so reprogramming takes effect on a frame edge.

Per boundary, the engine services the half-buffer that just **finished**
its window (`(frame+1) & 1`):

- **Output** (`pSndOutEn`): read `sndOutBase + half·sndSize·4` — reads
  may address ROM; CycloneBeep plays the chime PCM straight from
  `$408C5D24` — apply `singerCtl` attenuation (1.5 dB ladder as x65536
  integer gains) and `pMute`, and push stereo int16 to `audio_out`
  (guest rate, capture-sink deterministic).  End-of-window emission
  matters: CycloneBeep re-points `sndOutBase` just after the phase
  wraps, and a start-of-window snapshot replays a stale half.
- **Input** (`pSndInEn`): fill `sndInBase + half·sndSize·4` from the
  `machine.audioin` source (never into ROM/NuBus space), scaled by
  `singerCtl`'s A/D gain fields (`pLeftGain`/`pRightGain`, the same
  1.5 dB ladder upwards, 0 to +22.5 dB — the shipped `singerCtlInit`
  selects +7.5 dB and the speech front end's AGC drives the field), and
  with the converter's **noise floor** added: about an LSB, independently
  per channel, deterministic from the checkpointed sample counter.  The
  noise floor is not cosmetic — a run of mathematically exact zeros
  drives Apple's speech front end's envelope normaliser through a
  division by zero, whose infinities permanently saturate the endpoint
  detector's running cepstral mean.
- **Frame interrupt** (`pFrmIntEn`): latch PSC-VIA2 IFR bit 6
  (`PSCSNDFRM`) **and** pulse DSP EXT1 — the same gated tick (B2).
  If the previous EXT1 is still latched unserviced, that is a frame
  overrun: sticky `pdspFrameOvr` + L5 bit 1 (level until the host
  clears the $21C bit — B3).

`singerStat` reads the board-strap presentation `AV_SINGER_STAT`
(BI1/BI3 = input-source code 1 "microphone", BI4 output-port choice,
`pValidData`).  `singerCtl` is a PSC latch; the engine reads the
attenuation/mute fields live.

## `machine.sound`

`sample_rate`, `out_enabled`, `in_enabled`, `frames`, `overruns`,
`match(reference)`, plus the shared `capture` node
(`machine.sound.capture.start/stop`, `frames`, `peak`) — the same
golden-WAV machinery as the ASC machines (`check_sound()` in
tests/integration/lib/mac.script).

## `machine.audioin` — the emulator's first microphone

Host source surface, mirroring `machine.videoin`:

| source | behavior |
|---|---|
| `none` | default — mic absent, input records the converter's noise floor only |
| `tone` | deterministic 600 Hz sawtooth (integer math, pure function of the checkpointed sample counter) |
| `wav`  | a PCM16 WAV loaded via `machine.audioin.load <path>` (prepared offline at the codec rate; playback position checkpointed; `rewind()` restarts) |
| `host` | the platform microphone through the `gs_audio_in_*` seam |

`gain` (percent, default 100) lets bring-up sweep input levels without
re-mastering assets; it is a test-harness knob, applied *before* the
codec's own A/D gain ladder and noise floor.

### The `host` source in the browser

`src/platform/wasm/em_audio_in.c` overrides the seam for the WASM build and
`app/web2/src/state/microphone.svelte.ts` drives it, mirroring the camera
path exactly: a toolbar toggle gated on the `audio_in` capability, and a
`MediaStreamTrack` attached only while the user's toggle AND the guest's
`pSndInEn` both hold, so the browser's recording indicator is lit only while
the guest is genuinely listening.

Samples cross on a lock-free SPSC ring in the shared wasm heap rather than
the camera's latest-wins slot pair: the guest pulls whole 10 ms half-buffers
on the Singer's frame cadence while the browser produces on its own, so the
two rates need a queue.  The browser captures on **wall** time and the guest
consumes on **emulated** time, so the two clocks drift by definition — the
consumer drops its own backlog when it falls behind, bounding latency, and
reports "no source" on an underrun so the engine presents its noise floor
rather than a torn buffer.  `rd` has exactly one writer and `wr` has exactly
one writer; that, and nothing else, is what makes the ring lock-free.

> **The lifecycle notification fires on the GUEST's gate.**  `pSndInEn`
> changing drives `gs_audio_in_state`, exactly as the VDC clock drives
> `gs_video_in_state` — never the `machine.audioin` source setter, which
> merely echoes back the selection the caller just made and re-enters the
> frontend's own stream reconciliation.  That inversion raced the toggle
> against itself and left the control switching itself back off.
>
> **A dead capture path does not sound silent — it sounds like white noise.**
> The guest's recorder gains up hard (rung 6's tone golden plays back at full
> scale), so it amplifies the codec's own dither into full-scale hiss.  "No
> audio" and "wrong audio" are therefore indistinguishable by ear, which is
> why the mic button's tooltip carries the live transport counters — rate,
> samples in, samples out, underruns, overruns.  Check those FIRST: `in`
> climbing with `out` means samples are genuinely flowing and the problem is
> downstream.  The first bug here was an `AudioWorkletNode` built with
> `numberOfOutputs: 0` and never connected onward — a node with no path to
> the destination is not in the rendering graph, so its `process()` never
> runs and not one sample is captured.  Two more of the same family: the
> worklet module must be served as a real file (`addModule()` on a `blob:`
> URL is rejected under the cross-origin isolation SharedArrayBuffer needs,
> and fails *silently* into the deprecated ScriptProcessor), and the stream
> reconciliation must be serialised (its two callers fire within a tick of
> each other, and overlapping runs close each other's AudioContext).
>
> `tests/e2e/web2-specs/av-microphone.spec.ts` covers all of it against
> Chromium's fake capture device — the only thing that can, since none of
> these layers exist outside a browser.

**The capture is deliberately raw.**  `getUserMedia` is asked for
`echoCancellation: false, noiseSuppression: false, autoGainControl: false`.
Those are voice-call processing, and a browser AGC in front of the emulator
is exactly the defect that made recorded speech-recognition assets unusable —
levels 6-12 dB hot with the dynamics flattened, which Apple's recognizer
rejects (`sr-test-audio-assets.md` §2).

**The AudioContext runs at the browser's NATIVE rate**, not the codec's.
Asking for 24 kHz looks tidier and would leave the C side at 1:1, but a real
capture device routed into a context whose `sampleRate` does not match the
track's delivers *silence* — the graph runs, the worklet is pulled, full
quanta arrive, and every sample is ~0.  Chromium's fake device adopts
whatever rate is requested, so an e2e against it passes while every real
microphone fails.  The negotiated rate is published to the C side, which
does the conversion itself.

The C side then applies the PlainTalk microphone's own characteristics, the
same chain the offline asset generator applies, so live audio and recorded
assets reach the guest looking alike:

* a **100 Hz high-pass** — the electret, its preamp and the codec's AC
  coupling do not pass the proximity rumble a laptop or headset mic delivers;
* a **slow sensitivity normaliser** targeting TIL15884's 100-200 mVpp window
  for typical voiced speech.  This models the *fixed* sensitivity of the
  PlainTalk preamp — it cancels the tens of dB of spread between one user's
  microphone and another's, not the dynamics of speech.  It measures voiced
  blocks only, moves over seconds so it neither pumps nor races the guest's
  own AGC (`AnalogGC` → `singerCtl` A/D gain), and holds during silence
  rather than winding up into the noise floor.

…and then, when the rates differ, a **polyphase windowed-sinc resampler**
(48-tap Kaiser, 128 fractional phases, designed when the ratio changes and
carrying its tail across blocks).  Equal rates keep a straight 1:1 copy.
What this filter does is audible, so it is measured rather than asserted —
`micrig` probes it with steady tones:

| 48 kHz → 24 kHz | passband ≤ 8 kHz | 16 kHz folded onto 8 kHz |
|---|---|---|
| box filter (until 2026-08) | −1.25 dB | **−6.0 dB** |
| polyphase | −0.00 dB | **−81.6 dB** |

The stopband was the real defect: with only ~6 dB of rejection, everything a
microphone picks up between 12 and 24 kHz — hiss, fans, sibilance, switching
noise — folded back into the speech band nearly unattenuated.  At 44.1 kHz
the box was worse in a second way, spanning 1 or 2 input samples alternately,
which makes it a *time-varying* filter.  This mattered less while the DSP's
own `int32` defect (errata E16) was destroying recordings anyway; once that
was fixed it was the only crude resampler left in the chain.

Mono is fanned to both channels because that is what the hardware presents:
the PlainTalk plug's middle contact drives the left and right inputs with the
same blended signal.  No noise floor is added here — the codec model already
contributes its own, and a live microphone brings a room with it.

`local/gs-docs/debug-plaintalk/micrig/` exercises the conditioning without a
browser (it found two defects review had missed).  `connected` drives the mic-present sense;
`samples`/`position` expose progress.  The loaded WAV is checkpointed
with the machine; the host mic is checkpoint-ephemeral.

## Tests

`suite-av`: `av-chime` (power-on chime golden, content-identical to the
dossier oracle `boot-chime-24kHz.wav`), `av-audio-in-device` (plug
contract + tone landing in the guest double buffer), plus the DSP rows
that ride the frame tick (`av-dsp-boot`, `av-dsp-determinism`).

The guest-driven sound rows exercise the same engine from above:
`av-beep` (SysBeep through the sdev/DSP output path), `av-tts`
(TeachText Speak All), and `av-sound-in` — the Sound cdev's "Add…"
recorder capturing the `tone` source through `.AppleSoundInput` and
playing it back out, i.e. input and output in one round trip.  The
Sound Manager's record path applies AGC, so what comes back is the
600 Hz sawtooth driven to full scale.
