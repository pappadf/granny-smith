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
| `host` | the platform microphone through the `gs_audio_in_*` seam (weak defaults model "no mic"; a browser getUserMedia override can trail — the seam is the whole contract) |

`gain` (percent, default 100) lets bring-up sweep input levels without
re-mastering assets; it is a test-harness knob, applied *before* the
codec's own A/D gain ladder and noise floor.  `connected` drives the mic-present sense;
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
