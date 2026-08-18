# AWACS + sound engine — the PDM sound path

`src/machines/pdm/awacs.c`.  The sound block AMIC decodes at `$50F14000`
($20 byte registers) plus the AWACS codec behind its command port.  The
codec is a dumb ITT ASCO 2300-family stereo converter; everything
software-visible — the register file, the double-buffered DMA engine, the
completion flags and interrupts — is AMIC's.  Sources: the shipping ROM's
boot beep, AWACS `sdev` component and `.AppleSoundInput` driver
(disassembly), the ITT ASCO 2300 datasheet, and the Developer Note
pp. 46–48.  The AV family's Singer (`av/singer.c`, `singer.md`) is the
non-expanded face of the same codec spec and the structural template for
the engine.

## Register file (byte registers, offsets from `$50F14000`)

| Off | Function |
|---|---|
| `+$00` | codec command handshake: software loads `+$01`/`+$02`, strobes `$C0`; **BUSY (bit 7) clears within the same access** (the serial link is fast at emulated-time scale, and the ROM spins on it with a ~170k-iteration timeout per command otherwise).  `$40` = idle/cancel. |
| `+$01/+$02` | 16-bit command, big-endian: `reg<<12 \| data12`.  Registers are write-only on hardware; the model keeps shadows. |
| `+$06` | codec status; bit 3 = headphone connected — **always 0** (no jack modeled), so the speaker path stays live. |
| `+$08/+$09` | half-buffer length in frames (system software always writes `$0400` = 1024). |
| `+$0C…+$0E` | 24-bit output position: window offset of the current fetch pointer, 64-byte granularity, advancing in real time while RUN is set; **0 when stopped** (the boot beep drains by polling `(value & $3FFC0) == 0`). |
| `+$10` | bit 0 = output RUN; bits 2:1 = the codec rate for both directions (`%10` = 44 100, else 22 050 — exactly the drivers' read-back predicate). |
| `+$11` | input control (RUN bit 7, subframe select bits 5:2) — register storage only; the input datapath is a later phase. |
| `+$14` | input flags/enables (mirror layout of `+$18`); storage only. |
| `+$18` | output flags (W1C, never read-to-clear): bit 6 = half 0 (`window+$10000`) complete, bit 7 = half 1 (`+$12000`), bit 5 = underrun/stopped; enables in bits 3:1 gate flags 7:5 with a shift of 4. |

## Codec registers (behind the command port)

Only 0/1/2/4 are ever addressed: input mux/gain (0), mutes/loopthru (1 —
bit 7 speaker mute honored), headphone attenuation (2), speaker
attenuation (4 — bits 9:6 left, 3:0 right, 0 = loudest, −1.5 dB/step, the
same ladder as Singer's; the chime volume law `(7−vol)×2` rides on it).

## The output datapath

One 256 KB-aligned physical DMA window (base bytes at `$50F31000/1`) with
fixed half-buffer regions at `+$10000`/`+$12000`.  While RUN is set, a
scheduler event fires per half-buffer period; at each completion the model
**renders the half that just finished** — 16-bit big-endian interleaved
stereo read through the identity page table (the 7100/8100 fixed bank
windows are not host-identity, so no raw RAM-offset shortcut), speaker
attenuation and mute applied — into the shared host stream
(`audio_out.h`), then raises the half's flag (or ERR if the previous flag
is still pending), flips, and re-arms.  Clearing RUN raises the
stopped/underrun flag (bit 5), matching the ROM's stop sequences which
ack it after stopping.

Everything is deterministic (a pure function of guest RAM and the event
cadence), so `pdm-rom-ladder` matches the whole boot chime against a
golden WAV sample-exactly.

## Object surface

`machine.sound`: `sample_rate`, `out_enabled`, `frames` (output halves
rendered), `peak` (loudest |sample| since power-on), `match` (golden-WAV
compare) — plus the shared `machine.sound.capture` sink.  The host stream
opens at 44 100 Hz (the AWACS master-clock family rate), so a capture
armed before the chime stays valid for golden matching.
