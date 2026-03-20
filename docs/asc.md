# Apple Sound Chip (ASC)

Comprehensive technical reference for the Apple Sound Chip as used in the
Macintosh SE/30, IIx, and IIcx. This document covers the chip's history,
silicon variants, internal architecture, register programming interface,
FIFO and wavetable operating modes, interrupt routing, analog output chain,
initialization sequences, and behavioral details relevant to emulator
implementation.

Primary sources:
- *Guide to the Macintosh Family Hardware* (2nd Edition, 1990)
- *Macintosh Hardware Overview* (Rev 2.0, February 1991)
- System 7.1 ROM source code (reverse-engineered)
- Doug Brown's empirical hardware testing on real Mac IIci hardware (2011)
- Linux kernel `mac_asc.h` / `macboing.c`
- NetBSD `ascreg.h`

---

## 1. Historical Background

### 1.1 The Problem: CPU-Bound Sound

Before the Macintosh II, all Macintosh audio was produced by a software-
intensive, interrupt-driven mechanism tied to the video horizontal blanking
interval. A 370-byte buffer in main DRAM was scanned once per video frame:
the general logic IC (PAL in the Mac Plus, BBU in the Mac SE) would halt the
CPU, fetch one word from the buffer during each horizontal blanking interval,
and pulse-width-modulate the high byte into a signal sent to a single Sony
sound chip. The resulting sample rate was locked at ~22,254.5 Hz (370
samples × 60.147 frames/sec). The low byte of each word controlled floppy
disk motor speed for single-sided 400 KB drives.

This architecture had severe limitations: the audio sample rate was
inseparable from video timing, polyphonic sound required monopolizing CPU
cycles on the 68000, and the shared sound/video RAM access window created
unavoidable contention.

### 1.2 The Apple Sound Chip Initiative

The Apple Advanced Technology Group developed a dedicated audio ASIC to
equip the forthcoming Macintosh II family with multimedia capabilities. The
engineering effort involved Pete Foley, Mark Lentczner, Bob Bailey, Steve
Milne, Dave Wilson, and Wil Oxford. Internally referred to as the "Foley
Sound Chip," the ASC was one of the first chips at Apple to use VLSI
Technology's standard cell methodology and their off-the-shelf SRAM compiler
tool, fabricated in a double-metal, single-polysilicon process. The SRAM is
synthesized on-die, not an external component.

The Macintosh Hardware Overview (Feb 1991) describes the ASC as enhancing
"Classic Mac sound by implementing in hardware much of what was done in ROM
code on the earlier machines" and adding "a four-voice synthesizer
(Wavetable) mode." Stereo operation is provided by "doubling up the buffers
and sound processing hardware."

During development, the team digitized Peter Gabriel's song "Red Rain" in
its entirety, streaming it from the largest hard drives available to
demonstrate the chip's ability to sustain high-quality PCM playback without
degrading system responsiveness. The recording was ultimately replaced by
royalty-free compositions before product launch.

### 1.3 Machines That Use the ASC

The discrete ASC is present in:
- Macintosh II (68020, original ASC introduction)
- Macintosh IIx (68030)
- Macintosh IIcx (68030)
- Macintosh SE/30 (68030)
- Macintosh IIci (68030)
- Macintosh IIfx (68030)
- Macintosh Portable (68HC000)

The IIx, IIcx, and SE/30 share the same 256 KB ROM (ID `$0178`), the same
15.6672 MHz 68030 CPU, the same GLUE ASIC (344S0602), and the same set of
support chips including the ASC. For emulation purposes these three machines
are essentially the same hardware platform with different form factors and
expansion options.

---

## 2. Silicon Variants and Chip Identification

### 2.1 Part Numbers

| Part number | Description |
|---|---|
| 344S0053 | Original ASC — full feature set |
| 344S0063 | Cost-reduced ASC — identical register map and software compatibility |

Both versions contain the complete FIFO and 4-voice wavetable synthesizer.
The cost-reduced 344S0063 differs only in one observable way: bits 2–4 of
the VOLUME register (`ascVolControl`, offset `0x806`) always read as zero,
whereas on the 344S0053 they retain written values. The ROM POST exploits
this to distinguish chip steppings at runtime:

```
move.b  #$1C, ascVolControl(a1)   ; write bits 2,3,4
move.b  ascVolControl(a1), d3     ; read back
andi.b  #$1C, d3
bne.s   @GoTest                   ; non-zero → old silicon (344S0053)
lea     sndrnnew, a4              ; zero → new silicon (344S0063)
```

### 2.2 VERSION Register Detection

The VERSION register at offset `0x800` from the ASC base identifies the chip
variant. Only the upper nibble is significant for type selection:

```
move.b  ascVersion(a1), d3        ; read VERSION byte
and.b   #$F0, d3                  ; keep upper nibble only
cmp.b   #batmanSound, d3          ; $B0?
```

| Upper nibble | Raw byte (typical) | Equate | Chip |
|---|---|---|---|
| `$00` | `$00` | — | Standard ASC (344S0053 / 344S0063) |
| `$B0` | `$B8` | `batmanSound` | EASC "Batman" (343S1036) |
| `$E0` | `$E8` | `elsieSound` | V8 / Eagle integrated ASC (LC, Classic II) |

The SE/30, IIx, and IIcx all return `$00`. Variants in the Batman family
(nicknamed "BatRats") may return `$B1`–`$BF`; the `$F0` mask normalizes
them. The `wombatSound` equate names one Sonora-based derivative.

### 2.3 Successor Chips

#### EASC "Batman" (343S1036)

The Enhanced Apple Sound Chip was introduced with the Quadra 700/900/950
(68040). It is pin-compatible with the ASC and maintains base register
compatibility, but makes significant architectural changes:

- **Wavetable mode completely removed** — writing to wavetable registers
  produces no audible output. NetBSD developers confirmed that Quadra 700s
  produce "strange clicks/creak instead of beeps" when code attempts
  wavetable mode.
- Native 16-bit stereo output via serial interface.
- 8-bit sound input capability (one channel, using one of the on-chip FIFOs).
- Hardware sample rate conversion (SRC).
- Hardware CD-XA (ADPCM) decompression.
- Per-channel left/right volume registers.
- Improved interrupts and 10-bit PWM resolution.
- Extended registers at offsets `0xF00`–`0xF1F`.

The Macintosh Hardware Overview notes: "The wavetable mode is not supported
in Batman, Elsie, and future sound hardware on the Macintosh. The ASC
wavetable mode is rarely used, since it doesn't allow synchronized amplitude
changes."

#### Integrated System Controller Clones

To reduce cost in consumer machines, Apple integrated ASC-compatible sound
circuitry into multi-function system ASICs:

| Chip | Codename | Part number | Used in |
|---|---|---|---|
| V8 | V8 | 343S0116-A | Macintosh LC, LC II |
| Eagle | Eagle | 343S1054-A | Macintosh Classic II |
| Spice | Spice | 343S0132 | Color Classic (Performa 250) |
| Sonora | Sonora | 343S1065 | Macintosh LC III, LC 520, Mac TV |

Key differences from the discrete ASC:

- **No on-chip SRAM** — the FIFO buffer resides in main system DRAM and is
  accessed via DMA, reverting to a design reminiscent of the original Mac
  Plus architecture.
- **No wavetable synthesis** — mode 2 is non-functional.
- Registers present a software-compatible facade so the Sound Manager
  addresses them identically to a discrete ASC.
- Monaural output only on most models.
- The half-empty interrupt on Sonora-class chips uses **level-sensitive
  threshold comparison** (`count < 512`) rather than the discrete ASC's
  latch-on-transition behavior (see §6.4).

#### Feature Comparison Matrix

| Feature | ASC (344S0063) | EASC (343S1036) | Integrated (V8/Eagle/Sonora) |
|---|---|---|---|
| VERSION register | `$00` | `$B0` | `$E8` (V8/Eagle), `$BC` (Sonora) |
| Wavetable mode | Yes (4-voice) | **No** | **No** |
| FIFO mode | Yes | Yes | Yes (DMA from main RAM) |
| On-chip SRAM | 2 KB | 2 KB | **No** (main DRAM) |
| Sample rates | 22,257 / 22,050 / 44,100 Hz | Arbitrary via SRC | Typically 22,257 Hz |
| Extended registers | No | Yes (0xF00–0xF1F) | No |
| Half-empty IRQ | Latch-on-transition | Latch-on-transition | Level-sensitive (Sonora) |
| Sound input | No | Yes (1 channel) | Some models |
| Stereo output | Yes | Yes | Usually mono |

---

## 3. Internal Architecture

### 3.1 SRAM Organization

The ASC contains **2,048 bytes (2 KB) of internal SRAM**, organized as four
512-byte physical banks that are logically reconfigured depending on the
operating mode.

The *Guide to the Macintosh Family Hardware* (2nd ed.) states: "The ASC has
four 512-byte buffers of its own, so computers that use it do not use system
RAM for storing sound values." The *Macintosh Hardware Overview* describes
"two 1-kilobyte memory arrays" — the same 2 KB viewed from the FIFO-mode
perspective where four physical 512-byte banks are paired into two 1 KB
channels.

The CPU can address the full 2 KB range from offset `0x000` to `0x7FF`
relative to the ASC base address. Control registers begin at offset `0x800`.

### 3.2 Address Space Layout

| Offset range | Function |
|---|---|
| `0x000`–`0x3FF` | FIFO A / Wavetable voices 0–1 (1,024 bytes) |
| `0x400`–`0x7FF` | FIFO B / Wavetable voices 2–3 (1,024 bytes) |
| `0x800`–`0x82F` | Control and status registers |
| `0x830`–`0x8FF` | Partially documented (ROM writes `0xFE` to `0x830`–`0x837`) |
| `0xF00`–`0xF1F` | Extended registers (EASC only; not present on discrete ASC) |

Addresses `0x400`–`0x7FF` are **fully decoded**, not a mirror of
`0x000`–`0x3FF`. They serve as FIFO B in FIFO mode and wavetable voices
2–3 in wavetable mode.

### 3.3 Memory Mapping in the Macintosh Address Space

On the SE/30, IIx, and IIcx, the ASC is memory-mapped at physical address
**`0x50F14000`**. The system stores this address in the low-memory global
`ASCBase` at address `$0CC0` and also in the `DecoderInfo` record's
`ASCAddr` field. All Sound Manager code reads `ASCBase` to locate the chip.

The I/O space on these machines mirrors every `0x20000` bytes due to
incomplete address decoding in the GLUE ASIC. The canonical ASC base within
the I/O space is at offset `0x14000` from `0x50F00000`.

| I/O device | Offset | Physical address |
|---|---|---|
| VIA1 | `0x00000` | `0x50F00000` |
| VIA2 | `0x02000` | `0x50F02000` |
| SCC | `0x04000` | `0x50F04000` |
| SCSI (normal) | `0x10000` | `0x50F10000` |
| **ASC** | **`0x14000`** | **`0x50F14000`** |
| SWIM | `0x16000` | `0x50F16000` |

Note: the IIfx maps the ASC at `0x50F10000` instead — but the IIfx is a
different hardware platform and not covered in detail here.

### 3.4 The 8-Bit Data Bus and Dynamic Bus Sizing

The physical data bus connecting the ASC to the logic board is **8 bits
wide**. When the 68030 CPU executes a 32-bit longword write (MOVE.L)
targeting the ASC's buffer space, the GLUE ASIC's address decoding circuitry
responds with DSACK signals indicating an 8-bit peripheral. The CPU
autonomously breaks the single 32-bit transfer into four consecutive 1-byte
write cycles, automatically incrementing the target address for each byte.

This allows highly optimized 32-bit unrolled copy loops to fill the ASC
buffers rapidly, maximizing bus throughput despite the narrow physical
interface.

### 3.5 The Analog Output Chain: Sony Sound Chips

The ASC does not contain a traditional Digital-to-Analog Converter. It
generates a high-frequency digital bitstream using Pulse-Width Modulation
(PWM). This PWM signal is fed to external **Sony sound chips** (Apple part
number 343-0045, Sony OEM part CX1063AP / 3430045B), which serve as active
integrators, low-pass filters, and voltage amplifiers. Two Sony chips are
present in the SE/30, IIx, and IIcx — one per stereo channel.

The Sony chip also provides a power-on detection circuit and a 250 mW
speaker amplifier driving the internal 32-ohm, 2.25-inch speaker.

#### Stereo Routing

- **IIx and IIcx**: The internal speaker is connected to the left channel
  only. A VIA2 input bit indicates whether a plug is inserted in the stereo
  headphone jack. When using the internal speaker, the Sound Manager sends
  all audio through the left channel. When an external plug is detected, the
  Sound Manager switches to true stereo.

- **SE/30**: The Sound Manager **always operates in stereo mode**
  (`ascChipControl = 2`, bit 1 set). The analog circuitry on the logic
  board uses a dedicated hardware amplifier to sum the left and right
  channels to mono for the internal speaker, while maintaining stereo
  separation on the external headphone jack.

#### External Sound Jack

The external sound jack provides approximately 1.5 volts peak-to-peak at a
source impedance of ~47 ohms. It can drive headphone loads of 8 to 600
ohms or the input of most audio amplifiers. It is short-circuit protected.
The Control Panel offers eight choices (0 through 7) for overall volume.
When volume is set to 0, the Sound Manager disables sound output and
flashes the menu bar instead.

---

## 4. Complete Register Map

All offsets are relative to the ASC base address (`0x50F14000` on
SE/30/IIx/IIcx). Register names in the "Apple equate" column are from
Apple's internal System 7.1 ROM source code.

### 4.1 SRAM / Buffer Region

| Offset | Apple equate | Width | Description |
|---|---|---|---|
| `0x000`–`0x3FF` | `ascFifoLeft` | 1024 bytes | FIFO A buffer / Wavetable voices 0–1 |
| `0x400`–`0x7FF` | — | 1024 bytes | FIFO B buffer / Wavetable voices 2–3 |

In FIFO mode, the address within each 1 KB range is irrelevant — all writes
to `0x000`–`0x3FF` feed FIFO A's circular buffer regardless of the specific
address. All writes to `0x400`–`0x7FF` feed FIFO B. The chip uses internal
write/read pointers with `& 0x3FF` wrapping.

In wavetable mode, addresses are directly mapped to specific SRAM locations,
so the CPU can load waveform data into precise positions:
- Voice 0: `0x000`–`0x1FF` (512 bytes)
- Voice 1: `0x200`–`0x3FF` (512 bytes)
- Voice 2: `0x400`–`0x5FF` (512 bytes)
- Voice 3: `0x600`–`0x7FF` (512 bytes)

### 4.2 Control Registers

| Offset | Apple equate | Width | R/W | Description |
|---|---|---|---|---|
| `0x800` | `ascVersion` | byte | R | VERSION — chip type identifier. Upper nibble: `$00` = ASC, `$B0` = EASC, `$E0` = V8/Eagle |
| `0x801` | `ascMode` | byte | R/W | MODE — `0` = off, `1` = FIFO, `2` = wavetable |
| `0x802` | `ascChipControl` | byte | R/W | CONTROL — bit 0: PWM/analog select; bit 1: stereo enable; bit 7: processing time exceeded flag |
| `0x803` | `ascFifoControl` | byte | R/W | FIFO MODE — bit 7: FIFO clear toggle; bit 1: non-ROM companding enable; bit 0: ROM companding enable |
| `0x804` | `ascFifoInt` | byte | R | FIFO IRQ STATUS — **read-clears** all interrupt flags (see §6.3) |
| `0x805` | `ascWaveOneShot` | byte | R/W | WAVETABLE CTRL — bits 0–3 trigger playback of voices 0–3 |
| `0x806` | `ascVolControl` | byte | R/W | VOLUME — bits 5–7: 3-bit volume level (0–7) for PWM / Sony control; bits 0–4: Sony analog control lines |
| `0x807` | `ascClockRate` | byte | R/W | CLOCK RATE — `0` = 22.257 kHz, `2` = 22.050 kHz, `3` = 44.100 kHz |
| `0x80A` | `ascPlayRecA` | byte | R/W | Channel A play/record mode |
| `0x80B` | `ascPlayRecB` | byte | R/W | Channel B play/record mode |
| `0x80F` | `ascTestReg` | byte | R/W | TEST — bits 6–7: digital test mode; bits 4–5: analog test mode |

### 4.3 Wavetable Phase Registers

Each voice has a 32-bit register pair (only 24 bits valid), stored
big-endian, encoding a 9.15 fixed-point phase value:

| Offset | Register | Description |
|---|---|---|
| `0x810` | WAVE 0 PHASE | Current phase accumulator for voice 0 |
| `0x814` | WAVE 0 INCR | Frequency increment for voice 0 |
| `0x818` | WAVE 1 PHASE | Current phase accumulator for voice 1 |
| `0x81C` | WAVE 1 INCR | Frequency increment for voice 1 |
| `0x820` | WAVE 2 PHASE | Current phase accumulator for voice 2 |
| `0x824` | WAVE 2 INCR | Frequency increment for voice 2 |
| `0x828` | WAVE 3 PHASE | Current phase accumulator for voice 3 |
| `0x82C` | WAVE 3 INCR | Frequency increment for voice 3 |

### 4.4 ascPlayRecA / ascPlayRecB Encoding

| Value | Meaning |
|---|---|
| `$00` | Play mode; RFD (Record FIFO Data) disabled |
| `$01` | Record mode; RFD disabled |
| `$02` | Play mode at 22 kHz — used in Batman init only |
| `$81` | Record mode + RFD enabled — used in sound-input FIFO test |

For the SE/30, the ROM always writes `$00` (plain play) or `$01` (record).
Value `$02` appears only in the Batman-specific hardware initialization
path.

### 4.5 Volume Register Detail

Volume is a **0–7 integer** stored in bits 5–7 only:

```asm
ASL.B   #5, D0              ; shift 3-bit volume into bits 5-7
MOVE.B  D0, ascVolControl   ; write $00/$20/$40/$60/$80/$A0/$C0/$E0
```

Maximum value is `$E0` (volume = 7). Bits 0–4 drive the external Sony
analog chips — the upper 3 bits of the hardware register map directly to the
Sony volume control lines.

The hybrid digital/analog volume architecture avoids pure digital
attenuation, which would reduce effective bit-depth and introduce
quantization noise at low levels. By shifting attenuation to the analog
domain, the full 8-bit dynamic range is preserved even at quiet settings.

---

## 5. Operating Modes

### 5.1 Mode 0: Off

When `ascMode = 0`, the chip is inactive. No output is produced and no
interrupts are generated. This is the power-on default state.

### 5.2 Mode 1: FIFO (Sample Playback)

This is the standard operating mode used by the Sound Manager for all
digitized sound playback — system alerts, application audio, and sampled
sound resources.

#### Buffer Configuration

The 2 KB SRAM is partitioned into **two 1,024-byte circular FIFO buffers**:

| Buffer | Address range | Channel |
|---|---|---|
| FIFO A | `0x000`–`0x3FF` | Left |
| FIFO B | `0x400`–`0x7FF` | Right |

A critical behavioral detail: **in FIFO mode, the address within each
channel's range is irrelevant**. A read or write anywhere in `0x000`–`0x3FF`
reads/writes the next byte in FIFO A's circular buffer. The hardware
maintains internal read and write pointers that wrap at `& 0x3FF`,
producing 1,024-byte circular queues. The CPU does not need to track
specific addresses within the FIFO — any write to the channel's address
range enqueues a byte.

The System 7.1 source code defines `ascFifoLeft = 0x000` and
`ascFifoRight = 0x200`. The `0x200` value corresponds to the wavetable
voice stride (each voice buffer is 512 bytes), not a FIFO boundary; the
Sound Manager writes to these addresses out of convention, but the hardware
ignores the specific offset within the range.

#### Data Format

The CPU writes **8-bit offset-binary PCM** data to the FIFO buffers. The
ASC drains the data at the configured sample rate and converts each byte to
a PWM pulse, which is then filtered by the external Sony chips.

#### Sample Rates

| `ascClockRate` value | Sample rate | Notes |
|---|---|---|
| `0` | 22,257 Hz | Native Mac rate (backward compatible with Mac Plus) |
| `2` | 22,050 Hz | CD standard ÷ 2 |
| `3` | 44,100 Hz | CD standard |

The ASC lacks an internal sample-rate conversion engine. The Sound Manager
is responsible for resampling all audio (e.g., 11 kHz MACE compressed
audio) up to the ASC's native rate before writing to the FIFO.

#### Interrupt-Driven Refill

When a FIFO drains to the half-empty point (512 bytes remaining), the ASC
asserts an interrupt to request a refill. The Sound Manager services this
interrupt and writes the next block of samples, ensuring continuous gapless
playback. See §6 for full interrupt details.

#### Mono vs Stereo

- **Mono** (`ascChipControl` bit 1 = 0): Only FIFO A is used. Its output
  is sent to both Sony chips.
- **Stereo** (`ascChipControl` bit 1 = 1): FIFO A drives the left channel,
  FIFO B drives the right. On the SE/30, stereo mode is always used; the
  board hardware sums channels to mono for the internal speaker.

### 5.3 Mode 2: Four-Voice Wavetable Synthesis

One of the most distinctive features of the original ASC (absent from the
EASC and all integrated clones). While largely unused by application
software, it was heavily utilized by ROM routines for the **startup chime**
and the **Chimes of Death** (fatal POST error tones).

#### Buffer Configuration

The 2 KB SRAM is partitioned into **four 512-byte wavetable buffers**:

| Voice | Address range | Stereo assignment |
|---|---|---|
| Voice 0 | `0x000`–`0x1FF` | Left |
| Voice 1 | `0x200`–`0x3FF` | Left |
| Voice 2 | `0x400`–`0x5FF` | Right |
| Voice 3 | `0x600`–`0x7FF` | Right |

Software uploads a single cycle of a waveform (sine wave, square wave, or
sampled instrument snippet) into each 512-byte table. The ASC then uses
hardware phase accumulators to loop these waveforms continuously.

In stereo wavetable mode, voices 0–1 are summed and sent to the left
channel; voices 2–3 are summed and sent to the right channel. In mono
wavetable mode, all four voices are summed and sent to both channels.

#### Phase Accumulator and Frequency Control

Each voice has a **9.15 fixed-point phase accumulator** stored as a
big-endian 32-bit register (only 24 bits are valid):

- **9 integer bits** (bits 23–15): index into the 512-byte wavetable
- **15 fractional bits** (bits 14–0): sub-sample precision for fine
  frequency tuning

The ASC continuously adds the **increment** register to the **phase**
register on every clock cycle. The sample lookup extracts the 9 integer bits
using `(phase >> 15) & 0x1FF`, producing an index from 0 to 511. The
wavetable address for a given voice is:

```
address = 0x200 * voice_number + ((phase >> 15) & 0x1FF)
```

#### Frequency Calculation

The frequency produced by a voice is:

```
frequency = (increment_integer_part × sample_rate) / 512 Hz
```

Or more precisely, considering the full fixed-point increment:

```
frequency = increment × sample_rate / (512 × 32768)
```

where `increment` is the raw 24-bit value and `sample_rate` is 22,257 Hz.

ROM disassembly of the Mac IIci boot chime confirms this formula, producing
startup tones at approximately 130, 174, and 261 Hz.

#### Triggering Playback

Writing to `ascWaveOneShot` (offset `0x805`) with bits 0–3 set triggers
playback of the corresponding voices 0–3.

#### Limitations

The wavetable mode does not allow synchronized amplitude changes — once a
voice is playing, its volume cannot be smoothly modulated without software
intervention to rewrite the wavetable data. This limitation is cited by the
Hardware Overview as the reason the mode was "rarely used" and ultimately
removed from the EASC.

### 5.4 Sound Manager Deprecation of Wavetable Mode

With Sound Manager 3.0, Apple introduced a component-based audio
architecture. When older applications request square-wave or wavetable
synthesis via legacy Sound Manager commands, the OS no longer engages the
ASC's wavetable hardware. Instead, it digitally synthesizes the requested
waveforms into a PCM stream in main memory and feeds it to the ASC via FIFO
mode. The hardware wavetable feature is thus vestigial for application
software, used only by low-level ROM routines during boot (before the Sound
Manager initializes).

---

## 6. Interrupt Architecture

### 6.1 VIA2 CB1 Connection

The ASC hardware interrupt line connects directly to **VIA2 CB1** — not
VIA1, and not a NuBus slot interrupt line. This is confirmed by the System
7.1 source:

```asm
jASCInt  equ  Via2DT + 4*ifCB1    ; InterruptHandlers.a
```

`ifCB1 = 4`, so the sound interrupt dispatch entry lives at `Via2DT + 16`.

### 6.2 Enable / Disable / Clear at VIA2

VIA2 is a 6522-compatible chip. Bit 4 of the IER/IFR corresponds to CB1.

| Operation | Register | Value written | Effect |
|---|---|---|---|
| Enable | VIA2 IER | `$90` = `(1<<7)\|(1<<4)` | Set bit 4, direction = enable |
| Disable | VIA2 IER | `$10` = `(0<<7)\|(1<<4)` | Set bit 4, direction = disable |
| Clear | VIA2 IFR | `$90` or `$10` | Clear the CB1 flag |

From the System 7.1 source:

```asm
VIAEnableSoundInts:
    move.b  #(1<<ifIRQ)|(1<<ifCB1), Rv2IER(a0)   ; $90

VIADisableSoundInts:
    move.b  #(0<<ifIRQ)|(1<<ifCB1), Rv2IER(a0)   ; $10

VIAClearSoundInts:
    move.b  #(1<<ifIRQ)|(1<<ifCB1), Rv2IFR(a0)   ; $90
```

### 6.3 FIFO IRQ STATUS Register (ascFifoInt, offset 0x804)

| Bit | Meaning |
|---|---|
| 0 | Channel A half-empty (half of FIFO consumed) — refill interrupt |
| 1 | Channel A full / overflow |
| 2 | Channel B half-empty — refill interrupt |
| 3 | Channel B full / overflow |

**Critical behavior: reading this register clears all pending interrupt
flags.** The ROM always performs a bare read (`tst.b ascFifoInt(aX)`) purely
for this side-effect clear, discarding the value. An emulator must clear the
register contents on every CPU read.

### 6.4 Half-Empty Interrupt Semantics

The half-empty interrupt threshold is at **512 bytes** — exactly half of the
1,024-byte FIFO depth.

**On the original discrete ASC** (SE/30, IIx, IIcx, IIci), the half-empty
bit uses **latch-on-transition** semantics:
- The bit stays at 0 initially.
- It only becomes 1 after the FIFO has been filled past the halfway point
  and then **drains back down** through the 512-byte threshold.
- Reading the status register clears the bit (read-once behavior).
- The bit does not re-assert until the FIFO count crosses the threshold
  again going downward.

This is a **transition-detection** design, not a simple level comparator.

Doug Brown's empirical testing on real Mac IIci hardware (2011) confirms
this: code that worked on the LC III (Sonora, which uses a simple level
comparator) initially failed on the IIci because of this behavioral
difference.

**On Sonora and other integrated clones**, the same bit reads `1` anytime
the FIFO contains fewer than 512 bytes — acting as a plain threshold
comparator.

### 6.5 Interrupt Acknowledgment Lifecycle

The OS clears the interrupt at two levels, in order:

1. **Clear at VIA2** — write to VIA2 IFR to clear the CB1 flag.
2. **Clear at the ASC** — read `ascFifoInt` to clear the chip's internal
   interrupt flags and deassert CB1.

The full lifecycle during normal FIFO playback:

```
1. CPU writes samples into FIFO (addresses 0x000–0x3FF or 0x400–0x7FF)
2. ASC drains FIFO at the configured sample rate (22.257 kHz default)
3. When FIFO reaches half-empty threshold:
     ascFifoInt bit 0 (ch A) or bit 2 (ch B) set to 1
     ASC asserts VIA2 CB1 (active low edge)
4. If VIA2 IER bit 4 enabled:
     VIA2 asserts IRQ line → CPU receives Level 2 interrupt
5. Interrupt handler calls ClearSoundInt → VIA2 IFR bit 4 cleared
6. Interrupt handler reads ascFifoInt → register cleared, CB1 deasserted
7. Interrupt handler refills the empty half of the FIFO
```

An emulator should only assert a new VIA2 CB1 edge when the ASC state
machine next crosses the half-empty threshold going downward.

### 6.6 Interrupt Priority

The ASC interrupt flows through VIA2, which maps to **CPU interrupt priority
level 2** via the GLUE ASIC:

| IPL level | Source | Description |
|---|---|---|
| 1 | VIA1 | Timers, VBL, ADB, 1-second tick |
| 2 | VIA2 | NuBus slots, SCSI, **ASC** |
| 4 | SCC | Serial communications (LocalTalk/AppleTalk) |
| 7 | NMI | Programmer's interrupt switch |

---

## 7. FIFO Clear Protocol

The FIFO is cleared by **toggling** bit 7 of `ascFifoControl` (offset
`0x803`). The ROM always performs this as two separate writes:

```asm
ORI.B   #$80, ascFifoControl(A3)    ; assert clear strobe (bit 7 = 1)
ANDI.B  #$7F, ascFifoControl(A3)    ; deassert clear strobe (bit 7 = 0)
```

Or equivalently:

```asm
bset.b  #7, ascFifoControl(a4)
bclr.b  #7, ascFifoControl(a4)
```

The rising edge of bit 7 resets both FIFO read and write pointers to zero.
Both channels (A and B) are cleared simultaneously. An emulator should model
the clear on the rising edge: when bit 7 transitions from 0 to 1, all FIFO
state is reset.

---

## 8. Initialization Sequences

### 8.1 Standard ASC Cold Boot (ASCInitSoundHW)

The SE/30 ROM runs this routine at every cold boot, before any Sound Manager
involvement. It is dispatched via the sound vector table entry
`sndInitSoundHW` (index `$0B`):

```asm
ASCInitSoundHW:
    MOVEA.L ASCAddr(A0), A3         ; A3 = ASC base
    ASL.B   #5, D0                  ; D0 = volume (0-7) → shift to bits 5-7
    MOVE.B  D0, ascVolControl(A3)   ; write volume
    RTS
```

That is the **entire** initialization. All other registers are left at their
power-on reset defaults:

| Register | Default value | Meaning |
|---|---|---|
| `ascMode` | `0` | Chip inactive |
| `ascChipControl` | `0` | Mono, PWM |
| `ascFifoControl` | `0` | No clear in progress |
| `ascFifoInt` | `0` | No pending flags |
| `ascClockRate` | `0` | 22.257 kHz |
| `ascPlayRecA` | `0` | Play mode |
| `ascPlayRecB` | `0` | Play mode |
| `ascWaveOneShot` | `0` | No voices triggered |

### 8.2 Boot Volume Source

The ROM routine `InitSndNoRam` reads the PRAM byte at offset `$08`, lower 3
bits, to determine the startup volume. If PRAM is invalid (validation byte
at `$10` ≠ `$A8`) or the volume is zero, the ROM forces volume to **5** so
the startup chime is always audible:

```asm
cmpi.b  #$a8, d1                ; validate PRAM
beq.s   @pramValid
moveq.l #5, d1                  ; default: force level 5
@pramValid:
    and.b   #%00000111, d1      ; mask to 3 bits
    bne.s   @nonZeroVol
    move.b  #5, d1              ; minimum for boot beep even if PRAM says 0
@nonZeroVol:
```

### 8.3 Sound Manager Activation Sequence (Non-Batman ASC)

When the Sound Manager first enables audio playback it performs a full chip
reset:

```asm
; disable VIA2 sound interrupt first (move.b #$10, VIA2-IER)

move.b  #$1,  ascMode(a4)           ; FIFO mode
move.b  #$0,  ascPlayRecA(a4)       ; play mode, RFD off
move.b  #$80, ascFifoControl(a4)    ; pulse clear strobe ON
move.b  #$0,  ascFifoControl(a4)    ; pulse clear strobe OFF
tst.b         ascFifoInt(a4)        ; read-clear any stale interrupt flags
move.b  #2,   ascChipControl(a4)    ; stereo output
move.b  #0,   ascWaveOneShot(a4)    ; clear wavetable one-shot flags
move.b  #0,   ascVolControl(a4)     ; volume = 0 (set properly later)
move.b  #0,   ascClockRate(a4)      ; 22.257 kHz
```

After this sequence, the Sound Manager fills the FIFO and re-enables the
VIA2 interrupt.

### 8.4 POST Self-Test Register Writes

The POST self-test in the ROM (`USTNonCritTsts.a`) performs these writes on
any non-Batman ASC, confirming the full write-accessible register set:

```asm
; Reset (SndInitASC subroutine):
move.b  #1,   ascMode(a1)           ; FIFO mode
move.b  #0,   ascChipControl(a1)    ; clear overrun; PWM, mono
move.b  #$80, ascFifoControl(a1)    ; clear FIFO ON
move.b  #0,   ascFifoControl(a1)    ; clear FIFO OFF
tst.b         ascFifoInt(a1)        ; read-clear interrupt flags
move.b  #0,   ascWaveOneShot(a1)    ; clear wavetable flags
move.b  #0,   ascVolControl(a1)     ; volume = 0
move.b  #0,   ascClockRate(a1)      ; 22.257 kHz

; FIFO play test:
move.b  #2,   ascChipControl(a4)    ; stereo
move.b  #0,   ascPlayRecA(a4)       ; play
move.b  #$80, ascFifoControl(a4)    ; clear FIFO ON
move.b  #0,   ascFifoControl(a4)    ; clear FIFO OFF

; Wavetable test (skipped for Batman):
move.b  #$2,  ascMode(a1)           ; wavetable mode
move.b  #$0,  ascPlayRecA(a1)       ; play mode

; Record test:
move.b  #$81, ascPlayRecA(a1)       ; record + RFD enabled
move.b  #$80, ascFifoControl(a1)    ; reset FIFO ON
move.b  #0,   ascFifoControl(a1)    ; reset FIFO OFF
```

---

## 9. Sound Manager Software Architecture

### 9.1 Abstraction Philosophy

Direct manipulation of ASC registers by application software was strongly
discouraged by Apple. Writing to `0x50F14000` directly would break
compatibility with future Macintosh models using different audio silicon.
All audio capabilities are abstracted behind the Sound Manager API.

Software determines ASC presence at runtime via the Gestalt API, querying
the `gestaltHasAppleSoundChip` selector.

### 9.2 Sound Manager 3.0 Component Chain

Sound Manager 3.0 introduced a component-based processing pipeline:

1. **Decompressor** — expands compressed formats (MACE 3:1, MACE 6:1)
2. **Rate converter** — resamples to the ASC's native 22.257 kHz rate
3. **Apple Mixer** — digitally mixes multiple sound channels into a single
   stream in software
4. **ASC driver component** (subtype `'asc '`) — writes the final stream to
   hardware

Because the physical ASC requires 8-bit offset-binary PCM at exactly 22 kHz,
the driver component instructs the mixer to output in `kOffsetBinary` format
at the matching rate. The application developer remains entirely unaware of
FIFO buffer sizes, register offsets, or chip-level details.

### 9.3 Low-Memory Globals

| Address | Name | Type | Description |
|---|---|---|---|
| `$0CC0` | `ASCBase` | long | Physical base address of the ASC |
| `$0CC4` | `SMGlobals` | long | Pointer to Sound Manager global block |

`ASCBase` is set once at boot and never changes. The `DecoderInfo` record
(from `ProductInfo`) also contains `ASCAddr` at a fixed word offset,
aliasing the same value.

### 9.4 Sound Vector Table

The sound primitive dispatch table (pointed to by
`ProductInfo.SndControlPtr`) has these entries:

| Index | Equate | Function |
|---|---|---|
| `$00` | `sndDFACInit` | Init DFAC (not applicable to SE/30) |
| `$01` | `sndDFACSend` | Send byte to DFAC |
| `$02` | `sndSetVol` | Set playback volume |
| `$03` | `sndEnableSoundInts` | Enable sound interrupts at VIA |
| `$04` | `sndDisableSoundInts` | Disable sound interrupts at VIA |
| `$05` | `sndClearSoundInt` | Clear sound interrupt flag at VIA |
| `$06` | `sndInputSelect` | Select sound input source |
| `$07` | `sndQueryInput` | Query current input source |
| `$08` | `sndAuxByPass` | Enable/disable auxiliary bypass |
| `$09` | `sndPlayThruVol` | Set playthrough volume |
| `$0A` | `sndAGCcontrol` | Enable/disable AGC |
| `$0B` | `sndInitSoundHW` | Initialize sound hardware (boot) |

`sndInitSoundHW` resolves to `ASCInitSoundHW` on the SE/30 (§8.1).

---

## 10. Mode Transition Behavior

### 10.1 ascMode Writes

- Writing `ascMode = 0` stops the chip immediately. No further output or
  interrupts are generated.
- Writing `ascMode = 1` activates FIFO mode. If the FIFO is empty, no
  immediate interrupt fires; the first interrupt occurs when the playback
  pointer crosses the half-empty threshold after data has been written.
- Writing `ascMode = 2` activates wavetable mode. The SRAM is re-
  interpreted as four 512-byte voice tables.

### 10.2 Switching Between Modes

When switching modes, software should:
1. Write `ascMode = 0` to stop the chip
2. Clear the FIFO (toggle `ascFifoControl` bit 7)
3. Read `ascFifoInt` to clear any stale interrupt flags
4. Load new data into the SRAM as appropriate for the target mode
5. Configure control registers
6. Write the new mode value to `ascMode`

The ROM startup chime sequence follows this pattern: it loads wavetable data,
sets `ascMode = 2`, plays the chime, then later the Sound Manager resets
everything to FIFO mode.

---

## 11. Detailed Wavetable Mode Mechanics

### 11.1 Phase Accumulator Internals

Each of the four voices has two 32-bit registers:
- **Phase register** — current position in the wavetable
- **Increment register** — added to phase on every clock cycle

Only the lower 24 bits of each register are valid. They are stored
big-endian. The 24-bit value is interpreted as a **9.15 fixed-point
number**:

```
Bits 23–15: integer part (9 bits) → wavetable index 0–511
Bits 14–0:  fractional part (15 bits) → sub-sample precision
```

Sample lookup: `wavetable_base + ((phase >> 15) & 0x1FF)`

The 15-bit fractional part provides extremely fine frequency resolution —
approximately 0.07 Hz steps at the 22.257 kHz sample rate — enabling
precise musical interval tuning.

### 11.2 Stereo Sum in Wavetable Mode

The *Guide to the Macintosh Family Hardware* describes the stereo wavetable
configuration: "At each sample clock period, a sound value is taken from
each of the four buffers, the values are summed in pairs, and the result of
each summed pair is converted to a PWM signal."

- Left channel: Voice 0 + Voice 1
- Right channel: Voice 2 + Voice 3

In mono wavetable mode, all four voices are summed together and sent to
both channels.

### 11.3 Startup Chime Usage

The Macintosh II family startup chime is generated entirely in wavetable
mode:

1. The ROM loads one cycle of a waveform into all four voice buffers
2. Sets frequency increments to produce a specific chord
3. Sets `ascMode = 2` and triggers all four voices
4. The chord plays with no further CPU involvement

The "Chimes of Death" (fatal POST error tones) also use wavetable mode
with different frequency patterns to encode error codes.

---

## 12. EASC "Batman" Initialization (Reference)

Although the EASC is not present on the SE/30, IIx, or IIcx, the
`BatmanInitSoundHW` routine exists in the shared `$0178` ROM and may be
reached on Quadra machines. Documented here for completeness and to
illustrate the EASC's extended register set:

```asm
BatmanInitSoundHW:
    MOVEA.L ASCAddr(A0), A3
    ASL.B   #5, D0
    MOVE.B  D0, ascVolControl(A3)           ; volume

    MOVE.W  #$7F00, bmLeftScaleA(A3)        ; ch A → left (full)
    MOVE.W  #$007F, bmLeftScaleB(A3)        ; ch B → right (full)

FinishBatmanInit:
    MOVEQ   #1, D0
    MOVE.B  D0, bmIntControlA(A3)           ; mask sound interrupts ch A
    MOVE.B  D0, bmIntControlB(A3)           ; mask sound interrupts ch B

    ORI.B   #$80, ascFifoControl(A3)        ; clear FIFO ON
    ANDI.B  #$7F, ascFifoControl(A3)        ; clear FIFO OFF

    MOVE.B  #2,   ascPlayRecA(A3)           ; play mode at 22 kHz
    MOVE.B  D0,   ascMode(A3)               ; FIFO mode (D0=1)

    MOVEQ   #0, D0
    MOVE.B  D0, bmFifoControlA(A3)
    MOVE.B  D0, bmFifoControlB(A3)

    MOVE.W  #$812F, bmSrcTimeIncrA(A3)      ; SRC: 22.2545 kHz ch A
    MOVE.W  #$812F, bmSrcTimeIncrB(A3)      ; SRC: 22.2545 kHz ch B

    ; CD-XA filter coefficients for channel A
    MOVE.W  D0,     $F10(A3)                ; coeff 0 = $0000
    MOVE.W  #$003C, $F12(A3)                ; coeff 1 = $003C
    MOVE.W  #$CC73, $F14(A3)                ; coeff 2 = $CC73
    MOVE.W  #$C962, $F16(A3)                ; coeff 3 = $C962
    ; CD-XA filter coefficients for channel B
    MOVE.W  D0,     $F30(A3)
    MOVE.W  #$003C, $F32(A3)
    MOVE.W  #$CC73, $F34(A3)
    MOVE.W  #$C962, $F36(A3)

    MOVE.B  #$80, bmFifoControlA(A3)        ; enable SRC ch A
    MOVE.B  #$80, bmFifoControlB(A3)        ; enable SRC ch B
    RTS
```

Batman-specific registers (offsets from ASC base):

| Equate | Description |
|---|---|
| `bmIntControlA/B` | Interrupt control: `1` = mask, `0` = unmask |
| `bmFifoControlA/B` | FIFO/SRC control: bit 7 = SRC enable |
| `bmSrcTimeIncrA/B` | Word: SRC clock increment. `$812F` = 22.2545 kHz; `$3FDB` = 11 kHz |
| `bmLeftScaleA` | Word: left volume (high byte) / right volume (low byte) for ch A |
| `bmLeftScaleB` | Word: left volume (high byte) / right volume (low byte) for ch B |

CD-XA coefficients are hardcoded per-channel at offsets `$F10`–`$F16` (ch A)
and `$F30`–`$F36` (ch B). These are IIR filter weights for CD-XA (ADPCM)
decompression. The SE/30 does not use these registers.

---

## 13. Key Behavioral Facts for Emulator Implementation

This section summarizes the specific behaviors that must be correctly
modeled for faithful emulation of the ASC on the SE/30, IIx, and IIcx.

### 13.1 Power-On Register State

After power-on or reset:

| Register | Value | Notes |
|---|---|---|
| `ascVersion` | `$00` | Read-only; identifies standard ASC |
| `ascMode` | `0` | Chip inactive until software writes `1` or `2` |
| `ascChipControl` | `0` | Mono, PWM |
| `ascFifoControl` | `0` | No clear in progress |
| `ascFifoInt` | `0` | No pending flags |
| `ascVolControl` | boot value | Set by `ASCInitSoundHW` before Sound Manager starts |
| `ascClockRate` | `0` | 22.257 kHz |
| `ascPlayRecA` | `0` | Play mode |
| `ascPlayRecB` | `0` | Play mode |
| `ascWaveOneShot` | `0` | No voices triggered |

### 13.2 ascFifoInt Auto-Clear

Every CPU read of offset `0x804` must clear the register to `$00`. Setting
the register from `$00` to any non-zero value (internally, when a threshold
is crossed) should re-assert VIA2 CB1 if VIA2 IER has CB1 enabled.

### 13.3 FIFO Pointer Reset

Toggling bit 7 of `ascFifoControl` (write `$80` then `$00`) resets both
the read pointer and write pointer to zero. Both channels (A and B) are
cleared simultaneously.

### 13.4 Stereo on SE/30

The SE/30 always runs in stereo (`ascChipControl = 2`). Channel A drives
the left Sony chip and channel B drives the right. The two analog outputs
are summed by the board to mono for the internal speaker while remaining
separate on the headphone jack. An emulator need only maintain two
independent PCM streams and mix them to mono for audio output.

### 13.5 FIFO Address Independence

In FIFO mode, the specific address written within a channel's range does not
matter. All writes to `0x000`–`0x3FF` enqueue bytes to FIFO A; all writes
to `0x400`–`0x7FF` enqueue bytes to FIFO B. The ASC uses internal pointers
with `& 0x3FF` wrapping.

### 13.6 ascMode Transitions

- `ascMode = 0` stops the chip. No output, no interrupts.
- `ascMode = 1` (FIFO): output begins when data is present; first interrupt
  fires when the playback pointer crosses the half-empty threshold.
- `ascMode = 2` (wavetable): output begins when voices are triggered via
  `ascWaveOneShot`.

Writing `1` while the FIFO is empty should **not** generate an immediate
interrupt.

### 13.7 VERSION Register

Return `$00` from offset `0x800`. This causes all runtime checks to take
the standard-ASC code paths rather than the Batman or V8/Eagle paths.

### 13.8 Wavetable Priority

The startup chime and Chimes of Death use `ascMode = 2`. For a minimum
viable emulator, wavetable mode can be implemented after FIFO mode is
working, since the Sound Manager never uses wavetable mode for normal
application audio.

---

## 14. SE/30 vs IIx vs IIcx: Platform Differences

While the three machines share identical ASC hardware and ROM, there are
minor platform-level differences affecting sound:

| Aspect | SE/30 | IIx | IIcx |
|---|---|---|---|
| Form factor | All-in-one compact | Large modular desktop | Compact modular desktop |
| Expansion | 1 PDS slot | 6 NuBus slots | 3 NuBus slots |
| Stereo mode | **Always stereo**; board sums to mono for speaker | Switchable; speaker = left only | Switchable; speaker = left only |
| Headphone detection | Not used (always stereo) | VIA2 bit indicates plug insertion | VIA2 bit indicates plug insertion |
| Model ID bits | PA6=1, PB3=0 | PA6=0, PB3=0 | PA6=1, PB3=1 |
| Gestalt ID | 9 | — | — |

The ASC base address, register map, interrupt routing (VIA2 CB1), sample
rates, SRAM size, and all behavioral characteristics are identical across
all three machines.

---

## 15. Reference: Macintosh Hardware Sound Implementations

| Model | Audio silicon | Part number | On-chip SRAM | Wavetable | Stereo | Notes |
|---|---|---|---|---|---|---|
| Mac II | Discrete ASC | 344S0053 | 2 KB | Yes | Yes | Original introduction; 68020 |
| Mac IIx | Discrete ASC | 344S0053/0063 | 2 KB | Yes | Yes | 68030; shared ROM with SE/30 and IIcx |
| Mac IIcx | Discrete ASC | 344S0053/0063 | 2 KB | Yes | Yes | Compact modular; 3 NuBus slots |
| Mac SE/30 | Discrete ASC | 344S0053/0063 | 2 KB | Yes | Always | Internal speaker = mixed stereo-to-mono |
| Mac IIci | Discrete ASC | 344S0063 | 2 KB | Yes | Yes | Same ASC, different system ASIC (RBV) |
| Mac IIfx | Discrete ASC | 344S0063 | 2 KB | Yes | Yes | Different base address (`0x50F10000`) |
| Mac Portable | Discrete ASC | 344S0053/0063 | 2 KB | Yes | Yes | 68HC000; power-managed |
| Quadra 700/900/950 | Discrete EASC | 343S1036 | 2 KB | **No** | Yes | 16-bit output; sound input; CD-XA |
| Mac LC / LC II | V8 integrated | 343S0116-A | No (DRAM) | **No** | Mono | DMA FIFO from main RAM |
| Mac Classic II | Eagle integrated | 343S1054-A | No (DRAM) | **No** | Mono | DMA FIFO from main RAM |
| Color Classic | Spice integrated | 343S0132 | No (DRAM) | **No** | Mono | DMA FIFO from main RAM |
| Mac LC III | Sonora integrated | 343S1065 | No (DRAM) | **No** | Mono | Enhanced DMA; level-sensitive IRQ |
