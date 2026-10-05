# AWACS — the PDM sound codec

**Contents:**

1. [Overview](#1-overview) — what the part is, the name, the division of labour with AMIC,
   which machines carry it, clocking, the codec family
2. [Register file](#2-register-file) — the sound block at `$50F14000`, the codec command port,
   status, DMA control and status, the codec registers 0–7 behind the port, reset state
3. [Behaviour](#3-behaviour) — the TDM frame bus, the serial link and its taps, the DMA engine,
   sample rates, interrupts, bus arbitration, the analog path, command timing
4. [Programming model](#4-programming-model) — cold-boot init, the boot beep, the Sound Manager
   output component, the sound input driver, jack sense, volume laws, what the system publishes
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The AWACS is the audio codec of the first-generation Power Macintosh platform, Apple codename
PDM. Apple's developer note names the part **AWAC** and expands it as the "audio waveform
amplifier and converter": a 44-pin chip "that combines a waveform amplifier with a 16-bit
digital sound encoder and decoder (codec)", conforming to the IT&T *ASCO 2300 Audio-Stereo
Codec Specification* [1] §"AWAC Sound Chip", p. 17, and Glossary. The ROM's own Sound Manager
component spells the family name **AWACS** (its output component's MacsBug name is
`AWACSHARDWARE`) [3], and this page uses that spelling for the part throughout.

What the chip physically contains is deliberately little. On PDM the codec is **dumb**: it
holds the A/D and D/A converters, the analog input multiplexer, the programmable input gains,
two D/A attenuator pairs, the output mutes, and the sense inputs — and it speaks a
time-division-multiplexed serial frame protocol on four wires. Everything else the software
sees — the register file, the serial frame engine, the sound DMA, and the sound interrupts —
lives in the **AMIC** (Apple Memory-mapped I/O Controller, U14; the 8100 schematic's sheet
title spells the acronym out as "Apple Miscellaneous Interface Chip") [1] pp. 15–16, [4]
sheet 12. The developer note assigns AMIC "DMA for sound I/O" among its functions [1]
p. 16, and documents the codec's multiplexed transfer "between the PDS connector and the AMIC
for DMA transfers to and from RAM memory" [1] p. 46.

The complete sound path:

```
RAM (256 KB-aligned DMA window, double-buffered per direction)
   ↕  AMIC DMA engine — 4 KB half-buffers, completion flags, level-4 interrupts
AMIC serial frame engine — 256-bit frames, four 64-bit subframes
   ↕  4-wire serial link (SND_CLK / SND_SYNC / SND_OUT / SND_IN)
AWACS codec (U12) — ADC/DAC, mux, gains, attenuators, mutes, sense
   ↕  analog
mic jack / CD audio header / GeoPort telecom audio     (inputs A/B/C)
headphone jack / mono speaker amp / GeoPort telecom audio (outputs A/B/C)
```

All four serial wires are also routed to the PDS connector (pins 70/71/72/163) [1]
Table 3-16, from where the AV card taps subframes 2–4 for its DSP3210 and the DAV connector
re-exposes them to NuBus cards [1] pp. 46–47, 53–54. The Mac's own system sound always flows
through subframe 1 [1] p. 47.

There is **no Apple Sound Chip (ASC)** on this platform and no PSC: the platform's published
device table deliberately points the classic ASC base address at the ROM (`$40800000`) and
sets the ASC-present flag anyway, so that pre-Power Macintosh software which pokes an ASC
ends up harmlessly reading ROM [3] (*observed*). See §4.7.

### 1.2 Machines that carry it

| Machine | Notes | Sound hardware |
|---|---|---|
| Power Macintosh 6100/60, 6100/60AV | [1] | AWACS + AMIC engine; AV models identical, AV card taps subframes 2–4 |
| Power Macintosh 7100/66, 7100/66AV | [1] | same |
| Power Macintosh 8100/80, 8100/80AV | [1] | same; DAV usable only on the AV models of this generation (see §3.2) |
| Power Macintosh 6100/66 | [2] | no sound changes documented; clock-chip changes "transparent to software" [2] p. 5 |
| Power Macintosh 7100/80 | [2] | same |
| Power Macintosh 8100/100, 8100/110 | [2] | same (8100/110: 110 MHz CPU, 36.6667 MHz bus — sound path untouched) |

All models provide 16-bit digital stereo sound I/O at sample rates up to 44.1 kHz, and system
sound can be mixed with CD-ROM sound in analog form [1] §"Features", p. 8. The sound system
"achieves simultaneous 16-bit broadcast-quality stereo sound input and output, using four
8 KB buffers, and supports Apple's speech synthesis and recognition software" [1] p. 46.
The pin-level wiring facts in this page come from the 8100 board schematic [4]; the 6100 and
7100 boards are not in the evidence set and their sound wiring is *inferred* to match
(*unverified*).

### 1.3 Division of labour: codec, AMIC, software

The 0x20-byte register window the CPU touches at `$50F14000` — the platform decoder table
names the address `AwacsAddr` [3] — is **AMIC's**, not the codec's: the serial codec has no
parallel bus of its own. Writes to the window are serialized by AMIC over the auxiliary bits
of the frame bus to the codec; reads of the window return AMIC-maintained DMA state and a
latched image of the codec's status field [3][4] (*observed* in ROM code and schematic
wiring). The split, and the reason a "sound subsystem" page must cover both halves:

- **AWACS** — conversion, analog signal conditioning, gain/attenuation ladders, mutes,
  loop-through monitoring, jack sensing. Programmed exclusively through a 16-bit command
  port (§2.2); its registers are write-only (§2.10).
- **AMIC** — the frame-clock generator, the serial frame engine, the double-buffered DMA
  over fixed offsets in one relocatable 256 KB window, the buffer-length and rate controls,
  the completion/error flags and the level-4 interrupt path [1] pp. 15–16, 20; [3]; [4]
  sheet 12. The AMIC-wide view of the window base, the interrupt machinery and the DMA flag
  registers is given on the [AMIC page](amic.md); this page keeps the sound-specific detail.

System software never programs the sound hardware from generic Mac OS code: the 68k ROM's
per-machine sound-control vector table for this platform is a set of no-op stubs — set
volume, enable/disable/clear sound interrupts, input select all return immediately [3]
(*observed*). Unlike ASC-era machines, all real programming is done by two ROM-resident
hardware drivers: the Sound Manager's AWACS output component and the `.AppleSoundInput`
input driver (§4.3, §4.4).

### 1.4 Clocking

The sound master clock is a dedicated oscillator, **G3 = 45.1584 MHz**, feeding AMIC's C45M
input; AMIC derives the serial bit clock from it [4] sheets 11–12. The developer note's
fixed-clock table prints the sound oscillator as 44.1584 MHz [1] Table 2-3, p. 20 — a
misprint: the schematic value is 45.1584 MHz, and only 45.1584 MHz divides to exactly
44.1 kHz (45.1584 MHz ÷ 1024 = 44.1 kHz) [4] sheet 11 (*observed* in the oscillator
annotation). The serial bit clock at the PDS, `AwacClk`, is **256 × the sample rate** [1]
Table 3-16, p. 47 — 11.2896 MHz at 44.1 kHz. The ASCO 2300 specification, for its part,
defines the codec to *sense* the ratio between its master clock and the frame sync and
adjust its conversion rate automatically [5] §2.7; which ratios the PDM wiring presents to
the codec beyond the documented bit clock and the observed software rate select is not
established (§6).

### 1.5 The codec family

The AWACS is the **"expanded command set" part the ASCO 2300 data sheet promises**. The base
specification defines the codec's control plane as 24 auxiliary bits per subframe per
direction — a mute, input mux, gain and attenuation cells (§3.1) — and reserves the first
cell, the Expand bit, for "an expanded command set in the future": if the bit is ever set,
the plain codec flags error 1 and ignores the other control cells [5] §3.1, aux cell 1.
Three facts fix AWACS as that expansion, rather than a plain ASCO part [3][5][7]:

1. Its command port takes **16-bit `register << 12 | data12` commands** — an addressed
   register file, which the plain 24-bit cell layout cannot express; the same low-16-bit
   encoding survives into the later Grand Central machines' codec control register [7].
2. The ROM's helper that waits for the command port to drain is MacsBug-named
   **`WaitExpandClear`** [3].
3. Its capability set — two independent output attenuator pairs (speaker and headphone),
   per-output mutes, and register addressing — exceeds the plain cell set, which has only
   one shared attenuator pair and one mute [5] §3.1.

The plain-cell face of the same specification is the Singer codec of the AV Quadras, whose
24-bit control image maps one-to-one onto the ASCO cells ([singer page](../av/singer.md));
AWACS keeps the plain cell meanings *inside* its register 0 (mux and gains ≈ cells 3–12) and
registers 2/4 (attenuation ≈ cells 13–20) [3][5][7] (*inferred from the bit correspondence;
no document states the lineage explicitly*). The later **Screamer** part (codec revision 3
and up) extends the same command set with registers 5–7; the original AWACS reports codec
revision **2** in the status field per the later drivers' probes [7], and a PDM machine must
not be mistaken for a Screamer. Where — if anywhere — the revision value surfaces in the
PDM byte register file is unverified; the PDM ROM never reads it (§6) [3].

## 2. Register file

### 2.1 The sound block at `$50F14000`

The window is 0x20 bytes wide and byte-oriented; 16- and 24-bit quantities are composed
from consecutive byte addresses, **big-endian**. The map below is established by the ROM's
own code paths [3] and corroborated by the released MkLinux platform driver [6]; offsets
not listed were never observed touched by any software in the evidence set.

| Address | Register | Summary |
|---|---|---|
| `+$00` | Codec command control/handshake | bit 7 = BUSY/GO strobe; bit 6 = command hold; §2.2 |
| `+$01` | Codec command, high byte | bits 7:4 = codec register number; bits 3:0 = data[11:8] |
| `+$02` | Codec command, low byte | data[7:0] |
| `+$04`–`+$06` | Codec status, 24-bit big-endian, read | jack/mic sense, manufacturer, revision, error/overflow; §2.3 |
| `+$08`/`+$09` | Half-buffer length, high/low byte | length in frames (4-byte stereo units), shared by both directions; §2.4 |
| `+$0C`–`+$0E` | Sound-out position, 24-bit big-endian, read | current output pointer/counter; §2.5 |
| `+$10` | Sound control (output + shared rate) | bit 0 = output RUN; bits 2:1 = sample rate for the whole codec; §2.6 |
| `+$11` | Sound-in control | bit 7 = input RUN; bits 5:2 = subframe field; §2.7 |
| `+$14` | Sound-in status/interrupt flags | half-buffer completion flags (W1C) + interrupt enables; §2.8 |
| `+$18` | Sound-out status/interrupt flags | half-buffer completion + underrun flags (W1C) + interrupt enables; §2.8 |

Two registers the sound engine depends on live outside the block, in AMIC's DMA-wide
register file (see the [AMIC page](amic.md)): the DMA window base bytes at `$50F31000`
(bits 31:24) and `$50F31001` (bits 23:16) — the low 18 bits of the base are ignored, so the
window is 256 KB-aligned — and the DMA interrupt flag registers at `$50F2A008`/`$50F2A00A`
(§3.5) [3][6].

### 2.2 The codec command port (`+$00`–`+$02`)

The codec's only programming interface is a three-byte command channel. Software loads the
command, then strobes:

| Step | Write | Meaning |
|---|---|---|
| 1 | `+$00 ← $40` | Idle/prepare: bit 6 set, bit 7 clear. Performed before loading each new command; also written on timeout to cancel. |
| 2 | `+$01 ← hi`, `+$02 ← lo` | The 16-bit big-endian command word `register << 12 \| data12`. |
| 3 | `+$00 ← $C0` | GO: bits 7 and 6 set — latch the command and clock it out to the codec. |
| 4 | poll `+$00` bit 7 | Hardware clears bit 7 when the command has been serialized to the codec. Software spins here with a timeout; on timeout it writes `$40` to cancel. |

All of this is *observed* in every ROM code path that programs the codec — the boot beep,
the Sound Manager component, the input driver — each of which wraps its writes in exactly
this `$40` / `hi` / `lo` / `$C0` / poll sequence [3]. Bit 6 is written 1 at all times
(`$40` idle, `$C0` go); bits 5:0 are never set by any observed software. Read, bit 7 doubles
as the flag that distinguishes this expanded command interface from the plain-cell codec
protocol — the property later system software keys on [6]. Whether bit 6 has hardware
meaning or is simply part of the ROM's constant is not established (§6).

The busy handshake is genuinely slow: a codec command is clocked out serially over the
frame's auxiliary bits, and the ROM budgets for it — `WaitExpandClear` polls bit 7 for up to
80,000 iterations with a VIA read as pacing before every command, and the boot beep runs a
roughly 167,000-iteration timeout loop after each `$C0` strobe [3] (*observed*). The port
must eventually clear bit 7 on its own — the ROM spins on it.

### 2.3 Codec status (`+$04`–`+$06`)

A 24-bit big-endian read-only window on the codec's status reporting. What the ROM actually
uses is exactly one bit: **`+$06` bit 3 = headphone connected** — read by the boot beep, by
the output component's mute decision, and by the input driver's jack-poll task [3]
(*observed*). Nothing else in the window is ever read by ROM code.

The wider layout is established by the MkLinux platform driver, which ran against this
hardware [6], and is consistent with the ASCO 2300 status-aux field [5]:

- `+$06` (bits 7:0 of the 24-bit value) — the four input-sense lines: bit 3 = headphone
  connected, bit 1 = line-in sense, bit 0 = **not**-microphone-present (active low); bit 2 is
  not assigned on PDM (the headphone jack *is* the line out). These mirror the ASCO status
  field's sense cells BI1–BI4 [5] (*inferred from the correspondence; the PDM wiring of the
  four sense pins is only partly traced*, see §3.7 and §6).
- `+$05` low nibble (`$0F00` of the 16-bit view) — manufacturer ID.
- `+$05` high nibble (`$F000`) — codec revision. The original AWACS part reports **2**;
  revision ≥ 3 is the later Screamer [6][7]. The PDM ROM never reads it [3].
- `+$04` (bits 23:16) — the error-number, A/D overflow (left/right) and valid-data flags of
  the ASCO status field [5][6].

The manufacturer-ID value on PDM hardware is unverified (§6).

### 2.4 Half-buffer length (`+$08`/`+$09`)

A 16-bit big-endian length in **frames** — one frame being one 4-byte stereo unit — shared
by both directions: the input driver saves and rewrites the same register the output paths
program [3]. System software always writes `$0400` — 1024 frames, 4096 bytes per
half-buffer [3]; MkLinux programs `$07FF` [6]. The developer note's "four 8 KB buffers"
[1] p. 46 describes the underlying fixed regions (each half-buffer slot holds 2048 frames);
Apple's software uses half of each slot (§3.3) [3][6].

### 2.5 Sound-out position (`+$0C`–`+$0E`)

A 24-bit big-endian read-only value exposing the output engine's progress. The boot beep
drains the pipe by polling it until `(value & $3FFC0) == 0` — i.e. until the masked 18-bit
field reads zero, 64-byte granularity [3] (*observed*). The value must advance in real time
while the engine runs: the beep has no interrupt help, and a frozen counter hangs the ROM
right there [3]. Two readings of the field exist in the evidence: a **byte offset within
the 256 KB DMA window**, at 64-byte granularity, parked at zero when the engine is stopped
[3] — the reading the drain predicate is stated in — and, from later system software, a
**phase counter within the current buffer with intra-frame clock bits packed into the low
byte** (`+$0C` = 6-bit clock count | (phase & 3) << 6, `+$0D` = phase >> 2, `+$0E` =
phase >> 10, wrapping at the programmed buffer length) [6]. Both satisfy the same drain
predicate; which is the literal hardware semantic is open (§6). No input-side position
counter was ever read by any software in the evidence set (§6).

### 2.6 Sound control (`+$10`)

One byte carrying the output run bit and the sample-rate select for the **whole codec**:

| Bits | Meaning |
|---|---|
| 0 | **Output RUN** — output DMA and serial output enable. Set to start playback, cleared to stop; the AMIC init code stops output with `BCLR #0` [3]. |
| 2:1 | **Sample rate, both directions** — `%10` (byte value `$04`) = 44 100 Hz, `%00` = 22 050 Hz; `%01` = 29 400 Hz per the MkLinux driver [6]. See §3.4. |
| 7:3 | Never set by any observed software; meaning unknown (§6). |

That the rate lives in the *output* control register yet governs both directions is
*observed* from three independent behaviors: the output component writes the rate field as
`(old & $F9) | $04` for 44.1 kHz and `old & $F9` otherwise; the input driver *reads* the
rate back as `(+$10 & $FE) == $04 ? 44100 : 22050` and reprograms it with
`+$10 = (cur & 1) | desired` — preserving the run bit but owning the rate; and the shared
buffer-length register behaves the same way [3]. The boot beep leaves the byte at `$04`
(44.1 kHz, stopped) after running [3].

### 2.7 Sound-in control (`+$11`)

| Bits | Meaning |
|---|---|
| 7 | **Input RUN** — set last, after the rate/length/flags are programmed; the AMIC init code stops input with `BCLR #7` [3]. |
| 5:2 | A four-bit field software insists must be non-zero, always written `%0001` (bit 2): the output component writes `(old & $C3) | $04`, the input driver sets bit 2 iff `(old & $3C) == 0` [3]. Best interpretation: **input subframe select, one-hot, subframe 1** — matching the documented TDM layout that reserves subframe 1 for system sound and the subframe-select fields of the later codec control registers (*inferred — unverified*; no observed code ever writes any other value). |
| 1:0 | Cleared by the input driver before starting (`&= $FC`, then an OR of a constant that is 0 in the shipped ROM); function unknown [3]. §6. |

### 2.8 Sound-in (`+$14`) and sound-out (`+$18`) DMA status

Mirror-image register pair, one per direction. Flag bits live in the high nibble and are
**write-1-to-clear**; interrupt-enable bits live in the low nibble and are written
directly — the enable pattern gates the flags with a shift of 4 (`(flags >> 4) & enables`,
§3.5) [3][6]:

| Bit | `+$14` (sound in) | `+$18` (sound out) |
|---|---|---|
| 7 | input half-buffer **1** (window +`$E000`) complete, W1C | output half-buffer **1** (window +`$12000`) complete, W1C |
| 6 | input half-buffer **0** (window +`$C000`) complete, W1C | output half-buffer **0** (window +`$10000`) complete, W1C |
| 5 | unobserved (§6) | **underrun/stopped** flag, W1C |
| 4 | unobserved | — (does not exist on the output side) |
| 3:1 | interrupt enables (input start: write `$0C`; stop: write 0) | interrupt enables (output start: write `$0E`; stop: clear bits 3:1) |
| 0 | unobserved | unobserved |

Observed usage [3]: the output start sequence acks everything (`|= $C0`), acks the error
flag (`|= $20`), then enables all three sources (`|= $0E`); the output stop sequence clears
the enables (`&= $F1`), clears RUN, then acks bit 5 again — implying **the engine raises
bit 5 when it stops**. The input start clears all flags with `$F0`, then enables the two
buffer interrupts with `$0C`; the input stop writes 0. The interrupt handlers ack by
read-modify-write, OR-ing the flag bit back into the register; whether a write-back that
carries the *other* pending flag as 1 clears it too is open (§5, §6).

### 2.9 The DMA window and the buffer map

Sound DMA addresses memory as fixed offsets inside AMIC's single relocatable window — the
base bytes at `$50F31000/1` ignore the low 18 bits, so the window is 256 KB-aligned, and
system software points it at the top of physical RAM, where the nanokernel maps 160 KB of it
into logical address `$61000000` [3][6] (*observed*; the window mechanics are described on
the [AMIC page](amic.md)). The sound regions are fixed in hardware:

| Window offset | Contents |
|---|---|
| `+$C000` / `+$E000` | Sound **input** half-buffers 0 / 1 (8 KB slots) |
| `+$10000` / `+$12000` | Sound **output** half-buffers 0 / 1 (8 KB slots) |
| `+$14000` / `+$14800` | Ethernet transmit buffers, set 0 / set 1 — adjacent to, not overlapping, the sound regions [3] |

Apple's software only ever uses the first 4 KB of each 8 KB slot — 1024-frame halves [3];
the 160 KB reservation also covers the Ethernet and SCC regions outside this page's scope.

### 2.10 Codec registers 0–7 behind the command port

The 16-bit command word (`+$01/+$02`) addresses a file of twelve-bit registers in the
codec. The registers are **write-only**: the ROM keeps 16-bit shadows (register number
included) in driver globals and only issues a command when a shadow changes [3]
(*observed*). Four registers are used on PDM:

| Register | Function | Bits (as used on PDM) |
|---|---|---|
| 0 | Input mux and gain | bits 3:0 right A/D gain; bits 7:4 left A/D gain; bit 8 mic-preamp bypass; bits 11:9 input mux, one-hot |
| 1 | Mutes and loop-through | bit 7 speaker mute; bit 9 headphone mute; bit 6 loop-through |
| 2 | Headphone attenuation | bits 3:0 right, bits 9:6 left — 4-bit attenuation ladder |
| 4 | Speaker attenuation | same layout as register 2 |
| 3 | — | never used by any observed software |
| 5, 6, 7 | — | Screamer-only extensions of the later codec family; not present on PDM [7] |

**Register 0 — input mux and gain.** The gain nibbles run 0 = 0 dB … 15 = +22.5 dB in
1.5 dB steps [5] aux cells 5–12. Bit 8 is the mic-preamp bypass line of the later codec
family (`GAINLINE`, [7]); the PDM input driver toggles it on one control call — set for
parameter 0, cleared otherwise [3] (*observed*). The mux field bits 11:9 is one-hot,
cleared with `& $F1FF` before each selection: **bit 10 = PlainTalk microphone (source 1),
bit 9 = internal CD audio (source 2), bit 11 = GeoPort telecom audio (source 3)** [3]
(*observed* in the driver's source-select call; the bit 10/11 assignment agrees with the
later Linux codec model, though the later drivers themselves swap mic/line between part
revisions [7] — the PDM ROM is authoritative here). Driver default at open: `$049` —
microphone selected, both gains +13.5 dB [3]; mic selection forces gains `$99`, CD and
telecom selection force gains 0 [3].

**Register 1 — mutes and loop-through.** Bit 7 (`$0080`) = **speaker mute**; bit 9
(`$0200`) = **headphone mute**; bit 6 (`$0040`) = **loop-through** (input monitored to the
outputs — the driver's play-through switch sets/clears it, and its status getter returns
`(reg1 >> 6) & 1`) [3]. The later codec family defines three more fields in this register —
bit 2 recalibrate, bits 5:3 a sample-rate mirror, bits 11:10 parallel outputs [7] — **none
of which any PDM code path ever touches**: there is no recalibration sequence on PDM (the
rate lives in `+$10`), and the ROM writes reg 1 = `$000` at input-driver open [3].

**Registers 2 and 4 — output attenuation.** Identical 4-bit inverted ladders per channel
(register 2 drives the headphone/line output, register 4 the speaker path): **0 = loudest,
15 = most attenuated**, 1.5 dB per step, maximum −22.5 dB [5] aux cells 13–20. Note that
maximum attenuation is *not* silence — that is what the register-1 mutes are for; muting by
ramping attenuation first is the codec-documented click-avoidance procedure [5] §2.4.

**Volume laws.** The Sound Manager output component maps each channel byte `v` of the
32-bit Sound Manager volume to `att = (v >= $100) ? 3 : 15 - (v*12) >> 8` — full volume
maps to attenuation **3**, not 0 (headroom), silence to 15 — and folds the average of both
channels into the classic 3-bit `SdVolume` at low-memory `$0208` and into PRAM `$01F8`
[3]. The boot beep uses a different law, identical to the AV Quadras' startup chime:
`att = (7 − PRAM volume) × 2` duplicated into both channels — 3 dB per Control-Panel step,
volume 0 = −21 dB, explicitly *not* silence [3]. The PRAM volume itself: XPRAM location
`$10` must read `$A8` (validity) or the volume defaults to 5; otherwise volume = XPRAM
`$08 & 7` [3].

**Output component defaults** (`GetInitialPreferences`, "AWACS Output Prefs" preferences
resource) [3]: preferences version 2; **default output rate 22 050 Hz** (Fixed
`$56220000`); default speaker and headphone volumes both `$006E006E` (≈ 43 % — attenuation
code 10 per channel); automatic speaker-mute enabled; and a lookup for external
`'adio'`-family components of subtypes `'telc'`/`'telh'` (the telecom-audio path of the
AudioVision monitor) which, when both exist, takes over headphone dispatch [3].

### 2.11 Reset state

No document states the power-on values of the register file. What the ROM's behavior
constrains [3] (*all observed*):

- Both run bits are **clear** at power-on: the init code clears them (`BCLR #0, +$10`;
  `BCLR #7, +$11`) rather than setting them, as a defensive stop of a state it does not
  trust — and everything downstream works if the engine starts idle.
- The command port is idle with a codec that will accept `$40`-prepared commands; the
  very first thing the boot beep does to the sound block is program flags and rate, then
  issue codec commands.
- The buffer-length and rate fields hold *something* the beep overwrites before starting
  (it writes `$08`/`$09` = `$04`/`$00` and reads/patches `+$10`); no code path reads them
  expecting a specific reset value.
- The status register answers reads from the start: the boot beep reads `+$06` bit 3
  before it has written any configuration.

A pseudo-VIA1 port B line the platform tables name `vSndEnb` initializes asserted and
releases the sound subsystem from reset during startup [3] (*observed* in the
initialization tables; see the [AMIC page](amic.md)). Whether a CPU reset without power
cycle returns the engine to this state, or only the initial release from `vSndEnb` does,
is not established (§6).

## 3. Behaviour

### 3.1 The TDM frame bus

The developer note specifies the serial format the codec and AMIC share [1] pp. 47–48,
Figures 3-12, 3-13, 3-14, Table 3-16:

- A **frame is 256 bits** — four **subframes of 64 bits** each. Each subframe carries two
  32-bit audio samples, one left and one right; each 32-bit sample is **20 data bits plus
  12 auxiliary bits**.
- **Subframe 1 is reserved for Macintosh system sound I/O; subframes 2–4 are available for
  applications and expansion cards.**
- `AwacClk` — the bit clock, 256 × the sample rate — also clocks `AwacSync`. Data is
  clocked out on the clock's **falling edge** and sampled on its **rising edge**. A frame
  sync is a pulse **two AwacClk cycles wide**; a word sync is a pulse **one cycle wide**,
  at each 32-bit sample boundary.
- Signal timing at the PDS: minimum setup 10 ns, minimum hold 8 ns, maximum load 20 pF
  [1] Table 3-16.

The 12 auxiliary bits per 32-bit sample come in two six-bit halves whose concatenation
across the subframe's two samples forms the **24-bit control field (host to codec)** and
the **24-bit status field (codec to host)** of the ASCO 2300 specification [5] §3.1. The
plain control field is laid out as: cell 1 Expand, cell 2 mute, cells 3–4 input mux L/R,
cells 5–8 left input gain, cells 9–12 right input gain, cells 13–16 left D/A attenuation,
cells 17–20 right D/A attenuation, cells 21–24 the four output port pins BO1–BO4; the
status field as: Expand, A/D valid, A/D overflow L/R, a 4-bit error number, a 4-bit
revision, eight reserved bits, and the four input-sense lines BI1–BI4 [5]. This is the
plane over which AWACS commands travel — the expanded command set rides the same aux bits
(§1.5, §3.8).

The codec drives its serial data output only during the subframe it is assigned to and
tri-states it during the others [5] §3.1 — the property that makes the four-subframe TDM
share work. The Mac's own samples are 16-bit, left-justified inside the 20-bit field with
trailing zeros — the part is a 16-bit converter [5] §1, and the DMA format in RAM is
16-bit big-endian interleaved stereo [3].

### 3.2 The serial link and its taps

Board wiring (8100 schematic, sheets 10/12/19) [4]: AMIC drives the codec over
`SND_OUT → AwacsSDataIn`, `SND_CLK → AwacsSClk` (through 51 Ω), and `SND_SYNC → AwacsSync`;
the codec returns `SND_IN ← AwacsSDataOut`. The codec's remaining digital connections are
the two-bit select strap `AwacsSel` (SEL_0/SEL_1), system reset (active low) and TEST
[4] sheet 19. What the select strap programs — plausibly the codec's subframe assignment —
is not documented (§6).

All four serial nets also run to the PDS connector J14 [4] sheets 10/12, and the developer
note documents them there as `AwacClk` (pin 70), `AwacSync` (pin 71), `AwacDataOut`
(pin 72, AMIC to PDS) and `AwacDataIn` (pin 163, PDS to AMIC) [1] Table 3-16. Two consumers
tap them outside the main logic board:

- **The AV card** (6100/60AV, 7100/66AV, 8100/80AV): its DSP3210 taps the frame bus through
  the PDS and works subframes 2–4, while system sound keeps flowing through subframe 1 and
  AMIC [1] pp. 41–48; [3]. Sound hardware on AV and non-AV models is otherwise identical.
- **The DAV interface** on the AV card: a 60-pin connector (AMP 104549-8) that carries the
  unscaled 4:2:2 YUV video input plus the digital audio — `AwacSync`, `AwacSerOut`,
  `AwacSerIn`, `AwacBitClk` — from NuBus cards attached by a short flat ribbon cable;
  usable on the Power Macintosh 7100/66AV and 8100/80AV (the 6100/60AV has no free slot)
  [1] §"DAV Interface", pp. 53–54, Table 4-4.

### 3.3 The sound DMA engine

Each direction is **double-buffered over its two fixed regions** (§2.9): the engine
plays/fills half-buffer 0, raises that half's completion flag (and, if enabled, its
interrupt), moves to half-buffer 1, and wraps [3][6]. The shared length register
(`+$08`/`+$09`) sets the half length in frames; Apple's drivers always use $0400 frames —
1024 stereo 16-bit frames, 4 KB per half — giving a completion every 1024/44100 ≈
**23.2 ms at 44.1 kHz** (≈ 46.4 ms at 22.05 kHz) [3] (*observed*). The data format in RAM
is one 32-bit frame per sample period: 16-bit big-endian left | 16-bit big-endian right,
interleaved [3][6].

The underrun contract: the output engine raises the **underrun/stopped flag** (`+$18`
bit 5) when it stops having been running — the output stop sequence acks bit 5 *after*
clearing RUN [3] — and the output component's interrupt handler treats bit 5 as "restart":
it acks it and re-sets the RUN bit, without refilling [3]. Later system software describes
the symmetric condition — a buffer completion arriving while the previous flag is still
unacked — as the overrun/error indication, and notes the hardware "may miss a final
interrupt" when stopping, which is why its driver clears the run bit as soon as its queue
empties [6].

While running, the engine's **position counter** (`+$0C`–`+$0E`) advances with time; when
stopped it reads zero — the drain predicate of §2.5 is satisfied by the stop itself [3]
(*observed*; the counter is what the boot beep polls, so it must move).

### 3.4 Sample rates

The rate field (`+$10` bits 2:1) is the codec-wide rate select. Two values are *observed*
in Apple's own software: `%10` = 44 100 Hz and `%00` = 22 050 Hz — the output component
writes one of exactly those two, and the input driver's rate getter returns 44 100.0 only
for `%10`, else 22 050.0 [3]. The MkLinux platform driver documents a third, `%01` =
29 400 Hz [6]; the clock arithmetic is a divide of the 45.1584 MHz master by 2 and 64,
then by {16, 12, 8} for {22 050, 29 400, 44 100} Hz [6]. Whether the `%11` code or any
other rate exists on PDM hardware is unverified — the developer note caps the system at
"sample rates up to 44.1 kHz" [1] p. 8, and no Apple code path exercises `%11` (§6). The
22.05 kHz mode's mechanism (a halved frame rate versus sample repetition) is not observable
through the register interface (§6); the ASCO base part, for comparison, adjusts its
conversion rate by sensing the clock-to-sync ratio [5] §2.7.

### 3.5 Interrupts

Sound interrupts arrive at **CPU level 4** through the AMIC's 68k-model interrupt
dispatcher [3] (*observed* in the ROM's level-4 handler; the platform's interrupt
architecture is covered on the [AMIC page](amic.md)). The path:

1. The engine raises a completion (or error) flag in `+$14`/`+$18`, provided the
   corresponding enable bit is set — the gate is `(flags >> 4) & enables` [3][6].
2. AMIC mirrors the direction into its DMA flag registers at `$50F2A00A`: **bit 0 = sound
   in (DMA channel 8), bit 1 = sound out (DMA channel 9)**; the neighboring register
   `$50F2A008` carries channels 7–0 [3][6]. These summary bits behave as combinational ORs
   of the enabled engine flags — the ROM's handlers always ack the *source* in the engine
   register and never write the AMIC flag registers [3].
3. The level-4 dispatcher checks the SCC first (via `$50F2A000` bit 2), then reads the ten
   DMA flag bits as a 16-bit word from `$50F2A00A:$50F2A008` and dispatches
   most-significant-bit first — priority order **SCC > sound out (9) > sound in (8) >
   floppy (6) > Ethernet Tx (5) > Ethernet Rx (4) > SCC Tx A (3) > Rx A (2) > Tx B (1) >
   Rx B (0)** [3].
4. Each sound handler acks its flag by OR-ing the bit back into `+$14`/`+$18` (a
   read-modify-write of a write-1-to-clear register — see §5 for the ambiguity this
   creates) and defers the buffer copy out of the interrupt context [3].

The classic VIA-tied "sound interrupt enable" entries that ASC-era software expects are
vestigial here — the platform's sound runs on the DMA path, and the 68k sound-control
vectors that would program a VIA are no-op stubs [3] (§1.3).

### 3.6 Bus arbitration and DMA bandwidth

Sound DMA is a requestor on the CPU bus, arbitrated by the HMC (see the [HMC
page](hmc.md)). The documented priority order puts **I/O DMA for AWAC above SCSI and SCC
DMA and below SWIM III DMA**, with DRAM refresh and video refresh above all I/O and the
main processor last [1] §"CPU Bus Arbitration", Table 2-4, p. 20. This ordering is why the
boot beep blanks the screen while it plays (§4.2): with video refresh stealing DRAM
bandwidth quieted, the polled refill loop keeps up with the engine.

### 3.7 The analog path

Board wiring and connectors (8100 schematic, sheets 18–19) [4]:

| Path | Wiring |
|---|---|
| **Input A** | PlainTalk microphone jack J1 — `EmiMicInL/R`, `MicPower` (the jack powers the electret preamp), `MicSense*` plug detect into the codec's sense input. The PlainTalk microphone delivers line level [3]. |
| **Input B** | CD-ROM audio header J18 — `CDInL/R` into the codec's analog input. CD audio mixes in analog and can bypass DMA entirely [1] p. 8; [3]. |
| **Input C** | GeoPort/telecom audio `TeleAudioInL/R` from the HDI-45 monitor connector J6 [4] sheet 19. |
| **Output A** | `Line/HPOutL/R` to headphone jack J2, with `HPSense` feeding the status-register headphone bit [4] sheet 19. |
| **Output B** | `SpkrFiltL/R` through a resistive mono mix (R40/R49) into a TDA7052AT mono amplifier (U9) and the internal speaker — **the internal speaker is a mono mix of L+R**, matching the platform's published stereo-mixing feature bit [4] sheet 19; [3]. |
| **Output C** | `TeleAudioOutL/R` back to the monitor/GeoPort connector [4] sheet 19. |

The developer note's electrical specification [1] §"Sound I/O", Table 3-15, p. 46:
Audio In 8 kΩ impedance, 2 V rms maximum, 22.5 dB gain available; Audio Line Out 37 Ω
impedance, 0.9 V rms maximum, attenuated −22.5 dB (crosstalk degrades from −80 dB to
−32 dB when the output is connected to 32 Ω headphones). Bandwidth 20 Hz to 20 kHz ±2 dB;
harmonic distortion plus noise under 0.05 % over the bandwidth with a 1 V rms sine input;
input SNR 82 dB, output SNR 85 dB, with no audible discrete tones [1] p. 46. Software
controls the volume to the built-in speaker and the sound output connector independently
[1] p. 46 — which is exactly the register-1 mutes plus registers 2 and 4.

### 3.8 Command transport timing

A codec command issued at the command port is serialized by AMIC across the auxiliary bits
of subframe 1 of the frame bus — one 24-bit control field per subframe per frame means one
command per frame period at the fastest, about 22.7 µs at 44.1 kHz. That is the physical
reason the busy handshake is slow enough that the ROM wraps every command in a polling
loop with a large timeout (§2.2), and why the drivers keep shadows and only write on
change [3] (*the per-frame serialization is inferred from the documented frame format [1]
[5]; the polling behavior is observed* [3]).

## 4. Programming model

### 4.1 Cold-boot init

The ROM's AMIC initialization runs once the memory map is up; its sound-relevant steps,
in order [3] (*observed*; the AMIC-wide choreography is on the [AMIC page](amic.md)):

1. Soft-reset every AMIC DMA channel (channel control registers: SCSI `$1008`, Ethernet
   Tx `$0C20`, Ethernet Rx `$1028`, floppy `$1068`, SCC `$1088`/`$1098`/`$10A8`/`$10B8`).
   The sound engine has **no** channel control register — it is stopped through its own
   registers instead.
2. `BCLR #0, $50F14010` — stop sound out. `BCLR #7, $50F14011` — stop sound in.
3. Program AMIC's bus-speed field and write the DMA window base to `$50F31000/1`.
4. Build the ten-entry DMA dispatch table in the system heap (`DMADispGlobals`, pointer
   stored in the ExpandMem area the ROM reads at offset `$210`), all vectors initialized to
   `BadIntVector`.

The sound subsystem itself is held in reset until the `vSndEnb` line is released during
startup (§2.11).

### 4.2 The boot beep

The startup chime (ROM StartMgr path, file offsets `$E59B0–$E5CC2`) [3] — a fully polled
sequence, interrupts off:

1. Reset all non-sound DMA channels (the same eight control registers).
2. Set bit 7 of the video mode register at `$50F28000` — **video blanking**, not a sound
   function: the beep blanks the screen while it runs, buying the DRAM bandwidth the
   sound DMA needs (§3.6) [3] (*observed*; see the [AMIC page](amic.md)).
3. `+$18 = $F0`, `+$14 = $F0` (clear all flags), `+$11 = $04`, `+$10 = $04` — both
   directions stopped, 44.1 kHz selected.
4. Read the PRAM volume through Cuda; compute the chime attenuation law (§2.10),
   `att16 = gain << 6 | gain` for both channels.
5. Codec writes, each through the full `$40`/hi/lo/`$C0`/poll sequence: register 4
   (speaker attenuation) = att16; register 2 (headphone attenuation) = att16; then read
   `+$06` bit 3 and write register 1 = `$0080` if headphones are plugged in, `$0000`
   otherwise — **the speaker is muted during the chime iff headphones are present**.
6. Locate the top of physical RAM through a nanokernel call, 256 KB-align it, and write
   the DMA window base.
7. Fill both output half-buffers (window `+$10000`, `+$12000`) with 1024 longwords each of
   chime PCM.
8. `+$08 = $04`, `+$09 = $00` (1024-frame halves); `+$18 = $E0` (ack everything);
   `+$10 |= 1` — RUN.
9. Refill loop: poll `+$18`; bit 7 set → ack with `$80`, refill buffer 1; bit 6 set → ack
   with `$40`, refill buffer 0; interleave until the source data is exhausted, then
   zero-fill the tail, clear `+$10` (stop), and ack with `$C0`.
10. Drain: poll the 24-bit position until its masked value reads zero (§2.5).

The ROM's PowerPC hardware-init carries its own sound path for the very first chime of
the boot: it locates its sample table by scanning for the signature `"joebritt"` below the
top of the 68k ROM, with entry 0 the boot chime (16-bit stereo) and entry 1 the error
chord (8-bit samples expanded ×4 to two stereo frames each and 2× upsampled); each entry
carries the byte to write to `+$10` [3] (*observed*; the early path programs the window
base at physical 0 and plays from physical `$10000`/`$12000` before the memory map is
final, where the StartMgr path re-points the window at the top of RAM).

### 4.3 The Sound Manager output component

The ROM registers a `sdev`-type Sound Manager component for this hardware — `sdev awac`,
"AWACSHARDWARE" — alongside the mixer, rate converters, MACE3/6 decompressors, the
`adio hphn` external-headphone-dispatch component, and the sound input driver
`.AppleSoundInput` (DRVR ID −16500, "PDM sound in") [3] (*observed* in the component
resource strings). The component's hardware-facing behavior, from its MacsBug-named
routines [3]:

- **StartHardware**: if sound input is running (`+$11` bit 7), verify that `+$10` and
  `+$08/+$09` still match the configuration the input side set — output cannot reprogram
  the engine out from under a running input — returning error **−225**
  (`siInvalidSampleRate`) on conflict. Otherwise: set the half length (`$04`/`$00`),
  prime both half-buffers up the sifter chain (`PrimeDMABuf`/`GetSourceData`, zero-filling
  shortfall), install the sound-out vector (DMA channel 9, refcon `'JoeB'`) in the
  dispatch table, run `DetermineSpeakerMute`, ack all flags, enable all three interrupt
  sources (`+$18 |= $C0`, `|= $20`, `|= $0E`), and set RUN.
- **StopHardware**: clear the interrupt enables (`+$18 &= $F1`), clear RUN
  (`+$10 &= $FE`), ack the stopped flag (`+$18 |= $20` — raised by the stop itself,
  §3.3), and restore the dispatch vector to `BadIntVector`.
- **SetHardwareSampleRate**: compares the requested Fixed rate against `$AC440000`
  (44100.0); 44100 → `+$10 = (old & $F9) | $04`, anything else (the driver only ever
  passes 22050) → `+$10 = old & $F9`. If input is running with a conflicting rate, error
  **−227** (`siDeviceBusyErr`). The final `+$10` value is mirrored into a driver shadow.
- **INTERRUPTROUTINE** (entered from the level-4 dispatch with the sound-out flag set):
  `+$18` bit 6 → buffer at window `+$10000`, ack `$40`; bit 7 → window `+$12000`, ack
  `$80`; bit 5 → ack `$20` and re-set RUN (restart after underrun, no refill); otherwise
  defer the buffer copy.
- **VMUNSAFEINTERRUPTROUTINE** (the deferred part): mask the interrupt enables
  (`+$18 &= $F1`), drop IPL from 4 to 3, and — on the buffer-1 completion only — run
  `DetermineSpeakerMute` before refilling: **headphone-jack sense is re-examined every
  other buffer completion (~46 ms at 44.1 kHz) during playback**, not
  interrupt-on-change. Then refill via the sifter chain and re-enable.
- **DetermineSpeakerMute**: with auto-mute enabled, mute the speaker (register 1 bit 7)
  iff an external headphone component is registered or `+$06` bit 3 reads headphones
  present.
- **MuteSpeaker / MuteHeadphones**: set/clear `$0080` / `$0200` in the register-1 shadow
  and issue a codec command only when the shadow changes — always through
  `WaitExpandClear` and the `$40`/hi/lo/`$C0` sequence.

### 4.4 The sound input driver

`.AppleSoundInput` (DRVR −16500) records **16-bit stereo at 22 050 or 44 100 Hz, line
level** — the PlainTalk microphone is line level, and the platform publishes the
line-level feature bit [3]. Its behavior, from disassembly (the shipped driver is
stripped of per-function symbols) [3]:

- **Open**: allocate driver globals (failing with −108 if memory is short); ensure the
  `+$11` bits 5:2 field is non-zero (set bit 2 if zero); write codec register 0 = `$049`
  (microphone mux, +13.5 dB both channels) and register 1 = `$000`; default rate 22 050
  (Fixed `$56220000`); publish a pointer to a shared sub-block of its globals so the
  output component can observe input state.
- **Source select** (control call): clear register 0 bits 11:9, then select — source 1 =
  microphone (bit 10, gains forced `$99`), source 2 = internal CD audio (bit 9, gains
  forced 0), source 3 = GeoPort telecom audio (bit 11, gains forced 0; only offered when
  the `'telc'` audio component exists, and then the speaker is muted during play-through).
  Out-of-range sources return an error.
- **Input gain** (the Sound Manager AGC's actuator): a Fixed 0…1.0 control maps to
  nibble `round(g × 15)`; the mono call duplicates the nibble to both channels, the
  stereo call sets left/right independently — written to the register-0 gain nibbles.
- **Play-through**: sets/clears register 1 bit 6 (loop-through). While play-through is on
  and the source is not telecom, a **1000 ms Time Manager task polls `+$06` bit 3** and
  sets/clears the register-1 speaker mute so monitoring follows the headphone jack.
- **StartHardware (input)**: after a final codec write, save the current `+$10`, `+$09`,
  `+$08` to restore at stop. If output is running (`+$10` bit 0) with a *different*
  configuration than the input wants (rate field mismatch, or length not `$04`/`$00`), it
  **stops the output** (`BCLR #0, +$10`) — the mirror image of the output component's
  refusal. Then: install the sound-in vector (DMA channel 8), write the rate
  (`+$10 = (cur & 1) | desired`), `+$09 = 0`, `+$08 = $04`, clear-and-enable the input
  flags (`+$14 = $F0` then `$0C`), clear `+$11` bits 1:0, and set `+$11 |= $80` (input
  RUN).
- **StopHardware (input)**: `+$14 = 0`, `+$11 &= $7F`, restore the saved `+$10/+$09/+$08`,
  restore the dispatch vector.
- **Interrupt**: read `+$14`; bit 6 set → the buffer at window `+$C000` completed, ack
  `$40`; otherwise the `+$E000` buffer, ack `$80`; then defer the copy. The copy reaches
  the data CPU-side through the nanokernel's logical mapping — it reads samples at
  `$61000000 + offset` — and moves them through a 3 × 4 KB software ring in driver memory
  to the client. Note the ping-pong test is inverted relative to the output handler's
  (input tests bit 6 and assumes bit 7 otherwise), so a simultaneous both-halves-pending
  state resolves to half 0 first [3].

### 4.5 Headphone sense and speaker auto-mute

The headphone jack has no interrupt; its sense bit (`+$06` bit 3) is read in three places
[3] (*all observed*): by the boot beep (mute the speaker for the chime if headphones are
in), by the output component every other output buffer (~46 ms) while playing, and by the
input driver's 1 Hz Time Manager task during play-through. The jack must therefore answer
reads at any time, not only while DMA runs. Auto-mute — speaker muted whenever headphones
are present or an external headphone component is registered — is a user preference in
the component's defaults (§2.10), honored by `DetermineSpeakerMute`.

### 4.6 What the system publishes

- **Gestalt**: the ROM's Gestalt implementation reports the sound hardware as the AWACS
  selector when the platform device table marks the codec present [3].
- **Feature bits** published for the platform [3]: has-sound-input, 16-bit sound, stereo
  input, stereo output, stereo mixing (the mono speaker mix, §3.7), simultaneous play and
  record, and line-level input (PlainTalk).
- **The phantom ASC**: the platform's published device table lists the ROM base
  (`$40800000`) as the ASC base and sets the ASC-present flag although no ASC exists —
  classic software that pokes the Apple Sound Chip reads ROM and is harmless [3]
  (*observed*). See also the [ASC reference page](../../hardware/asc.md).

### 4.7 Later system software

The released MkLinux platform driver for these machines drives the same register file and
confirms the parts Apple's own ROM never exercises: the manufacturer and revision fields
of the status window (§2.3), the `%01` = 29 400 Hz rate code (§3.4), buffer lengths other
than `$0400` (it programs `$07FF`), the phase/position register layout (§2.5), and the
flag/enable gating — and it polls the status window at about 2 Hz for plug detection, the
same jack-sense-by-polling contract as Apple's drivers [6]. The later Linux and NetBSD
AWACS drivers (Grand Central-era machines) are **not** a guide to this machine's host
interface — their five 32-bit little-endian registers at 16-byte stride describe a
different platform's sound block — but their codec register model is the same lineage and
corroborates the AWACS register semantics used here [7] (§1.5, §2.10).

## 5. Quirks & errata

- **One codec, one rate.** Input and output share the engine: the rate lives in the
  *output* control register and the half-length register is common. The output component
  refuses to reprogram under a running input (−225/−227); the input driver instead
  **stops a conflicting output** and restores it at stop. Asymmetric by design, and easy
  to get wrong in either direction.
- **The busy handshake must clear.** Software spins on `+$00` bit 7 after every `$C0`
  strobe, with timeouts of tens to hundreds of thousands of iterations; the port must
  clear the bit on its own, and tolerate the `$40` cancel write.
- **Flags are write-1-to-clear, and acks are read-modify-write.** The handlers ack by
  OR-ing the flag bit back into the register; an implementation must not clear the *other*
  pending flag through the RMW's read (§2.8). Whether the write-back of one pending flag
  clears a simultaneously pending sibling is unobserved (§6).
- **Enables gate flags, not just interrupts.** A stale flag raised while its enable is
  clear must not assert the level-4 line — the output start sequence acks everything
  *before* enabling, and the deferred handler masks enables while it works.
- **Stopping raises the error flag.** The output engine sets `+$18` bit 5 when it stops;
  the stop path acks it *after* clearing RUN, and the underrun path in the interrupt
  handler re-sets RUN (so re-setting an already-set RUN bit must be harmless).
- **The position counter must advance with time.** The boot beep is pure polling — flags
  and the 24-bit counter, no interrupts — and a frozen engine hangs the ROM in the refill
  loop or the drain.
- **Full volume is attenuation 3, not 0.** The Sound Manager mapping reserves headroom;
  volume 0 under the beep's law is −21 dB, not silence.
- **The default output rate is 22 050 Hz** — 44.1 kHz appears only when 44.1 kHz content
  plays; the boot chime is the notable exception, running at 44.1 kHz.
- **Headphone sense is polled**, three ways (boot beep, ~46 ms playback poll, 1 Hz
  play-through poll); there is no jack interrupt to raise.
- **The boot beep blanks the screen** (`$50F28000` bit 7 — an AMIC video-control
  register, not a sound register) to keep video refresh from stealing DRAM bandwidth.
- **The mono speaker is a resistive L+R mix** behind one amplifier — stereo panning is
  inaudible on the internal speaker, and the published stereo-mixing feature bit says so.
- **The developer note's sound clock is a misprint**: Table 2-3 prints 44.1584 MHz; the
  board carries 45.1584 MHz, and only 45.1584 divides to exactly 44.1 kHz.
- **The ASC base address is a lie pointing at ROM** — deliberate compatibility
  furniture, not a decode (§4.6).
- **The input handler's inverted ping-pong test** resolves a both-halves-pending state to
  half 0, unlike the output handler's order — any model of the flag registers must
  satisfy both tests as written.
- **Codec registers are write-only with software shadows** — nothing in the command port
  reads a codec register back; all readback contracts in the software are shadow reads.

## 6. Open questions

1. **`+$00` bits 6:0** — whether bit 6 (always written 1) and the low bits have hardware
   meaning, and the exact read-side semantics of bit 7 beyond the busy/extended-codec
   roles the software relies on.
2. **The status window's full mapping on PDM** — the manufacturer ID value on production
   boards, where (if anywhere) the AWACS revision value 2 surfaces, the assignment of
   `+$06` bit 2, and the `+$04` flag bits: the MkLinux-documented layout is the best model,
   but Apple's ROM reads only `+$06` bit 3 and never constrains the rest.
3. **`+$10` bits 7:3** — never set by any observed software; unknown whether other rate
   codes (`%11`) or functions exist.
4. **`+$11` bits 5:2** — the input-subframe-select reading is inferred from the software's
   insistence on a non-zero field always written `%0001` and the documented subframe
   layout; no code ever writes another value, so the field is unproven. Bits 1:0 are
   cleared before starting and never set: function unknown.
5. **`+$14` bits 5:4 and 1:0** — unobserved; whether an input overrun/error flag exists
   (the input side enables two interrupt sources where the output side enables three) is
   open.
6. **The position register's literal semantics** — window byte-offset at 64-byte
   granularity versus buffer phase with packed intra-frame clock bits (§2.5); the two
   readings satisfy the same observed predicate, and whether an input-side position
   counter exists at all is unknown (no software reads one).
7. **Whether `+$10` RUN self-clears on underrun**, or bit 5 merely flags while RUN stays
   set — the interrupt handler's re-set of RUN is consistent with both.
8. **The W1C/RMW edge** — whether hardware protects a simultaneous both-halves-pending
   ack from clearing the sibling flag (§2.8, §5); no observed code path hits the window.
9. **The AMIC flag registers' clearing semantics** (`$50F2A008`/`$0A`) — the ROM never
   writes them; per-engine acks appear to clear the summary bits, but the exact mechanism
   is unverified.
10. **The `AwacsSel` strap** — what SEL_0/SEL_1 program in the codec (plausibly its
    subframe assignment for the TDM share), and whether the part takes a master clock
    in addition to the serial bit clock; the schematic digest shows only the four serial
    nets, the strap, reset and TEST.
11. **The 22.05 kHz mechanism** — halved frame rate versus sample repetition, and how
    29.4 kHz is produced; not observable through the register interface.
12. **Reset state** — the power-on values of the register file, and whether a CPU reset
    without a power cycle re-initializes the engine or only the `vSndEnb` release does.
13. **Absent registers** — no byte-swap (DMA data is always big-endian), no clip count,
    and no recalibration sequence exist on PDM; software never needs them. Whether they
    exist silently in hardware is unverifiable from software alone.
14. **6100/7100 board wiring** — pin-level facts here come from the 8100 schematic; the
    smaller boards' sound wiring is assumed to match.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh
   6100/60, 6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March
   1994 — §"Features" p. 8; §"Apple Memory-Mapped I/O Controller" pp. 15–16; §"AWAC Sound
   Chip" p. 17; §"Fixed-Rate System Clocks" Table 2-3 and §"CPU Bus Arbitration"
   Table 2-4 p. 20; §"Sound I/O" pp. 46–48 (Table 3-15 sound connections; Table 3-16 PDS
   sound signals; Figures 3-12/3-13/3-14 frame and synchronization diagrams); §"DAV
   Interface" pp. 53–54, Table 4-4; Glossary.
2. Apple Computer, Inc., *Developer Note: Enhanced Power Macintosh Computers* (Power
   Macintosh 6100/66, 7100/80, 8100/100, 8100/110), Developer Press, 1994 — §"Clock Chips"
   p. 5; §"Clock Speeds" p. 8.
3. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3
   (March 1994) — disassembly of the sound paths; 68k code runs at $40800000 + file
   offset. Cited offsets: AMIC init post-procedure $10390; startup beep (StartMgr)
   $E59B0–$E5CC2; PowerPC hardware-init sound path and "joebritt" sample-table scan
   $FFF0551C–$FFF056C8; Sound Manager AWACS component $1DC3F8–$1DDE40 (MacsBug symbols
   AWACSHARDWARE, PrimeDMABuf $1DD2B8, StartHardware $1DD380, StopHardware $1DD480,
   SetHardwareSampleRate $1DD500, SetHardwareSpeakerVolume $1DD720,
   SetHardwareHeadphoneVolume $1DD8B0, DetermineSpeakerMute $1DD9A0, MuteSpeaker
   $1DDAA0, MuteHeadphones $1DDB20, WaitExpandClear $1DDBA0, INTERRUPTROUTINE $1DD0A0,
   VMUNSAFEINTERRUPTROUTINE $1DD130, output-device init $1DC8F0, GetInitialPreferences
   $1DCF80); sound input driver `.AppleSoundInput` $1CEC99–$1D0489 (open $1CECEA, codec
   defaults $1CED06/$1CED4C, source select $1CF1D4, gain $1CF264, register-0 bit-8 toggle
   $1CF36E, rate getter $1CF928, telecom path $1CF992, play-through $1CFA46, 1 Hz
   jack-poll task $1CFB12, input interrupt $1CFB9C, deferred copy $1CFBCE, input start
   tail $1CFDA8–$1CFE5A, input stop $1CFE5C); component-resource strings
   $1DE060–$1DF0C8; level-4 AMIC dispatcher and the DMA flag-register reads.
4. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing
   051-0333 rev A — sheet 11 "Clocks" (oscillator G3 = 45.1584 MHz); sheet 12 "Apple
   Miscellaneous Interface Chip & Ethernet ROM" (AMIC U14, serial sound engine, C45M
   input); sheet 19 "AWACS Sound Circuit" (codec U12, jack and amplifier wiring, sense
   lines); sheet 10 (PDS connector J14).
5. IT&T (Micronas Intermetall GmbH), *ASCO 2300 Audio-Stereo Codec* data sheet, order
   no. 6251-333-3DS, third release, February 1995 — §1 (16-bit, 8–48 kHz conversion
   range); §2.4 output mute; §2.7 automatic sampling-rate detection; §3.1 "Full Feature
   Format – 256 Bit Format" (frame structure; auxiliary control cells 1–24 with the
   Expand bit; auxiliary status cells 1–24; SDOUT tri-state behavior); Figs. 3-1 and 3-4.
6. Apple Computer, Inc. and Prime Time Freeware, *MkLinux DR3* kernel sources, Power
   Macintosh (PDM) platform AWACS sound driver — codec status window (sense,
   manufacturer, revision), buffer-length and rate programming (including the 29 400 Hz
   code), the phase/position register, the DMA flag layout at $50F2A008/$50F2A00A, the
   extended-codec flag, jack-sense polling, and the stop/underrun commentary.
7. Linux kernel sources, AWACS sound drivers — `sound/ppc/awacs.c` and `awacs.h`,
   `sound/oss/dmasound/dmasound_awacs.c` (v2.6.12) — and the NetBSD macppc AWACS driver,
   `sys/arch/macppc/dev/awacs.c` — the codec register model, status-field layout and
   revision values (AWACS = 2, Screamer ≥ 3) of the later Grand Central machines of the
   same codec lineage. Cited only for the codec's own register semantics; these drivers'
   host interface (five 32-bit registers at 16-byte stride, DBDMA) does not exist on PDM.
