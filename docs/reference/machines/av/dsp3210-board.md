# The AV DSP3210 board integration

**Contents:**

1. [Overview](#1-overview) — what the part is on these boards, which machines carry it, clocking, the
   board around the DSP, reset straps
2. [Register file](#2-register-file) — the host-side window: `dspOverRun`, the PSC interrupt registers,
   the sound block; the kernel data area the host fills; the DSP-side addresses the board hands over
3. [Behaviour](#3-behaviour) — bus mastering and the shared address space, reset, interrupts in both
   directions, frame overrun, the timer heartbeat, the audio output path, the capture path, the
   telecom path
4. [Programming model](#4-programming-model) — the software stack, the three-stage boot bring-up,
   steady state, the driver's selector surface, rates and PRAM, the BIO port as a managed device
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is, and what this page covers

The AV Quadras — [Quadra 840AV](q840av.md) and [Centris 660AV](q660av.md), Apple's Cyclone/Tempest
boards — carry an **AT&T DSP3210 32-bit floating-point digital signal processor** as a second,
independent processor: "The DSP gives these computers the ability to perform fast, complex real-time
data processing tasks, such as speech recognition, audio compression, analog modem signal
processing, and so on" [1] p. 13. Unlike a peripheral on an I/O bus, the DSP sits on the CPU bus as a
fourth bus master — alongside the main processor, the PSC and MUNI — and fetches its programs and
data directly from main memory [1] pp. 13, 18.

The chip itself — instruction set, register model, exception architecture, boot ROM — has its own
page: see [DSP3210](../../hardware/dsp3210.md). This page is the **board integration**: how the AV
boards clock, reset, arbitrate and interrupt the part, which host-visible registers drive it, how
the ROM's Real Time Manager boots and services it, and how the audio output, sound-input capture
and telecom datapaths are wired around it. Wherever a fact below is about the silicon, the chip page
is cited instead of restated.

The board half is unusually software-shaped. Almost none of the DSP's integration lives in
dedicated hardware: the DSP→host interrupt is a GPIO pin toggled by a DSP store instruction, the
host→DSP interrupt is a pulse per sound frame borrowed from the codec's frame clock, and the
host↔DSP conversation is otherwise entirely shared memory. The wiring that exists — reset, clock,
bus arbitration, and three interrupt paths — is pinned down in §2–§3; the software contract that
makes it a coprocessor is §4.

### 1.2 Machines that carry it

| Machine | Apple board codename | CKI | Notes |
|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone | **66.6667 MHz** | DSP on the main logic board [1] pp. 13, 17–18 |
| Macintosh Centris 660AV | Tempest | **55.5000 MHz** | DSP on the main logic board; same platform, fewer slots [1] pp. 13, 17–18 |

Both machines' ROMs are a single shared image: the DSP driver, kernel and standard modules are
byte-identical between the two, and only the clock rate differs (*observed*; the disassembly below
is of that one image). The AV configurations of the first-generation Power Macintosh are reported
to carry the same DSP on their AV card; that wiring is outside this page's evidence and remains
*inferred — unverified* (the [chip page](../../hardware/dsp3210.md) §1.2 carries the same caveat).

### 1.3 Clocking

The DSP is clocked by **CKI**, "66.6667/55.5000 MHz, Oscillator, usage: DSP, MCA" [1] Table 2-2
pp. 17–18. Three consequences follow from that one table row:

- **The rate is documented, not inferred.** The 840AV runs the 66 MHz speed grade (15 ns CKI) and the
  660AV the 55 MHz grade (18 ns CKI); AT&T's March 1993 data sheet publishes both grades side by
  side [3] "Timing Specifications". The part's architecture is specified "up to 33 million
  floating-point operations per second (with a clock rate of 66.7 MHz)" [2] §1.1, and the
  instruction cycle is CKI/4 [2] §7.1 — so the boards deliver 16.67 MIPS (840AV) / 13.9 MIPS
  (660AV). The 66 MHz-class rate also shows up in the kernel's own arithmetic: a 10 ms frame
  measures 166,667 timer ticks at CKI/4 (*observed*, §3.6).
- **The DSP is asynchronous to the main processor.** The Dev Note says so explicitly — of the four
  CPU-bus masters, the DSP "(which is asynchronous with respect to the main processor)" is the only
  one called out [1] p. 18. Its bus transactions cross into the 40/25 MHz CPU-bus domain through
  the MCA.
- **CKI also feeds the MCA**, the memory controller and arbiter [1] Table 2-2 p. 18. The arbiter that
  grants the DSP the CPU bus runs on the DSP's own clock — the natural way to arbitrate a master
  whose cycle timing is CKI-based (the MCA's DSP grant timeout is even counted in DSP clocks,
  §3.1).

A separate clock domain touches the DSP only at the edges: the **Apple Telecom External Clock
Synchronizer (ATECS)** "can synchronize the DSP and sound subsystems to an external clock signal
received through the Apple GeoPort serial port connector. In the absence of an external clock, it
generates crystal-controlled 49.152 MHz timing signals for 48 KHz operation or 45.1584 MHz timing
signals for 44.1 KHz operation" [1] p. 16. Those ATECS outputs are PSC inputs (the codec clock
family, §3.7); how ATECS "synchronizes the DSP" itself, given that CKI is a fixed oscillator, is not
documented (§6.11).

The DSP's fully static design means the kernel can and does sleep the part between frames —
`waiti` parks it with peripherals and interrupt sensing alive, waiting for the next frame tick
([2] §3.2.3; §3.6 below).

### 1.4 The board around the DSP

Everything the DSP touches on the AV board belongs to one of five other parts:

| Part | What it does for the DSP |
|---|---|
| **MCA** (Memory Controller and Arbiter) | the DSP's bridge to RAM and ROM: bus interface, CPU-bus arbitration among the four masters, and bus timeouts [1] pp. 13, 18–19 (§3.1) |
| **PSC** ([PSC](psc.md)) | the DSP's entire host-visible register file: the reset/overrun register, the level-4/level-5 interrupt registers, and the sound DMA engine that moves samples between RAM and the codec (§2, §3.7) |
| **Singer** ([Singer](singer.md)) | the 16-bit stereo codec, on a serial frame bus driven by the PSC — the DSP never touches it directly (§3.7–§3.8) |
| **ATECS** | external-clock synchronization for the DSP/sound subsystems via GeoPort [1] p. 16 (§3.9) |
| **DAV connector** | time-division-multiplexed audio on the same serial frame bus as the Singer; the DSP is not on it [1] p. 44 (§3.10) |

### 1.5 Reset straps and the processor-mode contract

The DSP3210 latches four configuration bits from the **BIO7–BIO4** pins into `pcw[10:13]` on the
rising edge of `RESTN` [2] §6.2, §7.5.4.1, which selects one of the chip's three start-up paths
([DSP3210](../../hardware/dsp3210.md) §3.9). The AV boards strap the part into **processor mode**
(`pcw[10] = 0`, BIO7 strapped low): on reset release the chip fetches its first instruction from
*external* address 0 — host RAM — and its 256-word mask boot ROM, relocated at $50030000 in this
mode, never runs. This is established several independent ways, all from the shipped code [4]
(*observed*): the host writes *instructions* to physical 0 and never pulses any doorbell (§4.2);
the kernel data area is seeded with the processor-mode on-chip addresses $5003E000/$50040000,
matching the processor-mode memory map [3] Figure 8; the kernel's `slave` segment forms
processor-mode on-chip addresses directly; and the entry block's `pcw = $3B80` write sets C/PN = 0
before the kernel touches the bus (§4.2).

The remaining straps are not directly evidenced: `pcw[11]` (R/WN) and `pcw[12]` (D/SN) select the
bus signalling for the very first fetches, and nothing observes them before the kernel's own `pcw`
write. Since $3B80 programs R/WN = 1 (RW high on reads, Motorola style) and D/SN = 1 (DRAM/system-bus
signalling — pins 85/26 become the **AEN/DEN inputs**), the boards are assumed to strap BIO6 = BIO5
= 1 so the signalling never changes mid-flight (*inferred*; §6.12). The practical consequence of
D/SN = 1 is worth stating because it is unusual: **the board has no MRN/MWN write strobes at all**
— direction is carried by RW with CSN/ASN, exactly like a 68040, and an external controller gates
address/data with AEN/DEN [2] §6.6.2.

### 1.6 What the DSP is *not* wired to

Two negatives are load-bearing for anyone reasoning about the board:

- **The DSP's own serial port and DMA controller are unused.** The DSP3210's SIO and its two-channel
  DMAC are designed to stream a codec with zero software in the loop [2] §9.1–§9.2 — but on the AV
  boards the codec hangs off the *PSC*, which does its own sound DMA (§3.7). No shipped DSP code
  ever programs `ioc` or `dmac`; the kernel leaves those MMIO words untouched (*observed* [4]).
  The audio path is entirely shared-memory: the PSC moves samples between RAM and the codec, and
  the DSP reads and writes the same RAM.
- **The video capture path never touches the DSP.** The digitizer chain (the Philips multistandard
  decoder and scaler into VRAM through the CIVIC video-in port) is a separate subsystem with its own
  Component Manager driver [1] pp. 30–33; nothing in it programs or interrupts the DSP. The one
  observed interaction is indirect: video capture applications do audio playthrough, which travels
  the normal sound path and therefore the DSP (§3.8).

## 2. Register file

The DSP has no register window of its own on the host bus. Its board-level register file is the
**PSC's**: a byte at $50F3121C that owns the DSP's reset line, the level-4/level-5 interrupt
registers, and the sound-block registers that carry the frame tick. Behind those sit two
DSP-side locations the host *configures* rather than accesses: the kernel data area, and the MMIO
address of the DSP's own bit-I/O port.

### 2.1 `dspOverRun` — $50F3121C (byte, R/W)

The single register that owns the DSP's reset line. All PSC byte registers with bit fields use
VIA-style sense semantics: **bit 7 of a write is the set/clear select** — 1 sets the bits written 1,
0 clears them; a read returns the live bits [4] (*observed*; the driver's writes and the PSC
interrupt code agree).

| Bit | Name (recovered) | Meaning |
|---|---|---|
| 0 | `pdspReset` | 1 = hold the DSP in reset, with core state cleared; 0 = released, fetching |
| 1 | `pdspResetEn` | interlock: when 0, **defeats the reset bit** — a hardware-initiated reset can no longer assert |
| 2 | `pdspFrameOvr` | sticky **frame-overrun latch**, set by PSC hardware when a frame boundary passes with the previous frame unserviced (§3.5) |
| 7 | sense | 1 = set / 0 = clear, for bits 0–2 |

Every value the shipped ROM ever writes, and what each does [4] (*observed*):

| Write | Effect | Used by |
|---|---|---|
| `$83` | SET ResetEn + Reset → **assert DSP reset** with state cleared | the `stop` selector, the overrun handler, boot failure |
| `$01` | CLEAR Reset → **release the DSP; it begins fetching at physical 0** | `StartProcessorRoutine` |
| `$81` | SET Reset → halt the DSP again | boot timeout paths |
| `$04` | CLEAR the frame-overrun latch | `stop`, the overrun handler, `debx` |
| `$02` | CLEAR ResetEn → **disarm the hardware reset-on-overrun** | `debn` (§4.6) |
| `$82` | SET ResetEn → re-arm it | `debx` (§4.6) |

Two behavioural contracts go beyond the write table. First, **at power-on the DSP is held in reset
by hardware with bit 0 already set**: the driver's first release is a bare `$01` clear-write with no
preceding `$83`, and the latch reads back set before it (*observed* in the boot trace). Second,
`pdspResetEn` is *not* a power control: the writes that use it are the debugger-interlock pair
(§4.6), and its documented meaning matches the Dev Note's category-three overrun behaviour — "the
frame overrun logic will issue a hardware reset to the DSP and I/O subsystems" [1] p. 84 — i.e. it
**arms the hardware-initiated reset on frame overrun** (§3.5).

### 2.2 The interrupt registers — $50F31140/$144 (L4) and $50F31150/$154 (L5)

The PSC presents per-level interrupt registers, byte-wide, one pair (IR/IER) per 68040 interrupt
level [4]; the two that concern the DSP path are:

| Address | Register | Bits |
|---|---|---|
| $50F31140 | L4IR (read) | bit 0 = **SNDSTAT** (Singer codec status — the sense-pin change interrupt); bit 7 = OR of all pending on the level |
| $50F31144 | L4IER (read/write) | mirror of the IR bits; **bit 7 = sense** (1 = set the bits written 1, 0 = clear them) |
| $50F31150 | L5IR (read) | bit 0 = **the DSP interrupt** (DSP→host message), bit 1 = **FRMOVRN** (frame overrun); bit 7 = OR |
| $50F31154 | L5IER (read/write) | mirror; bit 7 = sense |

Clearing conventions are asymmetric and matter: **L5IR bit 0 is a write-to-clear latch** — the
handler writes `$01` to $50F31150 to acknowledge (§3.3) — while **L4IR is write-back-to-clear**: the
SNDSTAT handler reads $140, masks to bit 0 and writes the masked value back [4] (*observed*). The
IERs are enabled with sense-bit-7 writes (`$81`/`$82`/`$83` → $154 enable L5 bits 0/1/both;
`$01`/`$02`/`$03` disable) [4].

The PSC's IR registers have a documented metastability quirk that the dispatchers encode: **every
interrupt dispatcher reads the IR twice and loops until two reads agree** [4] (*observed* in the
ROM's level dispatchers). A board whose IR reads are not repeat-stable livelocks the ROM.

### 2.3 The sound block — PSC base + $200

The sound engine registers live in the PSC at $50F31200–$50F31218; the DSP integration touches
these fields (the full codec-facing detail belongs to [Singer](singer.md)):

| Address | Width | Name | Role in the DSP path |
|---|---|---|---|
| $50F31200 | word | `sndComCtl` | bit 6 `pFrmIntEn` — **the frame-interrupt gate that clocks the DSP** (§3.4); bits 7/8 the input/output DMA enables; bits 9–10 the codec rate code |
| $50F31204 | long | `singerCtl` | codec control; the input-gain and input-mux fields are the microphone path's only gain programming (§3.8) |
| $50F31208 | long | `singerStat` (RO) | codec status; its four sense bits are the capture path's presence detection (§3.8) |
| $50F3120C | long | `sndPhase` (RO) | the free-running play/capture position — the sound team's phase reference, handed to the DSP as an aliased address (§2.5) |
| $50F31210 | long | `sndInBase` | the **input double-buffer base** — the physical address the capture DMA fills |
| $50F31214 | long | `sndOutBase` | the **output double-buffer base** — the physical address the playback DMA drains |
| $50F31218 | word | `sndSize` | half-buffer size **in sample frames**; the production DSP path always uses 240 (§3.7) |
| $50F3121C | byte | `dspOverRun` | §2.1 |

`sndPhase`'s low bits are a 1/64-frame fraction and its bits 6–17 the integer frame offset within the
2 × `sndSize` double-buffer pass; the driver's phase-rollover status call returns `sndSize << 6`,
which pins the unit [4] (*observed*).

### 2.4 The kernel data area — the host→DSP register file

The DSP's kernel is not commanded through any hardware register. When the host loads the kernel it
fills a **data area at evt+0x200** (0x400 bytes, inside the evt segment allocation) with
addresses, flags and seeds — this is the register file of the board interface, and its layout is
recovered from the driver's initialization code [4] (*observed*). The fields the host writes at
boot, and the values the ROM writes into them:

| Offset | Value written | Meaning |
|---|---|---|
| +$40 | $FFFFFFFC | current message-context id (−4 = timeshare); the kernel flips it to −8 (real time) inside each frame |
| +$100 / +$104 | buffer physicals | TS_Messages / RT_Messages ring bases (two $800 named buffers) |
| +$110 / +$114 | buffer physicals | Singer **input / output** ring bases |
| +$17C | **$5003041E** | the DSP's own `bio` register — MMIO, processor-mode, big-endian address ([DSP3210](../../hardware/dsp3210.md) §2.6). The kernel dereferences this to toggle the doorbell (§3.3) |
| +$180 | $0003 | the value written to `bio` per message: field 0 = `11`, **complement BIO0** |
| +$18C | $FFFFFFFF | timer reload, until the kernel measures the real frame period into it |
| +$198 | 0 | handshake flags: **bit 0 = kernel alive (stage 2), bit 1 = first frame serviced (stage 3)** |
| +$19C / +$1A0 / +$1A4 | page physicals | three $200 pages carved at evt+$600/$800/$A00 (kernel stack among them) |
| +$1A8 | **$50040000** | `hmem` heap cursor — one past the top of the on-chip 64 KB window; cached code is allocated top-down from here |
| +$1B4 / +$1B8 / +$1BC | 0 / 0 / $01 | BIO read-shadow / BIO write-request / BIO **direction** byte — BIO0 already an output at boot |
| +$1C0 | block physical | the real-time task-list block (`rttl`) |
| +$1C4 | 0 | the frame counter (read by modules as the frame number) |
| +$1C8 | 1 | frame-service mode flags |
| +$1D0 | (kernel-written) | the measured frame period in timer ticks — the device's total GPB |
| +$1D4 | block physical | the timeshare task-list block (`tstl`) |
| +$1EC | buffer physical | RT_Messages base again |
| +$1F0 / +$1F4 | **$5003E000** | RAM1 base — the `lmem` heap bottom and the sleep gadget's home |
| +$1F8 | **$50040000** | `hmem` cursor again |
| +$1FC | host pointer | reference to the host-side driver globals; the kernel sets bit 8 of its first word on the first frame |

Two fields deserve comment. **+$168** (the sample-counter address used by the slave-phase-lock
mechanism, §3.6) is deliberately **zero on a single-DSP machine**: the driver's make-master path
writes 0 there, and only a slave configuration points it at the master's phase counter [4]
(*observed*). And the whole area lives in the **evt** segment's allocation — the older reading that
it belongs to the `core` segment is wrong; the loader treats evt+0x200, not core+0x200, as the
data area [4].

### 2.5 DSP-side addresses the board hands over

Three addresses connect the DSP's internal map to the host's I/O space:

- **`bio` at $5003041E** — the kernel's doorbell register, whose address the host plants at
  kernel+0x17C (§2.4). On-chip and invisible to the host; the host's only role is to name it.
- **The $F0 I/O alias.** The driver's `phas` selector returns `sndPhase` to DSP-side clients as
  **$F073120C** — the PSC register at host $50F3120C reached through a DSP-side window at
  $F0xxxxxx [4] (*observed*). The arithmetic — DSP $F0000000–$F07FFFFF mapping onto host
  $50800000–$50FFFFFF, i.e. `host = $50800000 | (dspaddr & $7FFFFF)` — is *inferred* from this one
  observed pointer; no document states the decode. The sound team's output task reads its position
  through this alias every frame (*observed*).
- **$50040000 and above** — per the chip's processor-mode map this is external memory A resuming
  above the on-chip window [3] Figure 8; the kernel treats it as the `hmem` cursor's starting value
  (§2.4). What the host decodes at $50040000+ is not established (§6.4).

## 3. Behaviour

### 3.1 Bus mastering and the shared address space

The DSP is a default-slave bus master: it executes from internal memory until it needs the bus,
asserts BRN, waits for BGN, acknowledges with BGACKN and drives the bus ([DSP3210](../../hardware/dsp3210.md)
§3.4). On the AV board the grant comes from the **MCA**, which "performs arbitration for control of
the CPU bus between the main processor, PSC, MUNI, and DSP" [1] pp. 13, 18, and which also furnishes
the DSP's timeout: "if the DSP does not issue a cycle start signal within **16 DSP clock cycles**
after it is granted bus control, the MCA terminates the DSP's control and issues a bus error
signal" [1] p. 19. (The general CPU-bus timeout — 16 µs outside NuBus space, 32 µs within —
applies to the other masters; the DSP's is counted in its own clocks, another fingerprint of the
asynchronous clock domain [1] p. 19.)

The address space the DSP masters is **the host's physical address space, untranslated**. The chip
has no MMU [1] p. 77; the 68k's MMU maps logical→physical for the main processor only, so the two
processors agree on physical addresses and differ on reachability: everything the DSP is handed
lives in "locked contiguous and non-cacheable" blocks precisely so that a physical address is a
stable contract [1] p. 77. In particular:

- **DSP physical 0 is host physical 0.** The boot stub is planted at absolute 0 (§4.2); there is no
  aperture or window register anywhere in the protocol [4] (*observed*).
- The on-chip regions ($50030000–$5003FFFF) are internal to the DSP and invisible to the host.
- **ROM and NuBus space ($40000000 and above) is not a DSP target.** The kernel's own error handler
  reports a bus error there as `'xbus'`, and the host-side message vocabulary confirms ROM addresses
  are never legitimately dereferenced by the DSP [4] (*observed*; a runaway DSP access above
  $40000000 takes the bus-error vector).
- **Cache coherency is software-managed and one-directional.** The host pushes dirty cache lines
  before releasing the DSP (CPUSHA on the 68040, CACR bit 11 on the 68030) and again before every
  read of DSP-written memory; the DSP, having no cache of the 68040's kind, does nothing [4]
  (*observed*). The Dev Note's "non-cacheable" DSPAddress rule is the same contract stated for
  application writers [1] p. 77.

Under normal circumstances "the DSP should demand only a low percentage of the CPU bus bandwidth",
though DSP-heavy applications can slow the main processor — the price of a shared-memory DSP [1]
p. 76.

### 3.2 Reset and release

The DSP's `RESTN` line is open-drain and bidirectional on the chip [2] Table 5-2; on this board its
owner is the PSC's `dspOverRun` bit 0 (§2.1). The sequence the hardware and the driver together
implement:

1. **Power-on**: the DSP is held in reset with its state cleared (*observed*; the first release
   write assumes it).
2. The driver writes `$01` to $50F3121C — reset released. The chip begins fetching at **physical
   address 0** in processor mode, with `pcw[9:0]` at its reset default $38F (maximum wait states,
   big-endian, Motorola signalling) and `pcw[10:13]` as strapped [2] Table 7-4. Reset timing
   particulars — the 8 rising CKI edges of internal hold, outputs high-impedance through the reset
   sequence, and the 4-CKI minimum RESTN pulse — are the chip's own [2] §7.5.4.1.
3. The first two instructions the fetched code executes are the kernel entry pair
   `r1 = (ushort24) $3B80; pcw = (short) r1` (§4.2), which sets **zero wait states on both bus
   partitions**, big-endian, Motorola signalling, processor mode, DRAM-style AEN/DEN inputs, and
   **locks pcw** until the next reset or an error exception [2] Table 10-8.

Note what is absent: nothing re-latches the straps, and nothing re-runs the on-chip boot ROM — in
processor mode there is no ROM path to re-run ([DSP3210](../../hardware/dsp3210.md) §3.9).

### 3.3 Interrupts, DSP to host: the BIO0 doorbell

The DSP3210 **cannot raise an interrupt by itself.** Its only interrupt-related outputs, IACK0 and
IACK1, acknowledge *inbound* requests (and jointly flag a double error) [2] §7.5.2.1, and its BIO
port has no interrupt-generation capability of any kind [2] §9.4. The chip's only general-purpose
outputs visible to board logic are the eight BIO pins. On this board, that fact plus the kernel's
code pins the whole outbound path [4] (*observed*):

1. The kernel's message enqueue routine ends with a single 16-bit store to the `bio` register at
   $5003041E — **one toggle of BIO0 per successfully queued message**, never batched. Both the
   register address and the value are host-supplied kernel data (+$17C = $5003041E, +$180 = $0003,
   the complement-BIO0 field encoding; §2.4).

```
r17 = r22 + 0x37c        ; &data+$17C
r16 = *r17++             ; r16 = $5003041E (the bio register)
r17 = *r17               ; r17 = $0003 (complement BIO0)
goto r18
*r16 = (short) r17       ; the doorbell write
```

2. The **PSC latches its level-5 bit 0 on the output transition of BIO0** — either edge, since
   consecutive messages alternate the pin's level. The latch is cleared by a write, not by the pin
   returning anywhere.
3. The 68040 takes the level-5 interrupt. The ROM's level-5 dispatcher re-reads L5IR until stable,
   masks with L5IER, and calls the installed handler in a loop until the masked status is zero —
   **the dispatcher itself never acknowledges; the handler owns the source** [4] (*observed*).
4. The Real Time Manager's handler is deliberately **software-edge-triggered**: it first masks the
   delivery (`$01` → $154 — L5 bit 0 off), then acknowledges (`$01` → $150 — the latch clear), then
   defers all real work and only re-enables (`$81` → $154) once the deferred work has drained and
   the DSP is still marked running [4] (*observed*, §4.3). Anything the DSP raises between the
   ack and the re-enable is latched and delivered afterwards — the handler runs exactly once per
   assertion window.

The dispatch pipeline has one more stage that is pure AV-platform history: the handler installs a
**DSPL2 stub**, and the ROM's interrupt epilogue runs it at 68040 **level 2** before the deferred
task queue [4] (*observed*). The purpose is Sound Manager compatibility: sound code written for the
VIA2-era level-2 sound interrupt must see its interrupt at level 2, so the level-5 DSP interrupt is
replayed one level down before the system's deferred processing.

### 3.4 Interrupts, host to DSP: the frame tick, and nothing else

**There is no host→DSP doorbell register.** No PSC register write signals the DSP; the driver's
every "command" is a shared-memory write the kernel reads on its own schedule. The Dev Note states
the model outright: "Message passing from the host to the DSP is accomplished by using shared
memory... The DSP must check this section for new information in every frame" [1] p. 153.

The one periodic hardware event the host *does* deliver is the **sound frame tick**: at each frame
boundary of the sound engine, while `sndComCtl` bit 6 (`pFrmIntEn`) is set, the board pulses the
DSP's **IR1N (EXT1) pin active-low** — a short pulse, one per frame [4] (*observed*; its
electrical shape is §6.3). The same gated tick is also what the host itself receives as the frame interrupt, so
one enable bit clocks both processors (a quirk with teeth, §5). With the production frame size
(§3.7) the tick lands every 10 ms at 24 kHz and proportionally at the other rates.

Three details of the tick are pinned by the kernel's own code [4] (*observed*):

- **It must be a pulse, not a level.** The kernel's boot-time calibration gadget spins on the *live*
  pin level (`ir1s` — PS bit 13, 1 = negated) waiting for the *next* edge; a held level would hang
  the measurement forever. The pulse also has to be narrow enough that the gadget's two spins
  straddle two different ticks — the observed constraint that first shaped the board model.
- The pin is shared with the **debugger**: a DSP stopped by the Snoopy debugger with no breakpoint
  set "will always stop at ExternalIntOne of the DSP operating system routines" [1] p. 446 — the
  host's break request is delivered through the same external interrupt.
- **Nothing drives IR0N.** The kernel's vector table carries a handler for external interrupt 0,
  but no shipped host code ever asserts that pin, and the polling conditions that read it
  (`ir0s`/`ir0c`) appear only in code that never executes on this board [4].

The DSP side of the frame tick — vector 15 dispatch, the shared trampoline that turns *any* enabled
interrupt into frame service, and the timer hand-off — is the chip page's §3.3 plus the kernel
described in §3.6 below.

### 3.5 Frame overrun: three categories, one latch, one reset

The Dev Note defines frame overrun in three categories [1] pp. 83–84, and each maps to a different
piece of the board:

| Category | Definition | Who detects it | What happens |
|---|---|---|---|
| 1 | the DSP acknowledges the current frame interrupt after the next arrives, but before a second | the **DSP operating system**, in software — the kernel polls the live EXT1 pin level between modules and treats "pin still asserted at the next module boundary" as a skip [4] (*observed*) | recovery in the next frame: worst-GPB task set inactive, application notified; others told a frame was skipped [1] p. 84 |
| 2 | acknowledged after two interrupts, before a third | the **external interrupt logic** — the board: `pdspFrameOvr` latches and FRMOVRN interrupts the host at level 5 [4] (*observed*) | "the interrupt logic sends a hardware interrupt to the main processor and the Real Time Manager executes its DSP overrun recovery code... issues the DSP a reset command" [1] p. 84; see §4.6 for the exact ROM sequence |
| 3 | no response to interrupts by the sixth frame | the board's overrun logic | "the frame overrun logic will issue a **hardware reset to the DSP and I/O subsystems**... to prevent possible problems in the output subsystems — for example, a fixed sound on the speaker or the telecom system left offhook" [1] p. 84 |

This is what `pdspResetEn` (§2.1) arms: the category-three hardware reset. The category-two host
path is entirely the driver's — the FRMOVRN handler kills the DSP with `$83` → $21C and never
restarts it (§4.6); the category-three reset is the board's own last resort. The exact set
condition of `pdspFrameOvr` in PSC hardware — one unserviced tick, or the category-two count — is
not documented anywhere (§6.8).

The DSP side of category one is worth one more sentence because it explains a chip-level register
oddity: the kernel's overrun handler executes `r = emr; emr = r|1; emr = r` — a bit-0 write to a
register whose bit 0 masks no documented source — to **drop a latched-but-untaken EXT1 request**
before waiting for the next edge [4] (*observed*; undocumented in [2]).

### 3.6 The timer heartbeat and the frame phase-lock

Between frame ticks the kernel does not spin; it sleeps, and what wakes it is the chip's own
32-bit timer [4] (*observed*):

1. **Calibration.** At boot, under `emr = $8000` (EXT1 only), the kernel parks in a quick-interrupt
   gadget in on-chip RAM1, spins on the live pin between two successive ticks, samples the timer
   at each, and stores the difference — the **frame period in CKI/4 ticks** — at kernel+0x1D0,
   where the host reads it back as the device's total guaranteed-processing-bandwidth figure
   (§4.5). At 66.6667 MHz this measures ~166,667 ticks for a 10 ms frame — an independent
   confirmation of the documented CKI rate.
2. **Hand-off.** After the first frame the kernel sets `emr = $0200` — **timer only, vector 9** —
   with `tcon = $23` (enabled, auto-reload, source CKI/4), and re-arms the timer reload from the
   measured period. From here the *timer* is the frame heartbeat and the EXT1 pin is consulted
   only for overrun detection (§3.5).
3. **Phase-lock (slave configurations).** A slave DSP's kernel re-arms the timer every frame from a
   **host-maintained free-running counter** whose address sits at kernel+0x168, taking the counter
   modulo $3C00 and scaling by 25/24 — the arithmetic implies the counter runs at 1.536 MHz, the
   64 × 24 kHz serial-clock family [4] (*observed*; the counter's writer is host-side). On a
   single-DSP board +$168 is zero and this mechanism is inert (§2.4).

The sleep itself is the documented `waiti` powerdown — execution stops, peripherals and interrupt
sensing stay alive, and any unmasked interrupt (here: the timer) resumes it ([DSP3210](../../hardware/dsp3210.md)
§3.3.5).

### 3.7 The audio output path

One path, three owners, and the DSP only ever touches the middle:

```
host RAM rings (SingerDMAOuput)  <-- DSP writes processed frames (its bus, any time)
        ^  PSC SndOut DMA (double buffer, sndOutBase/sndSize)
        |
PSC sound engine --> serial frame bus --> Singer codec --> amplifiers --> Audio Line Out
```

- **The codec.** A 16-bit stereo codec conforming to the ITT ASCO 2300 specification [1] p. 16,
  transferring 256-bit frames of four 64-bit subframes on a serial bus; **subframe 1 is reserved
  for the system sound I/O**, the other three are free for DAV cards and the communications codec
  [1] pp. 38, 44. The board's sound I/O panel: Audio In 8 kΩ, 2 V rms maximum, 22.5 dB gain
  available; Audio Line Out 37 Ω, 0.9 V rms [1] p. 38.
- **The DMA.** The PSC's sound output channel is a dedicated double-buffer engine (not one of its
  nine general channels), 1 bit wide on the I/O bus with a 16-byte buffer, and **the highest-priority
  channel to the CPU bus** [1] Table 2-10, Table 2-11 p. 29. `sndOutBase` holds the ring's physical
  address, `sndSize` the half-buffer size **in sample frames**; the engine plays half A then half
  B and re-latches the base once per pass, with `sndPhase` exposing the position (§2.3) [4]
  (*observed*).
- **The DSP's role.** The production sound path programs `sndSize` = **240 sample frames** at every
  rate — 10 ms frames at 24 kHz [4] (*observed*; the rate table is §4.5) — and the ring buffers the
  host allocates ("SingerDMAOuput", $F10 bytes each: a 16-byte DSPFIFO record header plus 960 × 4
  payload bytes) are the sections the DSP's sound modules write into. The codec sample format is
  16-bit signed big-endian, interleaved left/right [4] (*observed*).

The clock family: the Singer master clock is 24.576 MHz, and the codec's three reachable rates —
24 000 / 32 000 / 48 000 Hz — are divided from it. **22 050 and 44 100 Hz are not codec rates**:
the machine produces them by running the codec at 24/48 kHz and resampling *inside the DSP*, with
the rate-converter module shipped as a `dspf` resource and two kernel-data BIO fields switching
the clock family (§4.5) [4] (*observed*; [5]). The Dev Note's marketing line — "16-bit digital
stereo sound I/O at sample rates up to 48 kHz, including the standard rate of 44.1 kHz" [1] p. 6 —
is delivered by that DSP resampler, not by the codec.

### 3.8 The capture path: PlainTalk microphone to recognizer

The sound-input path is the output path run backwards, with the DSP doing rather more of the work:

```
microphone / line --> Audio In --> Singer A/D --> serial frame bus
      --> PSC SndIn DMA (double buffer, sndInBase) --> host RAM ring (SingerDMAInput)
      --> DSP sound-input module (gain, format, rate) --> host client stream
```

- **Presence and source sensing.** The codec has four general-purpose digital input pins, sampled
  each frame and exposed as the low nibble of `singerStat` (§2.3); the board wires them as sense
  straps. The driver reads exactly two combinations [4] (*observed*): **BI4** selects the codec's
  four digital *output* pins between all-high and all-low (the output-port strap), and **BI3/BI1**
  are decoded by the `dcon` status selector into a three-way input-source enumeration —
  BI3 = 0 → nothing connected; BI3 = 1 ∧ BI1 = 1 → 1; BI3 = 1 ∧ BI1 = 0 → 2. Which of 1/2 the
  Sound control panel labels "microphone" is not decidable from the ROM (§6.6).
- **The microphone.** "Apple also offers a compatible high-quality microphone that is specifically
  designed for speech recognition applications" [1] p. 38, and the speech software is blunt: "for
  best results with speech recognition, the Macintosh Quadra 840AV or Macintosh Centris 660AV
  microphone is required" [1] p. 318.
- **Gain and routing.** The A/D gain ladder (bits 16–19 of `singerCtl`, 1.5 dB steps to +22.5 dB)
  and the per-channel input mux bits (bits 20–21, selecting between the codec's two input pin pairs
  per channel) are the only input-gain programming anywhere in the ROM driver — both via
  read-modify-write field selectors [4] (*observed*).
- **Sense changes interrupt at level 4.** When the sampled BI pins change, the PSC raises **L4 bit
  0 (SNDSTAT)**; the driver's handler re-reads `singerStat`, re-derives the output-port value, and
  write-back-clears the flag (§2.2) [4] (*observed*).
- **The DSP's share.** The capture stream is consumed by DSP modules: the standard sound-input
  module converts the codec's stream for the Sound Input Manager, and the PlainTalk speech
  recognition front end — 26 `dspf` modules covering AGC, endpointing, cepstra, vector
  quantization and rate conversion, plus its own module-loading component — takes its input
  **straight off the Real Time Manager/DSP stream**, with no Sound Input Manager in its path [5]
  (*observed* by resource inspection). Text-to-speech is the mirror image: synthesis runs on the
  68040, and the synthesized audio plays out through the same DSP output path as everything else
  [5].

### 3.9 The telecom path

The modem-port story is a sibling of the audio story. The port "supports the full range of Apple
GeoPort protocols, helping the computer communicate with a variety of ISDN and other telephone
transmission means by using external pods" [1] p. 22, and the DSP is the signal processor behind
it — "analog modem signal processing" is one of the three uses the Dev Note names for the part
[1] p. 13. The telecom driver "provides the interface between the Telecommunications
Manager/Communications Toolbox and the Real Time Manager, including a set of standard telecom
modules, plus modem, fax, and speech" [1] p. 66. The clocking wrinkle is ATECS (§1.3): external
clock in through the GeoPort connector, or crystals (15.0528/16.384 MHz) multiplied to the
45.1584/49.152 MHz PSC inputs for 44.1/48 kHz-family operation [1] p. 16. On the serial frame bus,
the "communications codec" subframes are the remaining TDM slots — the `sndComCtl` communications
rate field exists for them and is **never written by any shipped code** [4] (*observed*).

### 3.10 What the DSP is not wired to

For completeness, the negatives a reimplementation can otherwise waste time on: the video capture
chain (DMSD → scaler → CIVIC → VRAM) has no DSP involvement [1] pp. 30–33; the DAV connector's
audio lines are TDM slots on the Singer frame bus, not a DSP port [1] p. 44; SCSI, Ethernet,
floppy and serial DMA all belong to the PSC's general channels [1] p. 29; and the DSP's own SIO
and DMAC are unused (§1.6). The one system-level consequence cuts the other way: **the AV
software set has no non-DSP sound fallback at all** — every Sound Manager stream, including the
system beep, travels the DSP path once the driver is installed [4][5] (*observed*).

## 4. Programming model

Everything in this section is *observed* behaviour of the shipped ROM and system software [4][5]
unless marked otherwise. One framing fact first: **the machine's own boot never touches the DSP.**
The ROM's start-up path contains no DSP power-on test and no early sound primitives against it;
the entire contract below begins only when the system software opens the DSP driver — the Real Time
Manager's initialization, moved into the system file's start-up code so its strings could be
localized [4]. A machine with a dead or absent DSP boots to the Finder silently.

### 4.1 The software stack

| Layer | Lives in | What it is |
|---|---|---|
| Sound Manager | System file | the Toolbox API; on AVs it signs in a DSP output device |
| Sound output device | System Enabler 088 | a Component Manager `sdev` of type `'dsp '` ("Built-in"), whose code is the `sift` sound driver |
| Mixers, converters, decompressors | System Enabler 088 | Enabler `dspf`-family resources, including the AppleSRC rate converter |
| Real Time Manager | ROM (`rtmr` 0) | the host-side kernel of the whole facility: tasks, modules, sections, GPB, message pumps |
| DSP driver + DSP operating system | ROM (`DRVR` 51 `.3210 a` + the five `'3210'` segments: `boot`, `evt`, `core`, `patches`, `slave`) | the driver is "two distinct components: one works exactly like a standard Macintosh driver... The other component performs a similar function for the DSP operating system. It contains all DSP code that is hardware-dependent, as well as booting and restart code" [1] p. 66 |
| Standard DSP modules | ROM (`dspf` 128: `Input`, `Midput`, `Output`, `Player`…) | the sound team's task bodies |
| Speech recognition front end | PlainTalk software (disk) | 26 more `dspf` modules, loaded by name through the same Real Time Manager [5] |

Everything above the driver reaches it through a single trap, **`_DSPDispatch` ($ABF5) with a
16-bit selector in D0** [4]. The published selector range 0–107 is the application API
[1] pp. 124 ff.; the driver's own control/status surface is a separate table of **four-character
tags** (§4.4), and everything at 108 and above is private plumbing (named buffers, segment loading,
resource fetching).

### 4.2 Boot bring-up, stage by stage

The driver's Open routine allocates its globals, installs the three PSC interrupt handlers
(§3.3, §3.8) into the PSC interrupt table, enables the L4 SNDSTAT interrupt — and pointedly does
**not** enable L5: the DSP sources stay masked until a successful boot [4] (*observed*). The
device sign-in then loads the `'3210'` segments, allocates the named buffers, patches the boot
image, fills the kernel data area (§2.4) and signs in the I/O devices — **nine of them: the eight
BIO pins as sub-devices and the Singer sound device** (§4.7). The `boot` control selector then
runs the whole bring-up [4]:

1. **Stop the frame engine** — a clean slate: `pFrmIntEn` off, the DSP reset asserted, both L5
   sources disabled, the overrun latch cleared, the L5 flag cleared. The frame engine must be
   stopped before the DSP boots, or the first tick against an unready kernel is a frame overrun
   that kills the bring-up mid-flight [4] (*observed*; the recovery path of §3.5).
2. **Reload segments and buffers** (the four named buffers — `SingerDMAInput`/`SingerDMAOuput`
   $F10, `RT_Messages`/`TS_Messages` $800 — are disposed and recreated 16-byte-aligned in both
   logical and physical space).
3. **`StartProcessorRoutine`** — the DSP-side boot, below.
4. **`StartFramesRoutine`** — program the sound engine (§4.5).
5. **Enable L5** (`$83` → $154: both the message doorbell and FRMOVRN).
6. **Stage-3 handshake** — poll kernel+0x198 **bit 1** for up to $80000 iterations: proof that the
   first frame tick was serviced.

**`StartProcessorRoutine`** is the heart of the board bring-up, and its shape is unlike any other
coprocessor boot on a Macintosh: the host plants a seven-word **DSP3210 program at physical
address 0** and releases reset [4] (*observed*):

```
; 1. mask to IPL 7; save the 28 bytes at physical $00..$1B
; 2. entry = physical('boot' segment) + $0C
; 3. plant the stub:
[$00] $802F0004                  ; if(true) goto pc+4   -> $0C
[$04] $C0010000 | low16(entry)    ; r1 = (ushort24) low16(entry)   <- latent
[$08]  ** left untouched **       ; $00000008 is the 68040 bus-error vector
[$0C] $90210000 | high16(entry)   ; r1 = r1 <<| high16(entry)
[$10] $10210000                   ; call r1 (r1)         ; r1 <- $18
[$14] $9DE10800                   ; *r1 = (long) r1      <- latent: [$18] <- $18
[$18]  ...                       ; the stage-1 handshake cell
; 4. push dirty cache lines so the bus master can see the stub
;    (68040: CPUSHA DC   /   68030: CACR |= $800)
; 5. MOVE.B #$1,$21C(A0)          ; *** clear pdspReset — the DSP runs ***
; 6. poll (phys $18 & $000FFFFF) == $18, ~4096 tries   ; stage 1
; 7. restore the 28 bytes at 0; restore SR
; 8. poll kernel+$198 bit 0, ~8192 tries               ; stage 2
; 9. on any failure: MOVE.B #$81,$21C(A0)             ; back into reset
```

Three contracts hide in those nine lines:

- **Stage 1 is the `call`'s link-register write.** The DSP executes the stub; `call r1 (r1)`
  branches to the kernel entry and leaves the return address $18 in r1; the call's latent
  instruction `*r1 = (long) r1` then stores $18 into cell $18. The handshake completes **before the
  kernel has executed a single instruction of Apple's own image** — the host is watching the
  *calling convention*, not the kernel. The 20-bit mask on the poll is defensive, not meaningful.
- **Cell $08 is skipped on purpose.** $00000008 is the 68040's bus-error exception vector, and the
  cache-push inside the window can report a deferred write bus error there; clobbering it while
  running at IPL 7 with the vector table in the blast radius would be unrecoverable. $0C–$1B *are*
  clobbered for the duration, which is why the whole window runs with interrupts masked (*inferred*
  from the skipped word and the IPL-7 window; the store that jumps over exactly that word is
  observed).
- **The release is a bare `$01`.** No preceding `$83` hold-write exists anywhere in the boot path —
  power-on hardware reset is the only thing that put the chip in reset (§2.1).

The kernel entry itself — what the `call` lands on — is the first two words of the host-patched
boot segment: `r1 = (ushort24) $3B80; pcw = (short) r1`, then execution falls into the segment's
real code (§3.2 decodes $3B80). The boot segment loads `r22` (the vector table base) and the
pointers from the planted table, walks the kernel's own chunk chain (`core` ++ `slave`, then
`patches`) — the kernel interprets its own image; the host never parses it — and jumps through the
kernel-main slot [4] (*observed*; the chunk-loader detail is the [chip page](../../hardware/dsp3210.md) §4.2).

### 4.3 Steady state

Once running, the system is a pipeline with one hardware event per frame and everything else in
RAM [4] (*observed*):

```
frame tick (IR1N pulse, pFrmIntEn-gated)
  -> DSP vector 15 -> shared trampoline -> frame service:
       BIO poll/write (host->DSP signalling, §4.7) -> timer re-arm -> task scheduler
       -> real-time modules run once each (frame n data in, n-1 out, n-2 to the ring)
       -> base level parks in waiti until the timer
  -> (host side) frame interrupt at level 2 via the PSC's VIA2 emulation
  -> DSP messages drain: L5 bit 0 -> mask, ack, defer to level 2, defer again to a task,
       kdspProcessMessages walks RT and TS rings in 16-byte DSPMessage records,
       calling each record's msVector
```

The Dev Note describes the same pipeline at the application level: each frame begins with the
frame interrupt, real-time tasks all run once, timeshare tasks run cooperatively until the frame
ends, and "if there are no active timeshare tasks to be done, the DSP goes into sleep mode (shuts
itself down), using the wait-for-interrupt instruction" [1] p. 73. Message delivery is
interrupt-driven *to* the host and polled *by* the DSP: the Dev Note's rule that the DSP "must
check this section for new information in every frame" [1] p. 153 is implemented as the kernel's
per-frame BIO poll and the FIFO conventions the modules honour.

The driver keeps three state bits — DSP running, frames parked, "down" — and every selector below
is a guarded transition among them [4] (*observed*).

### 4.4 The driver's selector surface

The driver's Control/Status entry points are addressed by four-character tags through a 39-entry
table (per-client or global scope per entry). The ones that touch the board [4] (*observed*):

| Tag | What it does |
|---|---|
| `boot` | the full bring-up of §4.2 |
| `stop` | `pFrmIntEn` off; `$83` → $21C (reset); both L5 sources off; overrun latch clear; L5 flag clear |
| `pdwn` / `pwup` | park/resume the frame engine: `pFrmIntEn` off/on and nothing else — **no reset, no L5 change** |
| `debn` / `debx` | the debugger interlock (§4.6) |
| `smrt` / `fmrt` | get/set sample rate / frame rate (locked pairs, §4.5) |
| `smsz` | bytes per sample: constant 2 |
| `fram` / `feat` | device capability reads (the rate-index mask) |
| `dsdma` | per-direction sound DMA enable; **couples the codec mute bit to output DMA off** |
| `gain` / `attn` | A/D gain / D/A attenuation field read-modify-write (§3.8) |
| `isrc` | input-source select: the codec input mux bits |
| `dcon` | the input-source presence enumeration from `singerStat` (§3.8) |
| `evti` / `evtb` | peek/poke a kernel data-area longword, by index or bit |
| `bios` / `biod` / `biom` / `bioh` | the BIO port as a managed device (§4.7) |
| `mast` / `slav` | master/slave load mode (§3.6) |
| `phas` / `phro` | the phase counter address (the $F0 alias, §2.5) / the phase rollover value |
| `ptyp` / `dslt` / `dnam` / `ionm` | processor type ('3210') / slot / device and IO-device names |
| `fifo` | the two Singer DMA buffer records |
| `tstl` / `rttl` | the timeshare / real-time load blocks' addresses |

### 4.5 Sample rates and their persistence

`StartFramesRoutine` asks the loaded sound module for its sample rate and frame rate (16.16
fixed), divides, and programs the engine. The ROM's rate table — which also settles the frame
size [4] (*observed*):

| Index | Rate (Hz) | Frame rate (Hz) | `sndSize` (frames) | Codec rate | Mechanism |
|---|---|---|---|---|---|
| 0 | 22 050 | 91.875 | **240** | 24 000 | codec at 24 kHz + **DSP rate conversion** |
| 1 | 24 000 | 100.000 | **240** | 24 000 | direct |
| 2 | 44 100 | 183.750 | **240** | 48 000 | codec at 48 kHz + **DSP rate conversion** |
| 3 | 48 000 | 200.000 | **240** | 48 000 | direct |

(The 32 kHz codec rate is reachable in `sndComCtl`'s rate field but is not in the driver's table;
nothing shipped selects it.) The programming order matters: bases and `sndSize` first, kernel BIO
fields second, and **`sndComCtl` last, in a single write** that sets the rate code and `pFrmIntEn`
together — the frame engine, and therefore the DSP's clock, starts exactly once, atomically [4]
(*observed*). The 22.05/44.1 kHz rows additionally set two kernel-data BIO fields — BIO2 as an
output, driven high — which switch the clock family the resampler runs against [4] (*observed*;
the board-level meaning of BIO2's destination is §6.2).

A rate change does **not** retune a running machine: `smrt` (set) validates the index, stores it,
and returns the new rate — **no PSC write**; the new rate takes effect at the next `boot`. The
chosen index persists in **slot PRAM** (record byte $06, bits 5–4, stored as index − 1), and the
restore path decodes it back with 4 mapped to 0 — so a factory-zero PRAM yields **24 000 Hz** [4]
(*observed*).

### 4.6 Overrun handling and the debugger interlock

The FRMOVRN handler is the machine's one act of self-surgery [4] (*observed*):

1. Queue the notification handler as a deferred task (once).
2. **Clear `pFrmIntEn`** — the frame engine stops; no further ticks reach anyone.
3. **`$83` → $21C** — the DSP is killed, state cleared.
4. Disable both L5 sources (`$03` → $154).
5. Clear the overrun latch (`$04` → $21C), then the L5 flag (`$01` → $150) — in that order: the
   IR bit re-latches while the source latch is set, so the source must drop first.
6. The deferred task notifies every client — default reason code `'govr'`, or the four-character
   code the dying kernel wrote into its own crash block — and posts a Notification Manager alert.

**There is no automatic restart.** Recovery is a client's re-`boot`. The Dev Note describes the
same sequence at the API level: the offending task is set inactive, its application is notified,
all other clients are told the DSP has been restarted [1] p. 84.

The `debn`/`debx` pair is the one sanctioned way to stop the *host* instead of the DSP [4]
(*observed*): `debn` (debugger enter) **forces the frame engine on if parked**, masks FRMOVRN
(`$02` → $154) and **clears `pdspResetEn`** (`$02` → $21C) — so the DSP keeps servicing sound
autonomously through an indefinitely long host freeze, and the overrun machinery cannot kill or
reset it; `debx` (debugger exit) clears the accumulated overrun latch, re-arms `pdspResetEn` and
re-enables FRMOVRN. Neither path resets the DSP or re-runs any part of the boot. (The one observed
caller of the pair is a debugger hook; which software calls them at runtime is §6.9.)

### 4.7 The BIO port as a managed device

The board's only DSP→host wire is not treated as a private trick: the driver signs in **each of
the eight BIO pins as an I/O sub-device** (indices $10–$17), alongside the Singer sound device,
and the Real Time Manager exposes per-pin state, direction, change-notification and handler
slots to clients [4] (*observed*). The Dev Note documents the same surface from the application
side: a `DSPBIODeviceParamBlk` carries `bioMessageHandler` — "gets called when state of bio pin
changes" — plus `bioPinState` (0 = low, 1 = high), `bioPinDirection` and `bioPinIntEnable` [1]
p. 191, and the notification arrives as the `'biop'` message, `kdspBIOPinChangedState` [1] p. 185.

How is that implemented on a port the chip cannot interrupt with? **The "interrupt" is a
synthesized poll.** Once per frame, the kernel's BIO service routine rewrites the `bioc` direction
image from kernel data, reads the `bio` register, masks the watched input bits, compares against
the last-seen state, and — for every changed pin with a message template installed — posts the
template as a message through the ordinary doorbell [4] (*observed*). Host→DSP signalling rides
the same routine: the kernel-data direction byte (+$1BC), the per-tick write value (+$1B8) and the
per-pin watch mask are all host-writable fields, so the host can drive a BIO pin level or watch a
pin edge by writing kernel data — the selectors `bios`/`biod`/`biom`/`bioh` are exactly these
fields' get/set wrappers (§4.4). The doorbell itself (§3.3) is the same mechanism used in the
other direction, with the value field pre-loaded with "complement BIO0".

## 5. Quirks & errata

- **The DSP→host interrupt does not exist as hardware.** The DSP3210 has no interrupt-output pin
  and its BIO port has no interrupt logic ([2] §9.4); the "DSP interrupt" at level 5 is a GPIO pin
  (BIO0) toggled by one DSP store instruction per message, latched by the PSC (§3.3). Any board
  model that invents an interrupt line — or that expects a *level* — breaks the message path.
- **One enable bit clocks both processors.** `pFrmIntEn` gates the host's frame interrupt *and*
  the DSP's IR1N tick — the same tick, not two independently gated events (§3.4). Parking the
  frame engine ("power down") silences the DSP's world; a driver that fiddles with one and not the
  other desynchronizes the kernel's phase measurement.
- **The frame engine must be stopped before the DSP boots.** The `boot` selector's first act is a
  full stop (§4.2); a tick arriving against a half-booted kernel is a frame overrun that kills the
  bring-up through the very handler that is supposed to guard it (§3.5).
- **The boot release is a bare `$01`.** No code ever writes `$83` first at power-on — the chip is
  held in reset by hardware (§2.1). A model that requires an explicit pre-hold never releases the
  DSP.
- **Physical 0 is a hand-grenade, and the ROM knows it.** The boot stub clobbers $0C–$1B of the
  68040's exception vectors at IPL 7, skips $08 (the bus-error vector) on purpose, and restores
  everything within the window (§4.2).
- **Stage 1 of the handshake is a calling convention, not a flag.** The host polls for the `call`
  link-register write of $18 into cell $18 — a value the DSP's own instruction sequence produces
  before any Apple code runs (§4.2). There is no "boot done" register anywhere.
- **The overrun teardown order is load-bearing.** The source latch (`$04` → $21C) must be cleared
  before the interrupt flag (`$01` → $150), because the IR bit re-latches while the source is set
  (§4.6).
- **The L5 handler is deliberately software-edge.** Mask, acknowledge, defer twice, then re-enable
  only if the DSP is still marked running (§3.3). A plain level-triggered handler either livelocks
  the dispatcher or delivers interrupts from a dead DSP.
- **The level dispatchers never acknowledge, and they demand stable reads.** The ROM's dispatcher
  loops on `IR & IER` until zero and double-reads the IR until two reads agree; the handler owns the
  source, and an IR that reads unstably hangs the loop (§2.2).
- **`pdwn`/`pwup` are not the reset-enable writes.** The `$02`/`$82` writes belong to the debugger
  pair `debn`/`debx`; power-down and power-up are pure `pFrmIntEn` park/resume (§4.6). Earlier
  readings that attributed the writes the other way round got both pairs wrong.
- **Wake never re-boots the DSP.** Neither `debx` nor `pwup` re-runs any handshake; the DSP was
  never reset, only the overrun machinery disarmed (§4.6).
- **Mute tracks output DMA.** The `dsdma` selector couples the codec mute bit to the output DMA
  enable — DMA off implies mute — and the boot chime's codec settings are *inherited* by the DSP
  driver, which contains no `singerCtl` initialization at all (§4.4) [4] (*observed*).
- **44.1 kHz is not a codec rate.** 22 050 and 44 100 Hz are DSP resamplings of the 24/48 kHz
  codec rates; the clock family is switched by driving BIO2 as an output (§4.5). A codec model
  that "supports" 44.1 kHz directly masks the resampler's existence — and the recognizer's
  dependence on it.
- **The DSP frame is always 240 samples; the boot chime's is not.** The ROM's pre-driver beep path
  drives the same sound engine directly, with 960-frame half-buffers and no DSP in sight — proof
  that the sound engine, the codec and the PSC DMA are usable without the DSP, and the only thing
  the DSP path adds is `pFrmIntEn` [4] (*observed*).
- **The kernel data area lives in the `evt` allocation, not `core`.** Every "core+0x200" in older
  notes is really evt+0x200 (§2.4) — a correction that changes which buffer a reimplementation
  must size.
- **The DSP reads host I/O through an alias whose decode is inferred.** Only one pointer
  ($F073120C) witnesses the $F0 window (§2.5); the base-and-mask arithmetic is a reconstruction,
  not a documented fact.
- **The debugger shares the frame-tick pin.** Snoopy's break request arrives through the same
  external interrupt the frame tick uses (§3.4) — a debugger break during sound playback is
  indistinguishable from a very long frame, which is precisely what `debn` exists to survive.

## 6. Open questions

1. **The board half of the doorbell.** The kernel's code pins the DSP side to BIO0 (value $0003 =
   complement field 0), and the host's PSC bit is level-5 bit 0 — but no document or schematic
   states which PSC input pin BIO0 reaches, whether the latch is edge- or level-sensitive at the
   pin, or whether other BIO transitions can also set it. The identification "BIO0 → L5 bit 0" is
   an inference from software alone.
2. **What BIO1 is for.** The kernel makes BIO1 an output at the first frame and never drives a
   value for it; the plausible reading is a board-visible "kernel is frame-locked" level or a
   diagnostic, but nothing DSP-side distinguishes further (§3.6). BIO2's destination — the
   clock-family mux the 22.05/44.1 kHz rows switch — is likewise inferred, not wired down (§4.5).
3. **The frame tick's electrical shape.** That it is a short active-low pulse per frame is forced
   by the calibration gadget's two spins (§3.4); the actual pulse width, its phase against the
   codec frame boundary, and the generating circuit inside the PSC are not documented anywhere.
   Whether the host's level-2 frame interrupt and the DSP tick fire on the same edge is
   *inferred* from the shared gate.
4. **What host physical $50040000 decodes to.** The kernel treats it as the on-chip heap cursor's
   starting value (§2.4), and the chip's map says external memory A resumes there — but a sound
   module has been observed attempting a large copy through that address range at play time, and
   what the real board does with such an access (RAM? open bus? fault?) is unresolved.
5. **The $F0 alias decode.** Only the one `phas` pointer is observed (§2.5). Whether the window
   is really 8 MB at $F0000000, whether it is read-only, and which other I/O addresses the DSP
   ever legitimately reaches through it are open.
6. **Which `dcon` code means "microphone".** The three-way enumeration is pinned (BI3/BI1 →
   0/1/2), but the System-side consumer that labels 1 versus 2 lives in the Sound Manager's
   RAM-based code, and no ROM evidence decides it (§3.8).
7. **The set conditions of `pdspFrameOvr`.** The ROM only ever clears the latch; what exactly
   sets it — one unserviced tick, or the Dev Note's category-two count — and how the category-three
   "hardware reset to the DSP and I/O subsystems" is actually sequenced are undocumented (§3.5).
8. **`pdspResetEn`'s exact circuit.** "When cleared, disables the dspReset bit" pins the interlock's
   effect on *software* resets; whether clearing it also defeats the power-on hardware reset, and
   whether the category-three reset respects it, is not established (§2.1).
9. **Who calls `debn`/`debx` at runtime.** The pair exists precisely for a host-freeze scenario,
   and the debugger is the obvious caller, but the shipped ROM's dispatch table does not install
   the call site — it lives in the debugger or the System (§4.6). Selector `$92`, called at the end
   of device sign-in but absent from the ROM's installed selector list, is in the same boat.
10. **The strap values on BIO6/BIO5.** The first fetches after reset run under the *strapped* bus
    signalling until the kernel's `pcw` write lands (§1.5); the values are inferred from the
    kernel's own choice, not observed.
11. **How ATECS "synchronizes the DSP".** The Dev Note claims the DSP and sound subsystems can be
    synchronized to a GeoPort-external clock [1] p. 16, but CKI is a fixed oscillator per the
    clock table [1] Table 2-2. Whether the DSP's domain is actually touchable from ATECS, or only
    the PSC/codec clocks are, is unresolved.
12. **The host counter behind the slave phase-lock.** The 1.536 MHz counter at kernel+0x168 and its
    host-side writer are identified only for slave configurations; on a single-DSP board the field
    is zero and the mechanism unused, so the counter's exact source register has never been
    observed (§3.6).
13. **The `RT_Messages`/`TS_Messages` notification flag bits.** The FIFO record's flag bits 8 and
    9 (message-sent, message-lost) are observed; the remaining bit-to-notify-condition mapping
    (`'femp'`/`'fful'`/`'flow'`/`'fhig'`) follows the Dev Note's vocabulary but has not been proven
    bit by bit [4].
14. **The AV Power Macintosh variant.** Whether those machines' AV card carries this same part
    with this same wiring — or a different board integration entirely — remains outside the
    primary evidence set (§1.2).

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — Chapter 1 "Features" p. 6 (sound I/O via the DSP; GeoPort);
   Chapter 2 "Hardware Details": "Functional Units" pp. 12–13 (MCA, DSP — 66.6667/55.5000 MHz, 8 KB
   internal RAM — PSC), "Apple Telecom External Clock Synchronizer" p. 16, "Singer" p. 16,
   "System Clocks" Table 2-2 pp. 17–18 (CKI, usage "DSP, MCA"), "Signal Buses" p. 18, "Bus
   Arbitration" p. 18 (four masters; DSP asynchronous), "Bus Timeouts" p. 19 (16 DSP clock cycles),
   "Serial Ports" p. 22 (GeoPort), "PSC Functions" p. 29 with Tables 2-10/2-11 (nine DMA channels,
   SndIn/SndOut, priorities), "External Video Input" p. 32, "Sound I/O" p. 38 (jacks, levels, the
   speech-recognition microphone, Table 2-15), "DAV Sound Interface" p. 44 (TDM frame, subframe 1
   reserved for system sound); Chapter 3 "Introduction to Real-Time Data Processing":
   "Software Model" pp. 65–66 (Real Time Manager, DSP operating system, DSP driver, sound and
   telecom drivers), "Frame Organization" p. 73 (frame interrupt; sleep via wait-for-interrupt),
   "Frame Size Selection" p. 74 (240-sample framing; two-frame latency), "Visible Caching" pp. 75–76
   (bus-bandwidth share; block moves), "DSP and Main Processor Addressing" p. 77 (no MMU;
   locked contiguous non-cacheable DSPAddress blocks), "Frame Overruns" pp. 83–84 (categories 1–3;
   category-2 hardware interrupt and reset; category-3 hardware reset of the DSP and I/O
   subsystems); Chapter 4 "Real Time Manager": "Accessing the DSP" p. 137, "From DSP to Host" p. 152
   (interrupt-driven messages, deferred tasks, DSPProcessMessages), "From Host to DSP" p. 153
   (shared memory, checked every frame), "Summary of the Real Time Manager" pp. 185, 191
   (kdspBIOPinChangedState 'biop'; DSPBIODeviceParamBlk); "How Does Casper Work?" p. 318
   (microphone required for recognition); Appendix C "Snoopy User's Guide" p. 446 (stops at
   ExternalIntOne).
2. AT&T Microelectronics, *AT&T DSP3210 Digital Signal Processor Information Manual* (document
   MN91-006OMOS, September 1991) — §1.1 (33 MFLOP at 66.7 MHz); §3.2.3 powerdown; §3.4 and
   Figure 3-4 memory organization (processor-mode on-chip window); §6.1 and Table 5-2 pin
   descriptions (RESTN open-drain, BRN/BGN/BGACKN, AEN/DEN); §6.2 processor control word and
   reset-configuration latch; §6.3 wait states and SRDYN; §6.6.2 DRAM/system-bus signalling;
   §7.1 instruction cycle (CKI/4); §7.5.2.1 double error (IACK0+IACK1); §7.5.4 and Table 7-4
   reset sequence and register values; §7.5.4.1 reset timing; §9.1–§9.2 serial I/O and DMA
   controller; §9.4 bit I/O (no interrupt capability); Table 11-1 pin list.
3. AT&T Microelectronics, *AT&T DSP3210 Digital Signal Processor — The Multimedia Solution*,
   Data Sheet, March 1993 — "Timing Specifications" (55 MHz and 66 MHz columns, CKI 18/15 ns);
   Figure 8 (processor-mode memory map).
4. Apple Computer, Inc., Macintosh Quadra 840AV / Centris 660AV boot ROM (the shared 2 MB image,
   mapped at $40800000) — the Real Time Manager (`rtmr` 0 at $40991630), the DSP driver `DRVR` 51
   `.3210 a` ($4099B720) and the five `'3210'` segments ($4099CFD0 ff.); driver Open $4099B756,
   selector table $4099C350, DSPhndlr $4099BB10, FRMOVRNhndlr $4099BBA0, SNDSTAThndlr $4099BC90,
   NotificationHandler $4099BC40, StartFramesRoutine $4099BCC0, StartProcessorRoutine $4099BE40,
   InitializeEVTandKernelSegments $4099C120, the boot/stop/debn/debx/pdwn/pwup and rate
   selectors, the boot-chime player and its PCM; the PSC level-3…6 interrupt dispatchers and the
   deferred level-2 sound machinery — annotated disassembly and observed execution; every fact
   marked *observed* in §2–§5.
5. Apple Computer, Inc., AV system software for the Quadra 840AV / Centris 660AV (System 7.1 AV,
   System Enabler 088, and the PlainTalk speech recognition software) — the `sift`/`gnth` −16565
   "Built-in" sound device, the `dspf` task modules (the standard sound team, the AppleSRC rate
   converter, and the recognizer's 26-module front end), and the observation that text-to-speech
   synthesizes on the 68040 — resource inspection, disassembly and observed execution.
