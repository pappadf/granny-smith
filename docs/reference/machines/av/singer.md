# Singer — the AV sound codec

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, the division of labor along the sound
   datapath, where software finds it, clocking
2. [Register file](#2-register-file) — the PSC sound block $50F31000+$200: sndComCtl, the singerCtl/singerStat
   codec images, sndPhase, the double-buffer geometry, reset state
3. [Behaviour](#3-behaviour) — the serial frame bus, rate selection and the clock family, the auxiliary field
   transport, the analog side, completion and interrupt signalling
4. [Programming model](#4-programming-model) — the two ROM drivers, the boot chime instruction by instruction,
   the chime data and volume mapping, the DSP driver's codec protocol
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **Singer** is "an I/O chip that constitutes a 16-bit digital sound codec. It conforms to the IT&T ASCO
2300 *Audio-Stereo Codec Specification*" [1] p. 16. It is the analog front end of the AV platform's sound
subsystem: a two-channel (stereo) A/D and D/A converter pair with programmable input gain, programmable
output attenuation and mute, a four-pin general-purpose digital port in each direction, and a serial port
that carries audio and control in time-division-multiplexed 256-bit frames. The developer note counts it
among the main logic board's components and, in the PSC's function list, names the pairing this page is
about: the PSC "contains 16-byte buffers for sound data to and from the Singer sound encoder and decoder
(**codec**)" [1] p. 13.

The ASCO 2300 itself is described by its vendor as "a high quality two-channel A/D and D/A converter for
modern digital signal processing systems such as sound processing in multimedia workstations", with "a
programmable conversion rate of 8 kHz to 48 kHz", 16-bit resolution, "more than 90 dB S/N (@ 24 kHz) and
0.03% THD from A/D input to D/A output", and "a new serial port protocol which can carry up to 4 stereo
channels of 16-, 18-, or 20-bit sound data and additional auxiliary information" [2] §1 p. 3. Every
codec-side fact in §2.3, §2.4 and §3 below is taken from the ASCO 2300 data sheet; whether Apple's Singer
is a stock ASCO 2300 in its 44-pin PLCC package or a custom part implementing the same specification is
nowhere stated (§6.1).

Singer replaces a dynasty. Every earlier Macintosh sound subsystem was built around the Apple Sound Chip
or one of its integrated clones; the AV machines are the first whose sound hardware is not an ASC of any
kind — the developer note's chip inventory carries Singer and no Apple Sound Chip [1] pp. 13–16. The
ASC/EASC lineage and its register-compatible clones are [ASC](../../hardware/asc.md)'s subject (§2.3
there); the two Apple data sheets for that line are listed here for completeness [5] [6] and are not
restated. The practical consequence for software is sharp: nothing in the ASC programming model — FIFO
mode, wavetable mode, half-empty interrupts, the `ascVersion` register — exists on this platform. Sound
reaches RAM only through the PSC's dedicated sound DMA engine, and the codec is programmed only through
the four PSC-mapped registers of §2.

### 1.2 Machines that carry it

| Machine | Apple codename | Board | Sound I/O panel | Notes |
|---|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone | the AV main logic board | ministereo sockets for Audio In and Audio Line Out; built-in speaker [1] p. 38 | three NuBus slots; DAV connector on the board, in line with slot $C [1] p. 42 |
| Macintosh Centris 660AV | Tempest | the same design, speed-reduced | same [1] pp. 10, 38 | one NuBus card via the PDS adapter; the DAV connector rides the adapter card [1] p. 42 |

The two models "contain essentially the same circuit board and system components" [1] p. 10, so the codec,
its wiring and its registers are identical on both; see [PSC](psc.md) §1.2 for the platform-wide facts
(clock rates, CPU, bus) that differ per model, and the machine pages [Quadra 840AV](q840av.md) and
[Centris 660AV](q660av.md) for box-level detail.

### 1.3 Division of labor: the sound datapath

Singer sits at the far analog end of a four-stage path, and each stage has an owner. The complete chain,
from speaker jack to RAM and back:

```
Audio In jack --amps--> Singer A/D --+
                                     | the 4-wire serial frame bus (SCLOCK, SSYNC,
RAM <-- PSC sound DMA engine <-------+  SDIN/SDOUT; see §3.1) shared with the
                                     |  DAV connector and the comms codec
Audio Line Out jack <--amps-- Singer D/A <--+
```

- **The codec** — conversion, gain, attenuation, mute, the digital port, the serial protocol: Singer
  (this page).
- **The DMA engine** — the double-buffer stream player/recorder that moves 16-bit stereo frames between
  RAM and the serial bus: the PSC's dedicated sound channels (1 bit wide on the I/O side, 16-byte FIFOs
  [1] Table 2-10 p. 29; the highest- and second-highest-priority channels to the CPU bus [1] Table 2-11
  p. 30). Its engine-side behavior is [PSC](psc.md) §3.5's subject; this page carries only the
  codec-facing registers inside the same block.
- **The DSP** — every byte of production sound. The DSP3210's task graph sits between the Sound Manager
  and the DMA engine: "Standard sound consists of a set of tasks installed in the real-time task list
  plus the DSP Sound Driver" [1] p. 116, and the sound tasks consume and produce the DMA streams
  ("The sound input task takes the serial data stream from the DMA channel coming from the stereo A/D
  converter"; "The sound output task ... transports it to the sound output buffer. The DMA channel passes
  this data to the sound D/A converters" [1] p. 117). The DSP side — its task list, its rate converter,
  its frame heartbeat — is [DSP board](dsp3210-board.md)'s subject (§3.7–§3.8 there).
- **The board taps** — the DAV connector exposes the same serial bus to NuBus accessory cards: "The
  Singer sound codec uses time-division multiplexing to transfer multiple audio channels between the DAV
  connector, the Singer chip, and the PSC for DMA transfers to and from RAM memory" [1] p. 44 (§3.1).

Apple's own compatibility rule keeps third-party software out of the chain: "Use only system API calls to
access hardware; never try to modify or program the serial, SCSI, ADB, sound, or video subsystems
directly" [1] p. 7 — and, as §4.1 shows, the shipped system software obeys it: only ROM code ever touches
the registers in §2.

### 1.4 Where software finds it

The codec has no address of its own. It is reached entirely through a block of PSC registers at PSC base
$50F31000 + $200 through +$21C (§2.1); the PSC "serialises the codec's aux-control/status fields onto the
frame bus, so the CPU only sees ordinary memory-mapped registers" — the machine's decoder record carries a
Singer base entry and it reads zero, the value used for a device with no independent window [3] (*observed*
in the ROM's decoder tables). There is no ASC-style base address to probe and no chip identification
register; a program that wants to know which sound hardware it faces asks the OS, not the silicon.

### 1.5 Clocking

The sound subsystem runs on one master clock and derives everything else from it:

| Clock | Frequency | Source | Role |
|---|---|---|---|
| C24_576M | 24.5760 MHz | out of the PSC [1] Table 2-2 p. 17 | the Singer master clock (CLOCK) |
| C22_5792M | 22.5792 MHz | out of the PSC [1] Table 2-2 p. 17 | labeled "Singer (44.1 KHz)" — see §3.2 and §6.9 |
| singerBitClk | 256 × f_s | the serial bit clock, present at the DAV connector [1] p. 44 | shifts SDIN/SDOUT and SSYNC |
| singerSync | f_s | frame/word sync, present at the DAV connector [1] p. 44 | marks frame and word boundaries (§3.1) |

The codec's three timing inputs are "the master clock (CLOCK, 24.576 MHz typically), the serial clock
SCLOCK, which is set externally to a value 1/n of the master clock and the serial synchronization signal
SSYNC"; "the SSYNC signal is always identical to the sampling rate", and the codec "senses the ratio
between the master clock and the serial sync and adjusts automatically to the desired sampling rate"
[2] §2.7 p. 4. So the sample rate is not a codec register: it is a *ratio*, selected by whoever divides
the master clock into SCLOCK — on this platform, the PSC, through sndComCtl's rate field (§2.2). The
automatic-rate-detection design also gives the codec its clock sanity check: "If the sampling rate of the
serial port drops below the specified minimum, an error is reported and the output muted" (error 3,
§2.4) [2] §2.7 p. 4.

Two further clocks bound the subsystem from the telecom side. The ATECS chip "can synchronize the DSP and
sound subsystems to an external clock signal received through the Apple GeoPort serial port connector.
In the absence of an external clock, it generates crystal-controlled 49.152 MHz timing signals for 48 KHz
operation or 45.1584 MHz timing signals for 44.1 KHz operation" [1] p. 16, and its 45.1584/49.1520 MHz
outputs feed the PSC as rate references [1] Table 2-2 p. 17 — the mechanism by which an external telecom
clock could discipline the sound subsystem. No shipped code exercises that path (§6.9).

## 2. Register file

### 2.1 The register block

All Singer-facing registers live inside the PSC's sound block at **$50F31000 + $200 through +$21C**, one
byte past the last of them the DSP3210's host register. [PSC](psc.md) §2.7 carries the summary table and
the engine-side view; the per-register detail is here. Access widths, as exercised by the ROM [3]:
word at $200 and $218, longword at $204/$208/$20C/$210/$214, byte at $21C.

| Offset | Absolute | Width | Name | Function |
|---|---|---|---|---|
| +$200 | $50F31200 | word | sndComCtl | sound and communications control: subframe selects, DMA enables, rate (§2.2) |
| +$204 | $50F31204 | long | singerCtl | codec control image — the aux control field (§2.3) |
| +$208 | $50F31208 | long | singerStat | codec status image, read-only (§2.4) |
| +$20C | $50F3120C | long | sndPhase | the DMA engine's playback position, read-only (§2.5) |
| +$210 | $50F31210 | long | sndInBase | sound-input double-buffer base (§2.6) |
| +$214 | $50F31214 | long | sndOutBase | sound-output double-buffer base (§2.6) |
| +$218 | $50F31218 | word | sndSize | half-buffer size in sample frames (§2.6) |
| +$21C | $50F3121C | byte | dspOverRun | the DSP3210 host register — not a Singer register; see [DSP board](dsp3210-board.md) §2.1 |

Unlike every other register group in the PSC, this block carries **no sense bit**: sndComCtl, singerCtl
and the base registers take plain data writes, and the field get/set and enable routines of §4.4 do
ordinary read-modify-write on them. The interrupt registers they feed (L4IR/L4IER, the VIA2 window) keep
the PSC-wide sense convention — see [PSC](psc.md) §2.2.

### 2.2 sndComCtl (+$200, word)

The sound and communications control word steers the DMA engine and the frame machinery. Bit assignments,
each anchored in the shipped ROM [3]:

| Bits | Field | Meaning |
|---|---|---|
| 0–1 | pSubFrmInSel | input subframe select — a **2-bit binary code**, 0..3, choosing *one* of the four subframes for capture (§3.1) |
| 2–5 | pSubFrmOutSel | output subframe select — a **4-bit one-hot mask, one bit per subframe**, so one output stream can fill several subframes (§3.1) |
| 6 | pFrmIntEn | frame interrupt enable — gates the frame tick to *both* processors: the host's level-2 sound-frame interrupt and the DSP's EXT1 pulse ([DSP board](dsp3210-board.md) §3.4) |
| 7 | pSndInEn | sound **input** DMA enable |
| 8 | pSndOutEn | sound **output** DMA enable |
| 9–10 | pSndRate | codec sample-rate code: 0 = 24 000 Hz, 1 = 32 000 Hz, 2 = 48 000 Hz (§3.2); code 3 is never used by any shipped code |
| 11–12 | pComRate | communications-codec rate — never written by any shipped code (§6.2) |
| 13–15 | — | unused; written as zero by every shipped writer |

The encoding asymmetry of the two subframe fields is proven by the constants the ROM writes. The output
enable value is **$0104** = bit 8 (pSndOutEn) + bit 2: the output field's value is %0001, subframe 1 —
a binary encoding of "subframe 1" would have needed two bits and produced $0101 [3] ($408C5C88). The
input field, by contrast, is never written with any value but 0 by shipped code; its two-bit width is
structural (§3.1: only one device may drive SDOUT per slot, so one code suffices).

Three constants recur verbatim [3]:

| Constant | Value | Written by |
|---|---|---|
| sndComCtlInit | $0000 | the boot chime's first write — everything off (a plain `CLR.W`) |
| soundOutEnable | $0104 | the boot chime's arm write: output subframe 1 + pSndOutEn |
| (teardown) | $0004 | the boot chime's last write: the subframe mask kept, DMA and interrupts off |

The frame-interrupt enable bit is the one field the DSP driver manages on its own: its enable/disable
pair reads sndComCtl, sets or clears bit 6, and writes it back [3] ($4099C82A–$4099C87A), and its rate
selector sets bit 6 together with the rate code in a single write (§3.2). The driver *clears* bit 6 before
taking the DSP out of reset and only re-enables it once the buffers and rate are programmed — the frame
engine, and with it the DSP's clock, starts exactly once [3] ([DSP board](dsp3210-board.md) §4.5,
*observed*).

### 2.3 singerCtl (+$204, long) — the codec control image

`singerCtl` is a direct image of the codec's 24-bit **auxiliary control field** — the control word the
PSC transmits to the codec inside every selected subframe on SDIN (§3.3). The correspondence is exact and
complete: register bit *n* carries auxiliary cell 24−*n*, so the field reads as a plain integer and each
multi-bit field (which the codec receives MSB first [2] §3.1 p. 8) appears as a normal nibble. Bits 24–31
are unused.

| Bits | Field | Codec pins/section | Meaning [2] §3.1 p. 8 |
|---|---|---|---|
| 0–3 | pOutputPort | BO1–BO4 | digital output port: "Controls four digital output pins on the codec (BO1 to BO4). A digital value of 1 gives a high output"; initialized to zero at the codec's power-up. Not audio — four general-purpose board-control pins (§6.7) |
| 4–7 | pRightAtten | aux cells 17–20 | right D/A output attenuation, 0 to −22.5 dB in 1.5 dB steps |
| 8–11 | pLeftAtten | aux cells 13–16 | left D/A output attenuation, same ladder |
| 12–15 | pRightGain | aux cells 9–12 | right A/D input gain, 0 to +22.5 dB in 1.5 dB steps |
| 16–19 | pLeftGain | aux cells 5–8 | left A/D input gain, same ladder |
| 20 | pRightInMux | aux cell 4 | right input select: 0 → RIN1, 1 → RIN2 [2] §4.2 p. 14 |
| 21 | pLeftInMux | aux cell 3 | left input select: 0 → LIN1, 1 → LIN2 |
| 22 | pMute | aux cell 2 | mute the D/A output — implemented as about −80 dB of attenuation [2] §2.4 p. 3; **also resets the codec's A/D valid-data counter**, which "allows mute to be used when changing sample rates" |
| 23 | pExpCtl | aux cell 1 | expand bit — **must be 0**. If set, the codec raises error code 1 and ignores all other aux control cells; audio processing continues (§2.4) |

The dB ladder, identical in both directions (Table 3-2 for gain, Table 3-3 for attenuation [2] p. 11):

| Nibble | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | $A | $B | $C | $D | $E | $F |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Gain (dB) | 0 | +1.5 | +3 | +4.5 | +6 | +7.5 | +9 | +10.5 | +12 | +13.5 | +15 | +16.5 | +18 | +19.5 | +21 | +22.5 |
| Attenuation (dB) | 0 | −1.5 | −3 | −4.5 | −6 | −7.5 | −9 | −10.5 | −12 | −13.5 | −15 | −16.5 | −18 | −19.5 | −21 | −22.5 |

Analog reference levels: digital full-scale D/A output is 0.94 V RMS (typical) at 0 dB attenuation into
100 kΩ or more, and the A/D clips at 1.44 V RMS (typical) at 0 dB gain [2] §4.5.3 p. 21. The developer
note's panel numbers are the same ladder seen from outside the box: "Audio In — 8 Ω impedance, 2 V rms
maximum, 22.5 dB gain available"; "Audio Line Out — 37 Ω impedance, 0.9 V rms maximum, attenuated
−22.5 dB" [1] Table 2-15 p. 38. (The panel's 2 V rms maximum over the 1.44 V rms clipping level is the
reason the gain ladder exists: line-level inputs are padded down, and the +22.5 dB of gain is for
microphone-level sources.)

Two init constants survive verbatim in the ROM [3] ($408C5C52, $408C5C90):

```
singerCtlInit = $0045500F   ; pOutputPort=%1111, attenuation L/R = 0 (0 dB),
                            ; gain L/R = %0101 (+7.5 dB), pMute = 1
unMute        = $0005500F   ; identical, pMute = 0
```

The ROM's own field get/set entry points independently confirm the layout from the shipped binary: one
entry addresses both attenuations with mask $00000FF0 shifted 4, another both gains with $000FF000
shifted 12, a third both input muxes with $00300000 shifted 20 [3] ($4099CADA, $4099CAE8, $4099CAF6).

### 2.4 singerStat (+$208, long, read-only) — the codec status image

`singerStat` is the image of the codec's 24-bit **auxiliary status field**, which the codec transmits
back on SDOUT in its assigned subframe (§3.3). Same correspondence: register bit *n* carries status cell
24−*n*; bits 24–31 unused.

| Bits | Field | Status cells [2] §3.1 p. 9 | Meaning |
|---|---|---|---|
| 0–3 | pInputPort | 21–24 | input sense: the levels on the codec's four digital input pins, "sampled at the last positive transition of the SCLOCK within a frame" |
| 4–11 | — | 13–20 | reserved, "Should be 0000" |
| 12–15 | pRevNum | 9–12 | revision number — "Set to 0000 for this part": a conforming codec reads as zero here; do not invent a revision |
| 16–19 | pSingerStatus | 5–8 | error number, 0 = none: 1 = the SDIN expand bit was set (control not understood; data still valid); 2 = alternate-format sync detected; 3 = serial clock out of range. Codes 2 and 3 **automatically mute the analog output** |
| 20 | pOFRight | 4 | right A/D overflow (clipping) |
| 21 | pOFLeft | 3 | left A/D overflow |
| 22 | pValidData | 2 | A/D data valid: 0 "after reset, mute or low-power operation until enough clocks have passed for the full latency of the digital filters" [2] §2.6 p. 4 |
| 23 | pExpStat | 1 | expand bit — must read zero |

What the ROM actually reads is exactly three bits, and only ever two routines read them [3]:

- **Bit 3** — the output-port strap: both the boot chime and the DSP driver read it before unmute, and
  set pOutputPort to %0000 when it is set, %1111 when it is clear (§4.2, §4.4). This is a run-time
  decision, made on every boot.
- **Bits 2 and 0** — a three-way input-source enumeration in the DSP driver [3] ($4099CAAA–$4099CACE):

```
MOVEA.L $40(A5),A0      ; A0 = PSC base
MOVE.L  $208(A0),D0      ; singerStat
CLR.L   $20(A2)          ; result = 0
BTST    #2,D0           ; sense bit 2
BEQ     done            ;   clear  -> 0
MOVE.L  #1,$20(A2)       ;   set    -> 1
BTST    #0,D0           ; sense bit 0
BNE     done            ;   set    -> 1
MOVE.L  #2,$20(A2)       ;   clear  -> 2
```

pValidData, pRevNum, pSingerStatus and the overflow bits are **never read by any shipped code** [3] [4];
a register that reads zero everywhere except the sense bits satisfies every reader the platform has.

One numbering caution: the bit-to-pin mapping of the two 4-bit port fields is not fully settled. The
cell correspondence above puts aux cell 21 (BI1/BO1) at register bit 3 and cell 24 (BI4/BO4) at bit 0
[2] §3.1 pp. 8–9; the drivers' own register definitions, as reflected in the annotations of the
disassembly, label the ports the other way round (bit 0 = BI1/BO1) [3]. No shipped code distinguishes
the pins individually — the contract is "bit 3 is the output-port strap, bits 2 and 0 are the input-source
straps" — so the conflict is harmless to a re-implementer but unresolved (§6.7).

### 2.5 sndPhase (+$20C, long, read-only)

The engine's playback position, laid out as a fixed-point frame counter [3]:

| Bits | Field | Meaning |
|---|---|---|
| 0–5 | pPreScaleLSB | fractional position within the current sample frame, in 1/64-frame units (*inferred*: nothing reads these bits; the scale follows from the status call below) |
| 6–17 | offset | integer frame offset into the double buffer, 0 to 2×sndSize−1 (§2.6) |
| 18–31 | — | unused; masked off by every reader |

The mask $003FFFC0 isolates the integer offset, and it is the only part of the register any shipped code
looks at: the boot chime spin-waits on `(sndPhase & $003FFFC0) == 0` and `!= 0` (§4.2), and the DSP driver
has a status call that returns `sndSize << 6` — one half-buffer expressed in sndPhase units if bits 0–5
are 1/64ths of a frame [3] ($4099C6A6). The register must be a live counter: a constant value hangs the
boot (§5).

### 2.6 sndInBase / sndOutBase / sndSize — the double-buffer geometry

These three registers define the DMA engine's buffer pair. [PSC](psc.md) §3.5 describes the engine's
behavior; the geometry, as pinned by both ROM drivers [3]:

| Register | Width | Unit | Meaning |
|---|---|---|---|
| sndOutBase | long | bytes | physical base of the output double buffer |
| sndInBase | long | bytes | physical base of the input double buffer |
| sndSize | word | **sample frames** (4 bytes each) | the size of *one half* of the double buffer |

The frame unit is proven twice [3]: the DSP driver computes sndSize as sample rate divided by frame rate
— *frames per DSP frame*, not bytes ($4099BD0E–$4099BD58) — and the boot chime writes sndSize = $3C0
(960) while using $F00 (3840 = 960 × 4) as the byte size of one half-buffer ($408C5C58–$408C5C5E). The
sample format is 16-bit signed, big-endian, interleaved left then right, four bytes per frame — the same
layout the DSP driver's ring records use ([DSP board](dsp3210-board.md) §3.7, *observed*).

The double buffer covers **2 × sndSize frames = 8 × sndSize bytes**. The engine plays half A, then half
B, then half A again, and the base register is re-latched once per full pass; sndPhase's offset field
counts 0 to 2×sndSize−1 and wraps at the pass boundary [3] (*inferred* at the latch point, but required
by the boot chime's buffer chaining, which advances sndOutBase by $1E00 = 7680 bytes = 2 × 960 frames
per wrap and would play half the audio if the phase wrapped at sndSize instead).

Writes to the base registers mid-pass must therefore not take effect until the next wrap: the boot chime
depends on the engine finishing its current pass over the old base before it starts on the new one
(*inferred*; the only observed writer, the chime's chaining loop, always writes at the wrap instant).

### 2.7 Reset state

The power-on values of the PSC-side registers are unknown — no shipped software ever reads one back to
check [3] [4]. What is pinned is the codec side: the four output-port cells read zero "after power-up
sequence" [2] §3.1 p. 9, and the A/D's valid-data flag reads zero until the decimation filters have
flushed [2] §2.6 p. 4. Every shipped driver initializes the block before use rather than assuming a
state: the boot chime zeroes sndComCtl and writes a full singerCtl image before enabling anything (§4.2),
and the DSP driver's bring-up re-runs the same shape (§4.4).

## 3. Behaviour

### 3.1 The serial frame bus

The codec's serial port is a four-wire bus, and the whole sound subsystem hangs off it. The four lines,
named here as the developer note names them at the DAV connector [1] Table 2-20 p. 44:

| Signal | Direction | Role |
|---|---|---|
| singerMClk | to the codec | 24.576 MHz master clock (CLOCK) |
| singerBitClk | to the codec | bit clock (SCLOCK), 256 × the sample rate; also clocks the sync and data lines |
| singerSync | to the codec | sync (SSYNC): marks the start of a frame or a word |
| singerSerOut | PSC to codec and DAV | the playback stream (SDIN) |
| singerSerIn | codec and DAV to PSC | the capture stream (SDOUT) |

"The signals singerSync, singerSerOut, and singerSerIn are clocked by the singerBitClk signal. The
falling edge of the clock is used to clock the signals, and the rising edge is used to sample them" [1]
p. 45 — matching the data sheet's convention: "The negative transition indicates data change, the
positive edge is the sampling edge. The serial clock always runs at 256 x the sample rate" [2] §3.1
p. 8. The sync pulses distinguish two events: "a frame sync is marked by a pulse two singerBitClk cycles
wide; a word sync is marked by a pulse one singerSync cycle wide" [1] p. 45; the line "is normally low,
and goes high for two bit cells at the beginning of a frame, or one bit cell at the beginning of a word
other than the first word in a frame" [2] §3.1 p. 8. A sync of any other shape is an error: "The codec
produces an error code if any other format sync is detected, and automatically mutes the output" [2]
§3.1 p. 8 (error 2, §2.4) — though the current revision also accepts a plain 50% duty-cycle sync for the
frame and derives word boundaries internally [2] §3.1 p. 8.

A **frame** is 256 bits: "The Singer codec transfers data in 256-bit frames, each of which contains four
subframes of 64 bits each. Each subframe carries two 32-bit audio samples, one left and one right. Each
sample contains 20 data bits and 12 auxiliary bits. Subframe 1 is reserved for the Macintosh system sound
I/O; the other subframes are available for applications and accessory cards to use" [1] p. 44. One frame
per sample period, on all four subframes at once — that is the time-division multiplexing: "The Singer
sound codec uses time-division multiplexing to transfer multiple audio channels between the DAV
connector, the Singer chip, and the PSC for DMA transfers to and from RAM memory" [1] p. 44. The frame
sync marks the frame, and each subframe within it carries its own word pair; the developer note figures
the whole structure as Figures 2-15 through 2-17 [1] pp. 44–45.

Subframe word layout, per the data sheet's Table 3-1 [2] p. 10: word A holds the 20-bit **left** sample
in cells 1–20 and 12 auxiliary control/status bits in cells 21–32; word B holds the 20-bit **right**
sample in cells 33–52 and the second 12 auxiliary bits in cells 53–64. Sound data is "MSB-first with
trailing zeros" where the source has fewer than 20 bits [2] §3.1 p. 7 — so the AV's 16-bit samples occupy
the top 16 of the 20 data cells and the bottom four read zero.

Which subframe the codec answers to is **strapped in hardware, not programmed**: the MODESEL pin
selects the frame format (low = 256-bit) and the SUBFRAME1/SUBFRAME2 pins select the subframe (00 = 1,
10 = 2, 01 = 3, 11 = 4) [2] Table 3-4 p. 12. On SDIN "the codec only responds to the selected subframe,
and ignores data in other subframes"; on SDOUT it "only drives the SDOUT line during the subframe that it
is assigned to, and tri-states SDOUT during other subframes" [2] §3.1 pp. 8–9. That tri-state contract is
what makes the bus shareable: the DAV connector's card, the communications codec and any other subframe
tenant can drive their own slots without colliding with the codec's.

The PSC-side subframe fields of sndComCtl (§2.2) choose which time slot the *DMA engine* uses: the input
select is a 2-bit code (one capture slot — only one device can drive a line per slot), the output select
a 4-bit mask (the same stream can be broadcast into several slots). The board keeps the system sound in
subframe 1: the boot chime enables output with the mask value %0001 (§2.2), and the developer note says
subframe 1 is the reserved one [1] p. 44. The developer note's earlier phrasing — "The Singer uses only
frame 0, leaving other frames available for other sound processing (for example, through the DAV
connector)" [1] p. 38 — is the same statement with the subframes counted from zero.

### 3.2 Sample-rate selection and the clock family

The codec has no rate register; it measures. Its three timing inputs are the master clock, SCLOCK and
SSYNC, and "the codec senses the ratio between the master clock and the serial sync and adjusts
automatically to the desired sampling rate. The serial clock is further checked to be compatible to the
selected frame" [2] §2.7 p. 4. The allowed ratios for the 256-bit frame, from Table 2-1 [2] p. 7:

| Sampling rate (kHz) | CLOCK : SCLOCK ratio |
|---|---|
| 48 | 2 |
| 32 | 3 |
| 24 | 4 |
| 19.2 | 5 |
| 16 | 6 |
| 12 | 8 |
| 9.6 | 10 |
| 8 | 12 |

On the AV board the master clock is 24.576 MHz, and `pSndRate` (sndComCtl bits 9–10) selects the PSC's
divider — code 0 divides by 4 (24 000 Hz), code 1 by 3 (32 000 Hz), code 2 by 2 (48 000 Hz). The table
is definitive in the ROM's rate selector, which compares the requested rate as a 16.16 fixed value and
emits the code [3] ($4099BD7C–$4099BDFA): 22050.0 and 24000.0 both select code 0, 32000.0 selects code
1, 44100.0 and 48000.0 both select code 2 — with anything else returning an error and leaving sndComCtl
untouched. The rate code and the frame-interrupt enable are written together, in one ORI and one store,
after an ANDI of #$01BF that preserves the subframe selects and the DMA enables while clearing the old
rate and bit 6 [3].

Two consequences follow, and they define the platform's rate story:

- **24 000, 32 000 and 48 000 Hz are the only rates that reach the codec.** They are exact: 24.576 MHz
  divided by 4, 3 and 2. The developer note's standard-sound description agrees from the software side:
  "16-bit sound at the current rates of 24, 32, or 48 kHz" [1] p. 117.
- **22 050 and 44 100 Hz are not codec rates.** The machine plays them by running the codec at 24 000 or
  48 000 Hz and resampling inside the DSP3210 — the rate selector sets two DSP-side flags for exactly
  those two cases, and the resampler ships as a DSP module resource [3] [4]. Apple thereby dodges the
  data sheet's requirement for native 44.1 kHz: "Further sample rates of interest are 7.2 kHz and
  44.1 kHz. In both of these cases, the master clock has to be changed to 22.1184 and 22.5792 MHz
  respectively ... Note: Switching from one master clock input to another requires the ASCO to be reset"
  [2] §2.8 p. 5. The developer note nonetheless advertises "16-bit digital stereo sound I/O at sample
  rates up to 48 kHz, including the standard rate of 44.1 kHz" [1] p. 6 — delivered by the DSP, not the
  codec ([DSP board](dsp3210-board.md) §3.7, whose §4.5 carries the full rate table and its persistence).

Code 3 (ratio 6 → 16 000 Hz, by Table 2-1's ladder) is never used by any shipped code [3]; whether the
PSC decodes it at all is unverified (§6.3).

The frame-rate side of the arithmetic — sndSize as sample rate divided by DSP frame rate, the 10 ms/5 ms
"gear shifts" of standard, high-fidelity and professional sound [1] pp. 121–122, and the resulting
240-frame production half-buffers ("For 10 ms frames and 24 kHz sound, this takes the form of two
240-longword buffers" [1] p. 118) — is [DSP board](dsp3210-board.md) §4.5's subject and is not restated
here.

### 3.3 The auxiliary field: control and status transport

Every subframe carries 24 bits of auxiliary data alongside the audio: the 12 aux bits of word A
concatenated with the 12 of word B [2] §3.1 pp. 7–8. On SDIN those 24 bits are the **auxiliary control
field** — the programmer's entire handle on the codec: expand bit, mute, the two input muxes, both
gains, both attenuations and the four output-port bits, in that cell order, MSB first [2] §3.1 p. 8. On
SDOUT they are the **auxiliary status field**: expand bit, valid-data, the two overflow flags, the error
number, the revision number, two reserved nibbles and the four input-sense bits [2] §3.1 p. 9. The
PSC-mapped registers of §2.3 and §2.4 are these two fields, verbatim, bit *n* of the register carrying
cell 24−*n* — the layout the ROM's own field masks confirm independently (§2.3).

Because control is transported in-band, it is *continuous*: the PSC re-transmits the singerCtl image in
every selected subframe, and the codec re-samples its input pins every frame ("sampled at the last
positive transition of the SCLOCK within a frame" [2] §3.1 p. 9). Two error rules guard the field's
integrity: a set expand bit "generates ... Error code 1 ... If this bit is detected, all other aux
control cells are ignored" [2] §3.1 p. 8 — audio keeps playing, only control is lost — and the two
data-format errors (alternate sync, serial clock out of range) mute the analog output automatically
[2] §3.1 p. 9. The valid-data bit is the capture side's readiness signal: 0 "after reset, mute or
low-power operation until enough clocks have passed for the full latency of the digital filters" [2]
§2.6 p. 4 — which is why the mute cell doubling as a valid-counter reset is a feature and not a quirk:
mute, change rate, unmute, and the flag re-converges [2] §3.1 p. 8. No shipped code reads pValidData
(§2.4); the latency figure itself is in the data sheet's characteristics tables [2] §4.5.3 pp. 21–23.

### 3.4 The analog side

The conversion chain, per the data sheet: the A/Ds are pulse-density modulators running at 6.144 MHz,
one external capacitor each, needing "no expensive antialiasing filters"; their 1-bit streams are
decimated by a three-stage multirate FIR chain to the sampling rate; on the output side an eightfold
interpolation chain feeds noise-shaping 5-bit D/A converters followed by built-in analog lowpass
filters [2] §2.9–§2.12 pp. 5–6. Ahead of the A/Ds sit the two 2-to-1 input multiplexers (LIN1/LIN2,
RIN1/RIN2 [2] §4.2 p. 14) and the gain ladders; behind the D/As sit the attenuation ladders and the
mute (§2.3).

The platform-level analog budget, from the developer note: "Sound I/O bandwidth is 20 Hz to 20 kHz,
plus or minus 2 dB. Total harmonic distortion and noise is less than 0.05% over the bandwidth with a
1 V rms sine wave input. The input signal-to-noise ratio (SNR) is 82 dB and the output SNR is 85 dB,
with no audible discrete tones" [1] p. 38 — against the codec's own "more than 90 dB S/N (@ 24 kHz) and
0.03% THD" [2] §1 p. 3; the difference is the board's amplifiers and jacks, not the silicon. The panel
impedances and levels are Table 2-15's [1] p. 38, quoted with the ladder in §2.3. Output muting is
analog-domain silence with a click budget: "Muting is done by setting the output attenuation to maximum
(about −80 dB). To mute without audible clicks, it is recommended first to ramp up the analog output
attenuation and to output digital zeros" [2] §2.4 p. 3 — the shipped drivers obey the second half
exactly (§4.4).

### 3.5 Completion and interrupt signalling

Three independent mechanisms tell software the engine has made progress, all attested in shipped code
[3] (the engine-side summary is [PSC](psc.md) §3.5):

1. **Polling sndPhase.** The boot chime's only completion mechanism (§4.2): no interrupt is enabled or
   taken, the routine simply watches the frame offset wrap.
2. **The frame interrupt** — sndComCtl bit 6. At each frame boundary of the sound engine the tick fans
   out to two consumers: the host receives it as the level-2 sound-frame interrupt in the pseudo-VIA2
   window ([PSC](psc.md) §2.8, bit 6 of the flag byte), and the DSP's IR1N (EXT1) pin is pulsed — "The
   same gated tick is also what the host itself receives as the frame interrupt, so one enable bit
   clocks both processors" ([DSP board](dsp3210-board.md) §3.4, *observed*). The tick must be a short
   pulse, not a level: the DSP's boot-time calibration waits for the *next* edge, and a held level
   hangs it ([DSP board](dsp3210-board.md) §3.4).
3. **The codec status interrupt** — level 4, bit 0 of the PSC's L4IR (SNDSTAT). It fires on a change of
   the codec's status — in practice, the input-sense pins — and its acknowledgment is the L4IR
   write-back-to-clear idiom: read the level register, mask to bit 0, write the value back [3]
   ($4099BCB0–$4099BCB8; [PSC](psc.md) §2.2). The frame *overrun* condition, by contrast, is a DSP-side
   level-5 source (FRMOVRN) handled by the DSP driver's overrun machinery, not by the codec
   ([DSP board](dsp3210-board.md) §3.5).

There is no completion interrupt for "half-buffer played" in the general channels' style: the frame tick
at the engine's frame boundary is the pacing signal, and the DMA gateway bit of [PSC](psc.md) §2.4 has
no bit for the sound engine — its two channels are not in the general channel file at all.

## 4. Programming model

### 4.1 Two ROM drivers, and nothing in RAM

Exactly two bodies of shipped code ever write the registers of §2, and both live in the boot ROM [3]:

1. **The boot-chime routine** at $408C5BB0 — the startup chime and the four death chimes, a complete
   open-coded polling player (§4.2). It runs on every normal boot.
2. **A ROM-resident DSP driver** at $4099B190–$4099CFA0 — the Sound Manager's path. It owns the rate
   selection, the buffer arming and the codec's gain/attenuation/mux surface, and it hands the actual
   audio work to the DSP's task graph, whose DMA-facing buffers this engine serves under the device
   names the ROM's own table carries: "SingerDMAInput" and "SingerDMAOuput" [3] ($4099C310 — the
   misspelling is Apple's).

Nothing else does. An exhaustive scan of the System 7.1 System file and System Enabler 088 — 2 518 files,
22 MB, searched for the PSC sound-block addresses, the init constants and the block's register offsets —
found **zero** register access [4]: the Enabler's sound components are a Sound-Manager `'sdev'`/`'dsp '`
pair whose code path is entirely `_DSPDispatch` calls into the DSP, plus the DSP-module rate converter
that §3.2 describes. The entire RAM-resident sound stack reaches the hardware only through the ROM's DSP
driver, which in turn reaches the codec only through the eight registers of §2.1.

The one-line summary of the platform's sound programming model: **the ROM is the driver, the DSP is the
mixer, and the codec is a register image on a frame bus.**

### 4.2 The boot chime, instruction by instruction

The routine at $408C5BB0 is called with an index in A1 — 0 for the boot beep (from the start-up path at
$408C1972), 1 through 4 for the four death-chime call sites ($408A8860, $408A8924, $408A896C,
$408A89AC) — and it is gated on the machine's decoder kind being the AV platform's [3]. Its complete
register-level behavior [3]:

```
CLR.L   $50036004            ; an unidentified PSC-alias write (§6.8), harmless
MOVE    SR,D3 / ORI #$0700,SR ; mask ALL interrupts (IPL 7)
MOVEC   CACR,D4 / MOVEC #0,CACR ; caches OFF
; volume: the beep reads PRAM first — XPRAM[$10] must be $A8 (validity) else volume 5;
;         volume = XPRAM[$08] & 7; XPRAM[$89] bit 1 selects chime variant 0 or 1;
;         the death chimes skip PRAM and play at full volume
; attenuation nibble = (7 - volume) * 2, duplicated into pLeftAtten|pRightAtten
MOVEA.L #$50F31000,A0
CLR.W   $200(A0)             ; sndComCtl = $0000  (everything off)
MOVE.L  #$0045500F,$204(A0)  ; singerCtl = singerCtlInit (MUTED)
MOVE.W  #$3C0,$218(A0)       ; sndSize = 960 frames
;       look up {byte length, self-relative offset} in the 5-entry table at $408C5CF0
MOVE.L  A1,$214(A0)          ; sndOutBase = the sample's first double buffer
MOVE.W  #$104,$200(A0)       ; sndComCtl = soundOutEnable (subframe 1 + pSndOutEn)
; unmute: singerCtl = unMute & $000FF00F | attenuation, with pOutputPort forced to
;       %0000 when singerStat bit 3 is set (§2.4)
@wait0: MOVE.L $20C(A0),D2    ; sndPhase
        AND.L  #$3FFFC0,D2
        BNE.S  @wait0         ; spin until the frame offset reads 0
        LEA    $1E00(A1),A1   ; advance one full double buffer (2 * 960 frames)
        MOVE.L A1,$214(A0)    ; sndOutBase = the next double buffer
@wait1: MOVE.L $20C(A0),D2
        AND.L  #$3FFFC0,D2
        BEQ.S  @wait1         ; spin until the frame offset leaves 0
        DBF    D1,@wait0      ; one iteration per double-buffer pass
; teardown: singerCtl = keep pOutputPort | $00455000 (0 dB, mute on),
;       sndComCtl = $0004 (subframe mask kept, DMA and interrupts off)
MOVE    D3,SR / MOVEC D4,CACR ; restore interrupt level and caches
```

The structural facts a re-implementer must take from this listing:

- **The routine runs at IPL 7 with the caches off.** No interrupt can rescue it, and it takes none:
  its only progress signal is sndPhase. A constant sndPhase hangs the first spin loop (value nonzero)
  or the second (value zero) — forever, on every boot [3] (*observed*: the two loops are the page's
  single most load-bearing behavior; §5).
- **The buffer chaining is one double buffer per iteration.** The advance is $1E00 = 7680 bytes =
  2 × sndSize frames — the full double buffer of §2.6 — written at the instant the phase offset wraps
  to zero. The loop count is `length / 3840 / 2 - 1`: byte length divided by the half-buffer size,
  halved to double buffers, minus one for the DBF.
- **Mute discipline follows the data sheet's advice**: initialize muted, enable DMA, unmute, and mute
  again at teardown — with the subframe mask left standing in sndComCtl so the next user inherits a
  configured bus [3] ([2] §2.4 p. 3).
- **The output-port decision is made at run time from singerStat bit 3** (§2.4) on every boot, in both
  drivers [3].

### 4.3 The chime data and the volume mapping

The chime PCM is in the ROM. A five-entry table at $408C5CF0 holds `{long byteLength; long
selfRelativeOffset}` pairs, the pointer resolving as `&offsetField + offset` [3] (*observed* in the
image; the entries below verified byte-for-byte):

| Index | Length (bytes) | Frames | PCM at file offset | Duration at 24 kHz | Used for |
|---|---|---|---|---|---|
| 0 | $00035D4C (220 492) | 55 123 | $C5D24 | 2.297 s | the startup chime |
| 1–4 | $0001E050 (122 960) | 30 740 | $FBA74 (all four) | 1.281 s | the death chimes — one sample, four call sites |

**Format: raw 16-bit signed big-endian, interleaved left/right, at 24 000 Hz, no header** [3] (*observed*
by decoding: the startup sample rises from silence with a smooth attack and a dominant zero-crossing
rate near 257 Hz; the error sample peaks near full scale around 1 kHz).

The volume mapping is 3 dB per user step, ROM-derived [3]: the attenuation nibble is (7 − volume) × 2
applied identically to both channels:

| PRAM volume | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| Attenuation nibble | $0 | $2 | $4 | $6 | $8 | $A | $C | $E |
| Level (dB) | 0 | −3 | −6 | −9 | −12 | −15 | −18 | −21 |

Volume 0 is therefore −21 dB, not silence. The PRAM bytes involved: XPRAM[$10] must hold $A8 (the
parameter-block validity signature) before the volume is trusted, otherwise volume 5 is used;
XPRAM[$08] bits 0–2 are the volume; XPRAM[$89] bit 1 selects chime variant 0 or 1 [3]. Which
user-visible setting corresponds to that last bit is unknown (§6.6).

One consequence of the loop arithmetic deserves a flag: the startup chime is 55 123 frames = 28.7
double buffers, and the loop counts 220 492 / 3840 = 57 half-buffers → 28 → minus one — 28 chained
advances after the initial arming, with teardown firing as the 29th pass begins. The sample's final
partial double buffer — 1 363 frames, about 57 ms — is thus never played (*inferred from the loop
arithmetic*; not verified against a real machine's audible chime, §6.12).

### 4.4 The DSP driver's codec protocol

The ROM-resident DSP driver is the production path, and its codec-facing surface is compact [3] (the
driver's full selector surface and boot sequence are [DSP board](dsp3210-board.md) §4; only the
Singer-side behavior is here):

- **Bring-up order.** The frame interrupt is *cleared* before the DSP leaves reset (bit 6 of sndComCtl,
  §2.2), the level-5 DSP and overrun sources are masked, the DSP bootstraps and runs, and only then does
  the driver program the buffers, the rate and the frame enable — in that order, with the rate and
  pFrmIntEn set in a single sndComCtl write [3] ([DSP board](dsp3210-board.md) §4.5, *observed*).
- **Buffer arming.** sndInBase and sndOutBase take the physical addresses of the two ring records the
  driver publishes to the DSP; sndSize is computed as sample rate divided by frame rate — the
  production value is 240 frames at every rate ([DSP board](dsp3210-board.md) §4.5) [3].
- **Rate selection** — §3.2's table: five accepted Fixed values, one rate code, one combined write;
  anything else is an error [3]. The choice persists in slot PRAM, record byte $06 bits 4–5, stored as
  index minus one, by the driver's save/restore pair [3] ($4099C040, $4099C0B0).
- **Mute is coupled to output DMA.** The driver's DMA-enable setter unmutes when pSndOutEn goes on and
  mutes when it goes off, in the same operation [3] ($4099CB38) — the click-free discipline of §3.4,
  enforced once instead of at every call site (*observed*).
- **The status-and-acknowledge handler.** One short routine refreshes the output port from singerStat
  bit 3 (the §2.4 decision) and acknowledges any pending codec status interrupt by reading L4IR, masking
  to bit 0 and writing the value back [3] ($4099BC90). It runs as part of the DMA-enable setter —
  "whenever you touch sound DMA, refresh the output port and clear any pending Singer status
  interrupt" is the driver's own rule, in code.
- **Volume, gain and input select** are field get/set calls on singerCtl: one entry addresses the two
  attenuation nibbles as a byte (mask $00000FF0, shift 4), one the two gain nibbles ($000FF000, 12),
  one the two mux bits ($00300000, 20) [3] ($4099CADA–$4099CAF6) — read-modify-write on the register
  image, with the codec following on the next frame's aux field.
- **The capture side** applies the gain and mux fields above and reports the input source from the
  three-way singerStat enumeration of §2.4; the DSP modules consume the captured stream ([DSP
  board](dsp3210-board.md) §3.8). The A/D-side engine cadence — what sndPhase reports and when the
  input half-buffers advance — is exercised by no code in this evidence set outside the DSP path
  (§6.13).

## 5. Quirks & errata

- **sndPhase must move, or the machine never boots.** The boot chime runs at IPL 7 with the caches off
  and spin-waits on the phase offset — one loop until it reads zero, the next until it reads non-zero.
  A constant value of either polarity hangs the ROM forever, with interrupts masked (§4.2, *observed*).
  This is the sound subsystem's one behavior that is fatal rather than merely audible.
- **The register images are the aux fields, cell-reversed.** singerCtl/singerStat bit *n* carries aux
  cell 24−*n* (§2.3, §2.4). A writer who lays the fields out in datasheet cell order — expand bit at
  bit 0 — sets the mute, inverts both muxes and scrambles every nibble.
- **One subframe field is a code, the other a mask.** pSubFrmInSel is a 2-bit binary number;
  pSubFrmOutSel is a 4-bit one-hot mask (§2.2). The $0104 enable constant is the proof — and a writer
  who treats the output field as a code selects the wrong subframe for every value past 1.
- **pMute resets the A/D valid-data counter.** Mute is not just an output control; it is the sanctioned
  way to flush the capture pipeline across a rate change [2] §3.1 p. 8 (§2.3).
- **pMute is software-coupled to pSndOutEn.** The driver unmutes when output DMA starts and mutes when
  it stops (§4.4) — so a driver that writes pMute directly will fight the ROM driver's discipline the
  next time it toggles DMA.
- **The expand bit poisons control, not audio.** A set pExpCtl raises error 1 and makes the codec
  ignore every other control cell, while audio keeps playing (§2.3, §3.3) — a silently
  un-programmable codec.
- **Two of the three error codes auto-mute.** Alternate-format sync and out-of-range serial clocks mute
  the analog output without any software action (§2.4) — and code 2 is exactly what a mis-shaped SSYNC
  produces [2] §3.1 p. 8.
- **The revision nibble must read zero.** "Set to 0000 for this part" [2] §3.1 p. 9 — a fabricated
  revision value is a nonconformance, and nothing reads it anyway (§2.4).
- **44.1 kHz is advertised but not a codec rate.** The developer note promises "the standard rate of
  44.1 kHz" [1] p. 6; the codec never runs at it — the DSP resamples (§3.2), and the data sheet's
  crystal-swap procedure [2] §2.8 p. 5 is never performed by shipped code.
- **32 kHz is reachable but unreached.** The rate field's code 1 divides the master clock by 3 and
  yields exactly 32 000 Hz, and the standard-sound software describes it as a current rate [1] p. 117 —
  but no shipped selector ever picks it ([DSP board](dsp3210-board.md) §4.5).
- **Volume 0 is −21 dB.** The ROM's own ladder gives the lowest user volume three and a half steps of
  attenuation, not silence (§4.3); a "silent" settings panel is not what the hardware does.
- **All four death chimes are the same sample.** The four error call sites share one 122 960-byte PCM
  block; only the startup chime has its own (§4.3).
- **The chime's own arithmetic drops its tail.** 28 chained double buffers cover 53 760 of the startup
  sample's 55 123 frames; the last ~57 ms is never played (§4.3, *inferred*).
- **One enable bit clocks two processors.** pFrmIntEn gates both the host's level-2 frame interrupt and
  the DSP's EXT1 pulse (§3.5) — see [DSP board](dsp3210-board.md) §3.4 for the teeth this gives the
  debugger interlock.
- **SNDSTAT is acknowledged by writing L4IR back.** Read, mask to bit 0, write the value — the codec
  status flag is cleared through the level register, not through any Singer-facing register (§3.5;
  [PSC](psc.md) §2.2).
- **The sound block has no sense bit.** Everything else in the PSC writes through a sense bit
  ([PSC](psc.md) §5); these registers take plain data. A driver that applies the PSC-wide convention
  here writes half the intended value.
- **Two register widths are strict.** sndComCtl and sndSize are words, the rest longwords, dspOverRun a
  byte — the ROM never deviates (§2.1), and a decode that answers only longwords breaks the chime's
  `CLR.W`.
- **"Frame 0" and "subframe 1" are the same thing.** The developer note counts the TDM slots from zero
  [1] p. 38, the data sheet from one [1] p. 44, [2] §3.1 — the system sound lives in the first
  subframe either way (§3.1).
- **Every shipped reader of singerStat reads at most three bits** — bit 3 and bits 2/0 — and nothing
  else in the register has a consumer anywhere in the shipped software set (§2.4). A register that
  returns zero in the unread fields is indistinguishable from a fully modeled one.

## 6. Open questions

1. **Is Singer a stock ASCO 2300?** The developer note says only that it "conforms to" the specification
   [1] p. 16; no Apple document gives a Singer part number, a package, or a supplier. The register
   decode here assumes the data sheet's part throughout.
2. **pComRate (sndComCtl bits 11–12).** Never written by any shipped code [3]; the communications
   codec's rate codes, its subframe assignment and its very identity on the AV board (the internal
   modem's codec is the natural candidate) are unattested outside the rate field's existence.
3. **pSndRate code 3.** Never used; ratio 6 → 16 000 Hz is the Table 2-1-consistent guess
   (*inferred*), and whether the PSC decodes the code at all is unverified.
4. **Where the subframe selects are first set for the DSP path.** The rate selector preserves bits 0–5
   and the boot chime leaves %0001/00 standing [3]; no shipped initializer writes them, and whether the
   DSP path relies on the chime's leftovers or on a reset default is unestablished.
5. **sndPhase bits 0–5.** The 1/64-frame scale is inferred from the `sndSize << 6` status call (§2.5);
   nothing reads the fractional field, and its true weight and update rate are unverified.
6. **XPRAM[$89] bit 1.** The chime-variant selector (§4.3); which user-visible setting — if any —
   controls it is unknown.
7. **The BI/BO pin numbering conflict.** The datasheet cell correspondence puts BI1/BO1 at register
   bit 3 and BI4/BO4 at bit 0 (§2.4); the shipped software's own register definitions, as reflected in
   the disassembly's annotations, label the ports bit-0-first. No shipped code distinguishes the pins
   individually, and no schematic is in the evidence set to settle it.
8. **PSC + $6004** — the boot chime's opening `CLR.L $50036004` (§4.2) aliases into the PSC's I/O
   space; two ROM sites touch it and no document or equate names a function. Harmless to accept, but
   unexplained.
9. **The 22.5792 MHz clock family.** Table 2-2 labels C22_5792M "Singer (44.1 KHz)" [1] p. 17, but no
   shipped code switches the codec's master clock — 44.1 kHz is DSP-resampled (§3.2). Whether the
   board can run the codec natively at 44.1 kHz, and what would perform the data sheet's
   switch-and-reset procedure [2] §2.8 p. 5, is unknown; likewise how, or whether, ATECS's external
   telecom clock ever disciplines the sound subsystem (§1.5).
10. **Frame-mode strapping.** The codec supports 64- and 32-bit frame formats under MODESEL high
    [2] Table 3-4 p. 12; the AV board uses the 256-bit mode only (§3.1), and the strap values are not
    directly observed anywhere in the evidence set.
11. **singerStat's unread fields.** pValidData, pRevNum, pSingerStatus and the overflow bits have no
    shipped reader (§2.4); their timing — especially the valid-data latency after mute and rate
    changes — is unverified against silicon, and the data sheet's timing diagrams are image-only in
    the available scan (§5 of [2], pp. 24–26).
12. **The chime's dropped tail.** The loop arithmetic leaves the startup chime's final 1 363 frames
    unplayed (§4.3, *inferred*); whether the shipped sample was authored to end inside the dropped
    region — or the chime genuinely ends 57 ms early on real hardware — is untested.
13. **The capture side's engine cadence.** sndInBase is armed by the DSP driver but every observed
    exercise of the engine is either an output path or DSP-mediated (§4.4); the input half-buffer
    boundaries, and whether sndPhase reflects the input stream at all, are unobserved (see also
    [PSC](psc.md) §6, its fourteenth open question).
14. **Mid-pass base writes.** The once-per-pass latch point is inferred from the chime's chaining
    (§2.6); whether a base write landing mid-pass is sampled at the half-buffer boundary, the pass
    boundary, or immediately, is unobservable from shipped code.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Compatibility Issues" p. 7 (the API-only rule); §"Summary of
   Features" p. 6 (the 44.1 kHz claim); Chapter 2: §"Peripheral Subsystem Controller" p. 13 (the PSC
   function list, the 16-byte Singer buffers), §"Singer" p. 16, §"Apple Telecom External Clock
   Synchronizer" p. 16, §"System Clocks" Table 2-2 p. 17 (C22_5792M, C24_576M, ATECS clocks),
   §"PSC Functions" Table 2-10 p. 29 (SndIn/SndOut channel width and FIFO) and Table 2-11 p. 30
   (sound channels' CPU-bus priority), §"Sound I/O" p. 38 ("frame 0", Table 2-15 panel levels, SNR
   and THD figures), §"DAV Connector" pp. 42–43 (Table 2-19: SingerSync/SingerSerOut/SingerBitClk/
   SingerSerIn/SingerMClk pins), §"DAV Sound Interface" pp. 44–45 (the TDM statement, Table 2-20,
   the 256-bit frame and subframe structure, Figures 2-15 to 2-17, the clock edges and sync widths);
   Chapter 3: §"Standard Sound" pp. 116–118 (the sound task list, the 240-longword buffers, the
   current rates), §"Sample Rate and Frame Rate Changes" pp. 121–122 (the three gear shifts).
2. Micronas Intermetall GmbH (formerly ITT Semiconductors), *ASCO 2300 Audio Stereo Codec*, final data
   sheet, 3rd release, February 8, 1995, order no. 6251-333-3DS — §1 p. 3 (introduction, S/N, THD,
   the serial-port protocol summary); §2.4 p. 3 (output mute); §2.6 p. 4 (valid data indicator);
   §2.7 p. 4 (automatic sampling rate detection); §2.8 p. 5 (other sampling rates); §2.9–§2.12
   pp. 5–6 (PDM converters, decimation and interpolation filters, D/A section); Table 2-1 p. 7 (CLOCK
   to SCLOCK ratios); §3.1 pp. 7–12 (Full Feature format: the four-wire port and sync rules p. 8,
   the auxiliary control field p. 8, the auxiliary status field and input/output port cells p. 9,
   Table 3-1 subframe structure p. 10, Tables 3-2/3-3 gain and attenuation ladders p. 11, Table 3-4
   frame modes and strapping p. 12); §4.2 p. 14 (pin connections); §4.5.3 p. 21 (analog levels);
   §5 pp. 24–26 (timing specifications).
3. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, release $10F3, image checksum
   $5BF10FD1, mapped at $40800000; CPU address = $40800000 + file offset) — full disassembly and data
   analysis. Cited sites: the boot-chime routine at $408C5BB0 with its callers ($408C1972 boot,
   $408A8860/$408A8924/$408A896C/$408A89AC death chimes), its register writes ($408C5C4C sndComCtl
   $0000, $408C5C50 singerCtl $0045500F, $408C5C58 sndSize $03C0, $408C5C78 sndOutBase, $408C5C88
   sndComCtl $0104, $408C5C8E–$408C5CAC the unmute with the singerStat bit-3 test, $408C5CB0/$408C5CC0
   the sndPhase spin loops, $408C5CE0 teardown $0004) and its five-entry sample table at $408C5CF0
   with PCM at file offsets $C5D24 and $FBA74; the ROM-resident DSP driver at $4099B190–$4099CFA0:
   StartFramesRoutine $4099BCC0 with the sndSize division $4099BD0E–$4099BD58 and the rate ladder
   $4099BD7C–$4099BDFA (the $01BF preserve-mask and the $0040/$0240/$0440 rate-plus-frame writes);
   SaveSampleRate $4099C040 and RestoreSampleRate $4099C0B0 (slot PRAM record byte $06 bits 4–5);
   the status-and-acknowledge handler $4099BC90; the DMA-enable setter $4099CB38 (mute coupled to
   pSndOutEn); the field get/set entries $4099CADA/$4099CAE8/$4099CAF6; the input-source enumeration
   $4099CAAA–$4099CACE; the `sndSize << 6` status call $4099C6A6; the frame-interrupt enable/disable
   pair $4099C82A–$4099C87A; the DSP-task device-name table $4099C310 ("SingerDMAInput",
   "SingerDMAOuput").
4. Apple Computer, Inc., System 7.1 System file and System Enabler 088 (System 7.1 enabler for the
   Macintosh Quadra 840AV and Macintosh Centris 660AV, version 1.0, 1993) — resource inventory,
   disassembly and exhaustive byte-scan (2 518 files, 22 MB) for the PSC sound-block addresses
   ($50F31000, $50F312xx), the init constants ($0045500F, $0005500F, $0104) and the block's register
   offsets: zero hits. The sound components found instead: the `'sdev'`/`'dsp '` component pair
   ("Built-in", whose code is entirely `_DSPDispatch` calls) and the `'dspf'` "AppleSRC" rate-converter
   module.
5. Apple Computer, Inc., Apple Sound Chip data sheet, 1986 — scanned copy, no text layer; the sound
   hardware of the pre-AV Macintosh line that Singer replaces. The ASC/EASC lineage is
   [ASC](../../hardware/asc.md) §2.3's subject and is not restated here.
6. Apple Computer, Inc., Enhanced Apple Sound Chip (EASC, 343S1036) data sheet — scanned copy, no text
   layer; same role as [5] for the 68040-Quadra generation's sound chip.
