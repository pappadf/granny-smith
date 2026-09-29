# AMIC — the PDM I/O Controller

**Contents:**

1. [Overview](#1-overview) — identity, system role, bus topology, revisions
2. [Register file](#2-register-file) — address decode, pseudo-VIA1, pseudo-VIA2, interrupt
   control, DMA file, sound block, video block, diagnostic and machine-ID registers
3. [Behaviour](#3-behaviour) — the interrupt line, nanokernel dispatch, DMA channels,
   sound cadence, video timing, reset state
4. [Programming model](#4-programming-model) — cold boot, interrupt service, per-channel
   driver sequences, the boot beep
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

The **Apple Memory-Mapped I/O Controller (AMIC)** is a 160-pin gate array that performs
most I/O logic and control for the first-generation Power Macintosh platform (internal
codename PDM) [1] p. 15. It appears, as the same die, in every model of the family:

| Model | Codename | Notes |
|---|---|---|
| Power Macintosh 6100/60, 6100/60AV | PDM ("Piltdown Man") | no second SCSI channel, no on-board NuBus |
| Power Macintosh 7100/66, 7100/66AV | Carl Sagan | three NuBus slots |
| Power Macintosh 8100/80, 8100/80AV | Cold Fusion | three NuBus slots, fast SCSI channel |
| Power Macintosh 6100/66, 7100/80, 8100/100, 8100/110 | — | Enhanced variants; no AMIC changes are documented [2] |

AMIC concentrates, on one die [1] pp. 15–16, [3] sheet 12:

1. **The classic-Mac interrupt model.** The 601 has a single external interrupt line; AMIC
   emulates the MC68000 seven-level structure by providing a pseudo-VIA1, a
   pseudo-VIA2-style slot-and-device interrupt bank, and a top-level interrupt control
   register that drives the 601's `INT` line [1] pp. 22–23. There is no physical 6522 on
   the board — the Cuda microcontroller is wired directly to AMIC pins that implement the
   VIA1 shift-register handshake [3] sheets 12, 16.
2. **A DMA engine** with channels for SCSI (two on the 8100), Ethernet receive and
   transmit, the SWIM III floppy controller, the SCC serial ports (transmit and receive,
   two ports), and sound input and output [1] pp. 15–16.
3. **The serial sound engine**: AMIC owns the register window at `$50F14000`, runs the
   256-bit TDM frame bus to the AWACS codec, and moves sound data between that bus and
   RAM [1] pp. 46–48, [3] sheet 12.
4. **Built-in video support**: video timing, dot-clock selection, sync and blanking
   generation, monitor-sense lines, and the read side of the RAM-based video FIFO
   ("monitor support" in Apple's function list) [1] p. 16, [3] sheet 12.
5. **All I/O-space address decode.** AMIC receives the full 32-bit CPU address bus and
   decodes the I/O region internally; the peripherals see only chip selects and low-order
   buffered address lines [3] sheet 12.
6. **The Cuda interface and reset fan-out.** The 5-wire Cuda transport is
   register-emulated behind the pseudo-VIA1 shift register; Cuda can reset the machine
   through AMIC's reset input, and AMIC drives the `RESET` line to every peripheral; the
   programmer's-switch NMI enters through Cuda as `CudaNmi*` [3] sheet 12.

### Bus topology

AMIC is a slave and a DMA master on the 601 CPU bus (full address and transfer-control
pins), but has **no 64-bit data-bus pins**: all register data and all DMA data move over
the 16-bit `IOData<15..0>` bus through the two Data Path chips, which bridge
`CPUData(64) ↔ MemData(64) ↔ IOData(16)` under AMIC control [3] sheets 5, 12. DMA bus
mastership is arbitrated by the HMC: AMIC requests the bus with `IOReq*` and receives
`IOGrant*` [3] sheets 4, 12. The arbitration order is [1] Table 2-4, p. 20:

DRAM refresh > video refresh > SWIM III DMA > AWAC (sound) DMA > SCSI DMA > SCC DMA >
expansion card > main processor.

(The Ethernet DMA channels are not listed in the developer note's table; their
arbitration position is unknown — see §6.)

### Error behavior

AMIC does not support the PowerPC 601 extended transfer protocols; using them on AMIC
space causes a transfer error exception [1] p. 16. An access to an address the MMU maps
but AMIC does not decode makes AMIC assert a bus error after **40 µs**; Apple documents
the condition as not generally recoverable [1] p. 21.

### Access conventions

- Every AMIC register is **byte-wide**. Multi-byte quantities (DMA addresses, counts) are
  written as consecutive byte accesses, most significant byte first [4], [5].
- System software inserts `nop`s after I/O writes to serialize accesses; a read following
  a write must observe the written value — no write posting is visible to software [4].
- The system maps the I/O block as one 256 KB region at `$50F00000` [4].

### Silicon revisions

Three revisions matter to software; all are detectable through register behavior [4]:

- **EVT1 ("old AMIC")**: lacked the handshaked pseudo-DMA path and serial DMA; an
  early-silicon erratum returned `$FF` on the first read of a register (workaround code
  survives in the ROM's Ethernet driver) [4]. EVT1 boards were never shipped.
- **AMIC-2**: distinguished by the SCSI control register's I/O-bus-speed field (bits 3:2
  of `$50F32008`, §2.5) becoming writable and readable; on EVT1 parts those bits read
  back 0. The ROM's identity code probes exactly this [4] ($FFF032B8).
- **AMIC ≥ 3** (every shipped board): the slot-interrupt enable bit 2 of `$50F26012` is
  implemented and sticks when written; on earlier parts the slot lines were also wired in
  a different order [4]. A model of a production machine passes both probes.

## 2. Register file

### 2.1 I/O-space address decode

Everything AMIC decodes lies in `$50F00000–$50F3FFFF`, plus the machine-ID register at the
top of the I/O region. The block bases, as fixed in the ROM's platform decoder records
and printed in the developer note's register figure [1] Figure 2-2, [4]:

| Base | Block | Owner | Notes |
|---|---|---|---|
| `$50F00000` | Pseudo-VIA1 | AMIC | classic VIA map, register *n* at `base + n·$200` (§2.2) |
| `$50F04000` | SCC (85C30 core in Curio) | Curio | read and write base identical |
| `$50F08000` | Ethernet station-address PROM | board PROM | 32×8; driver reads byte *n* at `$50F08001 + n·$10` [4] |
| `$50F0A000` | MACE Ethernet registers (in Curio) | Curio | 16-byte register stride |
| `$50F10000` | SCSI-A: 53C96 core (in Curio) | Curio | 16-byte register stride |
| `$50F10100` | SCSI-A pseudo-DMA data port | AMIC | 16-bit byte-swapped aperture for handshaked pseudo-DMA (*inferred — unverified*) |
| `$50F11000` | SCSI-B: 53CF96 fast SCSI | 53CF96 | 8100 only; 16-byte register stride |
| `$50F14000` | Sound block | AMIC | AWACS codec access + AMIC sound DMA, 0x20 bytes (§2.6) |
| `$50F16000` | SWIM III floppy registers | SWIM3 | **512-byte register stride** (`A0–A3` fed from `BufAddr<9..12>`) [3] sheet 20 |
| `$50F24000` | Ariel II video CLUT/DAC | Ariel II | 4 byte registers selected by `RS0/RS1` = `BufAddr<0..1>`: address, data, control, color key; CLUT data accesses auto-increment the address; DAC depth codes `$08/$09/$0A/$0B/$0C` = 1/2/4/8/16 bpp [4], [5] |
| `$50F26000` | Pseudo-VIA2 interrupt/config bank | AMIC | byte registers; the 32-byte file mirrors across the whole `$50F26000–$50F27FFF` island (§2.3) |
| `$50F28000` | Video control block | AMIC | mode/depth/sense/test + beam counters (§2.7) |
| `$50F2A000` | Top-level interrupt control | AMIC | ICR at `+0`, DMA flag registers at `+$8`/`+$A` (§2.4) |
| `$50F2C000` | Diagnostic/strap register | AMIC | bit 0 must read 1 (§2.8) |
| `$50F31000` | DMA register file | AMIC | through `$50F322xx` (§2.5) |
| `$50F40000` | HMC configuration port | AMIC select, HMC data | AMIC asserts `HmcCS*`; a 35-bit shift register is loaded serially, one bit per byte access on `IOData<0>`: a write to `+$8` resets the bit pointer, then 35 byte accesses to `+$0` shift bit 0 of each [3] sheet 4, [4] ($FFF03838) |
| `$5FFFFFFC` | Machine-ID register | AMIC | byte-wide, read-only (§2.8) |

The BART NuBus controller at `$F0000000` is not decoded by AMIC.

### 2.2 Pseudo-VIA1

AMIC emulates a 6522 VIA1 with the classic Macintosh addressing: register *n* of a 6522
appears at `$50F00000 + n·$200` [1] Figure 2-2, which prints the VIA1 IFR at
`$50F01A00` and IER at `$50F01C00`. Because the ROM initializes and drives it through the
completely standard Macintosh VIA1 path [4], the full 6522 register semantics must be
preserved — the IER set/clear convention (bit 7 of the written byte selects set or
clear), IFR write-1-to-clear with bit 7 ignored, and IFR bit 7 computed as the OR of the
enabled bits 6–0 [6]. Register *n* = 1 (`ORA/IRA` without handshake) is unused, as on
classic Macs.

Register usage on the PDM [1] Figure 2-2, [4]:

| Register | Offset | PDM function |
|---|---|---|
| ORB/IRB | `+$0000` | Port B: Cuda handshake + sound enable (below) |
| DDRB | `+$0400` | direction: TREQ in; BYTEACK, TIP, SndEnb out |
| DDRA | `+$0600` | direction: vSync, vHeadSel out; vSCCWrReq in |
| T1C-L/H, T1L-L/H | `+$0800/0A00/0C00/0E00` | standard VIA timer 1 (Time Manager, beep timing) |
| T2C-L/H | `+$1000/1200` | standard VIA timer 2 |
| SR | `+$1400` | Cuda byte transport, external-clock shift mode |
| ACR | `+$1600` | SR mode 011 (shift in under external clock); T1/T2 one-shot |
| PCR | `+$1800` | 0: CA1 falling edge, CA2 in, CB1 falling edge (Cuda clock), CB2 in (Cuda data) |
| IFR | `+$1A00` | bit 7 IRQ, 6 TMR1, 5 TMR2, 4 VIA CLK, 3 VIA DATA, 2 SHFT REG, 1 "60.15 HZ" tick, 0 CA2 |
| IER | `+$1C00` | same bit assignment, VIA set/clear convention |
| ORA/IRA | `+$1E00` | Port A (below) |

Interrupt sources into the pseudo-VIA1:

- **CA1, 60.15 Hz tick** — the classic one-per-vertical-retrace interrupt, generated by
  AMIC's own video timing (60.15 Hz is the frame rate of the 12" 512×384 mode) [1]
  pp. 23, 39. It ticks regardless of which monitor mode is selected.
- **CA2** — the Cuda one-second/real-time-clock line (classic Egret/Cuda wiring) [1]
  Figure 2-2.
- **CB1/CB2 and SR** — Cuda byte transport: the shift register runs in external-clock
  mode, clocked by the Cuda, exactly as on the Egret-based Quadras [3] sheet 16.
- **T1/T2** — the emulated VIA timers. They tick at the classic VIA rate of
  **783.36 kHz** (the 31.3344 MHz I/O clock divided by 40), which every Mac OS timing
  loop depends on (*inferred — unverified*: the rate is the classic-Mac VIA timer rate
  and is consistent with the on-board 31.3344 MHz oscillator [1] Table 2-3; no
  register-level source states AMIC's divisor directly).

Port bits [4]:

- **Port A** (at `+$1E00`): bit 3 `vSync` (sync-modem enable, output, reset value 0);
  bit 5 `vHeadSel` (floppy head select, output, reset value 1); bit 7 `vSCCWrReq`
  (SCC wait/request status, input). The other port A bits are unconnected — the classic
  overlay/ROM bits do not exist on this machine.
- **Port B** (at `+$0000`): bit 3 `vCudaTREQ` (Cuda transfer request, input, reset 1);
  bit 4 `vCudaBYTEACK` (output, reset 1); bit 5 `vCudaTIP` (output, reset 1); bit 7
  `vSndEnb` (output, reset 1 = sound subsystem held in reset). The classic RTC lines
  (PB0–PB2) do not exist — clock and parameter RAM travel in Cuda packets.

Deltas against a real 6522: only the functions above are wired; PB0–2, PB6, CA2 output
modes, the PB7-T1 output mode and the port latching modes have no external effect. The
developer note's figure draws the IFR as read-only, but the ROM's level-1 handler clears
IFR bits by writing them, so the write-1-to-clear convention holds [4]. The timer and
shift-register edge cases are those of the MOS 6522 core re-implemented in the gate
array; exact reload/latch quirks are unverified (§6).

### 2.3 Pseudo-VIA2 interrupt bank

The second interrupt bank at `$50F26000` follows the register model of the Sonora chip's
VIA2-substitute block; the ROM's decoder table names it "RBV" [4]. Byte registers at the
compact offsets [1] Figure 2-2, [4]:

| Offset | Register | Function |
|---|---|---|
| `+$00` | VIA2 data | unused on this platform |
| `+$01` | RAM size | DRAM configuration (Sonora path); nothing on PDM reads or writes it — treat as inert |
| `+$02` | **Slot IFR** | slot and VBL interrupt flags |
| `+$03` | **Device IFR** | SCSI/floppy/any-slot interrupt flags |
| `+$04` | VRAM size | VRAM configuration; the PDM has no VRAM and the sizing path is skipped [4] |
| `+$05` | Speed register | CPU speed / wait-state configuration (Sonora path); unused on PDM |
| `+$12` | **Slot IER** | enables for `+$02` |
| `+$13` | **Device IER** | enables for `+$03` |

**Partial decode.** The bank decodes only the **low five address bits**, mirroring the
32-byte register file across the entire `$50F26000–$50F27FFF` island. This is
load-bearing: the SCSI driver addresses the bank at the compact offsets (`$50F26003`,
`$50F26013`), while the ROM's generic level-2 interrupt dispatcher reads the same
registers at classic-VIA stride off the same base — device IFR at `base + $1A03`
(`$50F27A03`) and device IER at `base + $1C13` (`$50F27C13`), whose low five bits alias
to the compact offsets [4] (dispatcher at `$408104A0`, base pointer in low memory
`$0CEC`). A decode that serves only the compact addresses feeds the dispatcher zeros, and
the still-asserted interrupt level is never serviced.

Slot interrupt register bits [1] Figure 2-2:

| Bit | Slot IFR (`$50F26002`) | Slot IER (`$50F26012`) |
|---|---|---|
| 7 | reads 0 | SET/CLR sense bit |
| 6 | VBL (built-in video) | VBL enable |
| 5 | SLT3 — slot `$E` (PDS) | SLT3 enable (*absent on 6100/60*) |
| 4 | SLT2 — NuBus slot `$D` | SLT2 enable |
| 3 | SLT1 — NuBus slot `$C` | SLT1 enable |
| 2 | SLT0 — NuBus slot `$B` | SLT0 enable |
| 1–0 | read as 1 | unused |

Slot bit *n* corresponds to slot `$9 + n`: bits 2–4 are the three physical NuBus slots
(`NuBusIrq<0..2>*`), bit 5 is the PDS line (`PdsIrq*`, the video/AV card slot — on the
6100 it is the adapter card's single NuBus slot) [3] sheets 12, 22, [4]. The slot lines
are active-low and level-sensitive: the register **reads an asserted line as 0**; unused
bits 1–0 read 1; reset value is `$7F` (nothing asserted). The interrupt handler ORs the
read with `~$7F`, inverts, and ANDs with the IER — the RBV convention. Bits 6–2, gated by
the IER, OR together into the device bank's "ANY SLOT" bit. Writable IER bits: `$78`.

Only VBL is software-clearable: it latches on vertical blank (driving the bit low in the
active-low register) and is deasserted by writing `$40` — in the active-low convention,
writing the bit sets it back to 1. The ROM's video code clears the flag with a `$40`
write and then spins until bit 6 reads 0, waiting for the next blanking period [4].

Device interrupt register bits [1] Figure 2-2:

| Bit | Device IFR (`$50F26003`) | Device IER (`$50F26013`) |
|---|---|---|
| 7 | VIA2 IRQ — OR of enabled bits 6–0 | SET/CLR sense bit |
| 6 | SCSI-B IRQ (*8100*) | SCSI-B enable (*absent on 6100/60, 7100/66*) |
| 5 | FDC IRQ (SWIM III chip interrupt) | FDC enable |
| 4 | reads 0 | unused |
| 3 | SCSI-A IRQ | SCSI-A enable |
| 2 | SCSI-B DRQ (*8100*) | SCSI-B DRQ enable (never used by system software) |
| 1 | ANY SLOT (from the slot bank) | ANY SLOT enable |
| 0 | SCSI-A DRQ | SCSI-A DRQ enable (never used by system software) |

- The IFR accepts VIA-style flag writes with bit 7 ignored; the SCSI driver writes `$88`
  to it to clear (a no-op for the level bits), and the ROM's slot handler writes `$82`
  (bit 7 + bit 1) to acknowledge the any-slot aggregate before scanning the slot IFR [4].
  The individual flag bits are **level** views: the SCSI IRQ bits mirror the 53C9x INT
  pins and the FDC bit the SWIM III interrupt, and clear when the chip deasserts.
- The DRQ bits are live views of the chips' DREQ pins, used for polled pseudo-DMA;
  nothing latches or acknowledges them.
- The IER uses the bit-7 SET/CLR convention. Writable enable bits: `$7B` (the fast-SCSI
  driver's enable value `$C8` — SET plus bit 6 — proves bit 6 is writable on the 8100;
  the DRQ enable bits 0 and 2 are never set by shipped software, and their writability
  is unverified) [4], [5].
- The bank drives 68k interrupt **level 2**; device IFR bit 7 feeds ICR bit 1 (§2.4).
- Dispatchers re-read these registers in loops, so reads must be free of side effects.

### 2.4 Interrupt control and DMA flag registers

The interrupt control register (ICR) at `$50F2A000` is the top of the interrupt tree and
the only register the PowerPC-side code reads [1] Figure 2-2, [4], [5]:

| Bit | Read (1 = asserted) | Write |
|---|---|---|
| 7 | `CPUINT` — the CPU INT output is (latched) asserted | write 1: acknowledge — clear the latch, drop INT |
| 6 | `INTMODE` — 1 = change-latched mode | mode select |
| 5 | NMI (Cuda `CudaNmi*`, the programmer's switch) | — |
| 4 | `DMAIRQ` — OR of all DMA-channel flags (`$50F2A008/A`) | — |
| 3 | Ethernet (MACE chip) interrupt | `MACEIE` — enable the MACE interrupt to the CPU (status and enable share the bit position) |
| 2 | SCC chip interrupt (port A or B) | — |
| 1 | pseudo-VIA2 IRQ (device IFR bit 7) | — |
| 0 | pseudo-VIA1 IRQ (VIA1 IFR bit 7) | — |

Bits 5–0 are live level views of the six aggregated sources; a single read returns the
complete source picture and is non-destructive. Bit positions and the dispatch behavior
below are established by the ROM's nanokernel interrupt handler [4] (`ExtIntHandlerPDM`,
ROM `$FFF10780`, flag-to-level table at `$FFF10740`) and agree with the released MkLinux
platform code [5].

Two byte-wide DMA flag registers mirror the per-channel completion flags [4], [5]:

| Bit | `$50F2A008` | `$50F2A00A` |
|---|---|---|
| 0 | SCC port B receive (`hwAmicRXB`) | Sound in (`hwAmicSIN`) |
| 1 | SCC port B transmit (`hwAmicTXB`) | Sound out (`hwAmicSOUT`) |
| 2 | SCC port A receive (`hwAmicRXA`) | — |
| 3 | SCC port A transmit (`hwAmicTXA`) | — |
| 4 | Ethernet receive (`hwAmicERX`) | — |
| 5 | Ethernet transmit (`hwAmicETX`) | — |
| 6 | Floppy (`hwAmicFDC`) | — |
| 7 | — (unused) | — |

Each bit mirrors the interrupt flag (IF) of the corresponding channel control register
(sound: the flags in `$50F14014`/`$50F14018`, §2.6), gated by that channel's interrupt
enable. The flag registers are **read-only mirrors**; a flag clears only by clearing the
underlying channel's IF. Any nonzero bit asserts ICR bit 4. These registers are read only
by 68k-side dispatch code; the PowerPC side uses the ICR alone [4].

### 2.5 DMA register file

The DMA engine's registers live at `$50F31000` and up [4], [5]:

| Offset (absolute) | Register |
|---|---|
| `+$0000–0003` (`$50F31000`) | DMA window base: byte `+$0` = bits 31:24, `+$1` = 23:16, `+$2` = 15:8, `+$3` = 7:0; only bits 31:18 are significant (the window is 256 KB-aligned; software writes the top two bytes) |
| `+$0C20` (`$50F31C20`) | Ethernet transmit control/status |
| `+$1000–1003` (`$50F32000`) | SCSI-A buffer address, 32-bit, MSB at `+$0` |
| `+$1004–1007` (`$50F32004`) | SCSI-B buffer address (8100) |
| `+$1008` (`$50F32008`) | SCSI-A control (plus the I/O-bus-speed field, bits 3:2) |
| `+$1009` (`$50F32009`) | SCSI-B control |
| `+$1010` (`$50F32010`) | SCSI-A current address, read-only (*inferred — unverified*) |
| `+$1014` (`$50F32014`) | SCSI-B current address, read-only (*inferred — unverified*) |
| `+$1028` (`$50F32028`) | Ethernet receive control/status |
| `+$1030` (`$50F32030`) | Ethernet receive head pointer (AMIC-written page index) |
| `+$1034` (`$50F32034`) | Ethernet receive tail pointer (software-written page index) |
| `+$1044/$1045` | Ethernet Tx set-0 count, high/low byte |
| `+$1054/$1055` | Ethernet Tx set-1 count, high/low byte |
| `+$1060–1063` | Floppy buffer offset (bits 15:8 at `+$1062`, 7:0 at `+$1063`) |
| `+$1064/$1065` | Floppy count, high/low byte (16-bit) |
| `+$1068` | Floppy control/status |
| `+$1080..` | SCC channel block 0: address long at `+$0`, count at `+$4`/`+$5`, control at `+$8` (blocks every `$10`: `$1080`, `$1090`, `$10A0`, `$10B0`) |
| `+$1100/$1102` (`$50F32100/2`) | DMA bus-error enable / flag, 16-bit each; no driver touches them and no behavior is established (§6) |

Each DMA channel has one byte-wide control/status register with a shared bit vocabulary
[4], [5]:

| Bit | Name | Semantics |
|---|---|---|
| 0 | `RST` | soft reset: write 1; self-clearing; leaves the channel stopped with the interrupt enable off |
| 1 | `RUN` | channel enable: set to start, clear to stop |
| 2 | `CONT` | (SCC channels) continuous/ring mode — keep running and wrapping |
| 3 | `IE` | interrupt enable: gates the channel IF into the DMA flag registers |
| 4 | `PAUSE` / `FLUSH` | SCC: pause request (poll for frozen); SCSI: flush — write 1, poll until it self-clears |
| 5 | `FROZEN` / `SET0` | SCC: read-only frozen status after a pause; Ethernet Tx: set-0 "buffer free" status |
| 6 | `DIR` / `OVRRUN` / `SET1` | SCSI and floppy: direction, 1 = memory-to-device (write); Ethernet Rx: overrun latch, write-1-to-clear; Ethernet Tx: set-1 "buffer free" status; SCC: set by the receive-arm sequence — function unresolved (§6) |
| 7 | `IF` | interrupt flag: sets on completion; **write 1 to clear** |

The completion signal is IF plus the flag bit in `$50F2A008/A`; there is no terminal-count
status bit, and (except Ethernet transmit) RUN stays set at completion for the software to
clear.

### 2.6 Sound block

The 0x20-byte window at `$50F14000` is AMIC's: it carries the serial interface to the
AWACS codec plus the sound DMA engine's control and status. The map is established by the
ROM's boot-beep code [4] (HWInit sound path, `$FFF0551C–$FFF056C8`) and the released
MkLinux AWACS platform driver [5]:

| Offset | Register | Bits |
|---|---|---|
| `+$00–02` | Codec control (24-bit, big-endian) | `+$0` strobe/mode byte: bit 7 = latch pulse (the ROM idles it at `$40` and pulses `$C0`), bit 6 = busy/command hold; bit 7 also flags an extended codec. `+$1/+$2` = 16-bit codec command: `register# << 12 \| 12-bit value` [4], [5] |
| `+$04–06` | Codec status (24-bit, read) | bit 0 = NOT microphone present, bit 1 = line-in sense, bit 3 = headphone connected (the ROM mutes the speaker when set); `$0F00` manufacturer ID, `$F000` revision; overflow/valid flags in bits 18–23 [4], [5] |
| `+$08/$09` | Buffer size, high/low | frames (one 4-byte stereo frame each) per ping-pong buffer; the ROM programs `$0400`, MkLinux `$07FF` [4], [5] |
| `+$0C–0E` | Phase (read-only) | free-running frame counter within the current buffer, plus intra-frame clock bits: `+$0C` = 6-bit clock count \| (phase & 3) << 6; `+$0D` = phase >> 2; `+$0E` = phase >> 10; wraps at the buffer size [4], [5] |
| `+$10` | Sound control 0 | bit 0 = output DMA run; bits 2:1 = sample rate: 0 = 22 050, 1 = 29 400, 2 = 44 100 Hz (the 45.1584 MHz master divided by 2, 64 and {16, 12, 8}) [4], [5] |
| `+$11` | Sound control 1 | bit 7 = input DMA run; bit 6 = frame interrupt enable (*inferred — unverified*); bits 5:2 = output subframe select, bits 1:0 = input subframe select (which TDM subframe carries system sound; the system uses subframe 1) [1] p. 47, [4], [5] |
| `+$14` | Sound-in DMA control/status | flags in the high nibble, enables in the low nibble: bits 7/6 = buffer 0/1 completion IF, bit 5 = error (overrun), bit 4 = additional input status; bits 3/2 = buffer interrupt enables, bit 1 = error interrupt enable. Flags are write-1-to-clear; enables are written directly [4], [5] |
| `+$18` | Sound-out DMA control/status | same layout without bit 4: IF0/IF1/ERR at `$80/$40/$20`, IE0/IE1/IER at `$08/$04/$02`; flags write-1-to-clear. Bit 6 (`$40`) pairs with the buffer at window offset `+$10000`, bit 7 (`$80`) with `+$12000` [4], [5] |

Interrupt generation: `(flags >> 4) & enables` nonzero asserts the direction's flag bit in
`$50F2A00A` (bit 0 in, bit 1 out) → ICR bit 4 → 68k level 4, selectors 8/9.

The codec's own gain, mute and attenuation registers (codec registers 0, 1, 2, 4) are
reached through the command channel at `+$00–02`; their layout is a property of the AWACS
codec, not of AMIC, and is covered with the sound subsystem. The TDM frame bus — 256 bit
clocks per frame, four 64-bit subframes of two 32-bit slots (left, right), 20 data bits
plus 12 auxiliary bits per slot, subframe 1 reserved for system sound, frame sync two
clocks wide and word sync one — is specified by the developer note [1] pp. 47–48 and
carried on AMIC pins also routed to the PDS connector [1] Table 3-16.

### 2.7 Video control block

The video control block at `$50F28000` implements the Sonora video-control register
model (the PDM's video I/O is a Sonora derivative) [4]:

| Offset | Register | Bits |
|---|---|---|
| `+$0` | Mode | bit 7 = blanking (1 = blanked, syncs off); bits 4:0 = monitor timing code |
| `+$1` | Depth | pixel depth code, `& 7`: 0/1/2/3/4 = 1/2/4/8/16 bpp; 16 bpp only in the 512×384 and 640×480 modes [1] p. 36 |
| `+$2` | Monitor sense | writes drive/tristate the three sense lines (bit 3 set = float all; line A = bit 2, B = bit 1, C = bit 0); reads return the sensed line levels in the upper nibble, read then `lsr #4` [4], [5] |
| `+$3` | Test | bit 0 (*inferred — unverified*) |
| `+$4/$5` | Horizontal beam position | read-only counter, high 3 bits / low 8 bits (*inferred — unverified*) |
| `+$6/$7` | Vertical beam position | read-only counter, high 2 bits / low 8 bits (*inferred — unverified*) |

Timing is generated inside AMIC per mode code — there are no programmable horizontal or
vertical timing registers [4], [5]. The mode codes, from the ROM's declaration-data video
parameter nodes [4] with the timings of [1] Tables 3-8/3-10 and the released MkLinux
platform driver [5]:

| Code | Timing |
|---|---|
| 1 | Portrait/Full-Page 640×870 @ 75 Hz, 57.2832 MHz dot clock |
| 2 | 12" 512×384 @ 60.15 Hz, 15.6672 MHz (this frame rate is the VIA1 tick) |
| 6 | Hi-Res 640×480 @ 66.67 Hz, 31.3344 MHz |
| 9 | 16" 832×624 @ 74.55 Hz, 57.2832 MHz (also the multiscan codes) |
| 10 | 512×384 variant, 560 wide |
| 11 | VGA 640×480 @ 59.94 Hz, 25.175 MHz |
| 13 | Hi-Res-400 640×400 |

The dot clocks are synthesized inside AMIC from the board's fixed oscillators (31.3344,
25.175, 57.2832 MHz; the 45.1584 MHz sound oscillator is not a video clock) [1]
Table 2-3, [3] sheet 11. The register at `+$2` also serves as a power-on self-test
diagnostic output: the ROM's diagnostic module writes status bytes to `$50F28002` [4]
(`$FFF30180`).

Video is RAM-based: there is **no framebuffer-base register** in AMIC. The scan-out base
is a property of the HMC configuration (physical 0 in the state the system ROM programs,
or `$100000` when reconfigured), and the system software accordingly forces its
framebuffer allocation to the bottom of DRAM [4], [5]. The vertical blanking interrupt
(VBL) is slot-bank bit 6 of `$50F26002` (§2.3), raised by AMIC's own vertical timing;
there is no external VBL line [3] sheet 12.

### 2.8 Diagnostic and machine-identification registers

**Diagnostic register `$50F2C000`**: bit 0 must read 1. The ROM's power-on self-test reads
it at boot; a 0 diverts into the ROM's serial monitor with error code `$00020101`, and
the machine plays the error chord instead of the normal chime [4] (`$FFF04E84`).

**Machine-ID register `$5FFFFFFC`**: read-only, byte-wide; writes are ignored. The value
is `$A55A30xx` — upper half the `$A55A` signature, low half the model ID [1] Table 1-5
gives the low three bits (6100 `%000`, 7100 `%010`, 8100 `%011`): production hardware
reads `$3010` (all 6100s — the `$3011` the OS reports for a 6100 is substituted in
software after the AMIC revision probe), `$3012` (7100) and `$3013` (8100) [4]
(identity flow at ROM `$10070`, HWInit byte reads at `$FFF0303C`). A long-word
write/read-back probe of the register must **fail** (the ROM's identity code depends on
the long access failing while byte accesses work); whether the hardware fails it by
masking, lane-limiting or bus error is not established (§6) — the ROM reads bytes 2 and 3
individually [4].

## 3. Behaviour

### 3.1 The CPU interrupt line

Source bits 5–0 of the ICR are live level views. The single 601 `INT` line is driven by
bit 7 under the mode selected by bit 6 [4], [5]:

- **INTMODE = 1** (the mode the system ROM runs): any **change of the source picture —
  assertion or deassertion — sets bit 7 and raises INT**, which stays latched until
  acknowledged. A source that merely stays asserted does **not** re-latch after the
  acknowledgment; a source going away does latch, which is how the interrupt consumer
  learns to re-read the flags and find zero. (*Observed*: both halves are forced by the
  shipping ROM — a model that re-raises the line while a level merely holds livelocks
  the early 68k boot, which runs with a pending VIA1 timer interrupt at a raised
  interrupt priority level before the boot code disables VIA1 interrupt sources; and
  without deassertion latching, a stale posted interrupt level is never retracted [4].)
- **INTMODE = 0**: bit 7 and INT simply follow the OR of bits 5–0 (*inferred —
  unverified*: no system software runs in this mode).

Acknowledgment: a write with bit 7 set clears the latch (the nanokernel writes `$C0` =
INTMODE|CPUINT, the ROM's POST code and MkLinux write `$80`); toggling INTMODE also
clears the latch (*inferred — unverified*: only the clearing of the latch by writers is
observed) [4], [5].

### 3.2 Nanokernel dispatch and the 68k level model

The 601 has one interrupt line, so the ROM's nanokernel converts the ICR picture into
the classic 68k interrupt level on every external interrupt [4]:

1. Write `$C0` to `$50F2A000` (acknowledge the latch, keep INTMODE set), `eieio`, then
   read the same register for the source bits.
2. Map the low 6 bits through a 64-entry table to a 68k IPL: **NMI (bit 5) → 7; SCC (2)
   and DMA (4) → 4; Ethernet (3) → 3; VIA2 (1) → 2; VIA1 (0) → 1; none → 0** — the
   highest implied level wins.
3. Store the level into the 68k emulator's pending-interrupt level variable; the emulator
   takes the corresponding autovector when the 68k status register allows it.

The 68k handlers then read the second-level registers exactly as on a classic Macintosh.
The installed autovector levels are [4]:

| 68k level | Sources |
|---|---|
| 1 | pseudo-VIA1: 60.15 Hz tick, one-second, Cuda transport, timers |
| 2 | pseudo-VIA2: SCSI-A/B IRQ and DRQ, SWIM III chip IRQ, slot lines, VBL |
| 3 | MACE Ethernet chip interrupt (dispatched unconditionally — the only source) |
| 4 | SCC chip interrupt plus all DMA-completion flags |
| 5, 6 | unused |
| 7 | NMI (Cuda `CudaNmi*`) |

Within level 4, the ROM's dispatcher reads `$50F2A00A:$50F2A008` as one 16-bit value and
scans it **most-significant bit first**, so the dispatch priority is sound out > sound in
> floppy > Ethernet Tx > Ethernet Rx > SCC Tx A > Rx A > Tx B > Rx B, with the SCC chip
interrupt checked before any DMA flag. Because the CPU interrupt is change-based, the
level-4 handler loops until every flag is clear before returning [4]. The SCSI channels
have no DMA completion flag at all (§3.4); their interrupt arrives at level 2 from the
SCSI chip's own INT pin.

### 3.3 The DMA buffer window

All channels except the two SCSI channels address memory as offsets inside a single,
physically contiguous, 256 KB-aligned window whose base is programmed once into
`$50F31000/01` [4], [5]. The system software asks the nanokernel for the physical page
behind logical `$61000000` and writes it there; the nanokernel reserves **160 KB** for
the DMA buffer [4]. DMA references physical DRAM only — buffers must be physically
contiguous and locked, and the SCSI driver refuses DMA for any address at or above
`$40000000` (ROM/NuBus space) and double-buffers instead [4], [5]; SCSI alignment is 8
bytes, the window base 256 KB.

The window layout is fixed in hardware — channel "address" registers hold only small
offsets [4], [5]:

| Window offset | Contents |
|---|---|
| `+$00000` | Ethernet receive ring: 192 × 256-byte pages, 48 KB |
| `+$0C000` / `+$0E000` | Sound **input** buffers 0/1 (8 KB slots) |
| `+$10000` / `+$12000` | Sound **output** buffers 0/1 (8 KB slots) — also the floppy region: the floppy channel's effective address is `(window_base \| $10000) + programmed offset` |
| `+$14000` / `+$14800` | Ethernet transmit buffers, set 0/set 1 (2 KB each) |
| `+$20000` / `+$22000` / `+$24000` / `+$26000` | SCC ring buffers (8 KB each; see the A/B pairing caveat, §3.5) |

The floppy region overlays the sound-output/Ethernet-transmit area — software chooses
non-conflicting offsets. The total is exactly 160 KB. At cold boot the ROM programs the
window base to **physical 0** and plays its startup sound out of physical
`$10000/$12000` [4]; the Mac OS later re-points the window at the nanokernel's 160 KB
buffer [4].

### 3.4 SCSI channels

The two SCSI channels are unique among AMIC's channels: each has a full 32-bit physical
**buffer address register** (`$50F32000` A, `$50F32004` B), written one byte at a time,
MSB first, 8-byte aligned — and **no count register**. The 53C9x's own transfer counter
terminates the transfer; AMIC services the chip's DREQ while RUN is set [4], [5].

Control register bits (`$50F32008/09`) [4], [5]:

- bit 0 `RST`: soft reset — self-clears, zeroes the channel's internal offset and word
  count, preserves the direction bit and (on channel A) the speed field (*inferred —
  unverified* for the preserved bits).
- bit 1 `RUN`: start/stop.
- bits 3:2: **I/O bus speed** field, written once at boot from the measured bus speed
  (an inverse delay factor: the value decreases as the bus gets faster); readable and
  writable on AMIC-2 and later only — which is exactly how the ROM's revision probe
  detects AMIC-2 [4]. The precise encoding of the four values is not fully established
  (§6).
- bit 4 `FLUSH`: stop-read flush — after clearing RUN on a device-to-memory transfer,
  software sets FLUSH and polls until it self-clears; AMIC assembles four 16-bit words
  into aligned 64-bit memory writes, and FLUSH drains a partial assembly (*inferred —
  unverified*: the 16-to-64 assembly is a model derived from the data-path width; the
  self-clearing and the poll sequence are the ROM driver's [4]).
- bit 6 `DIR`: 1 = memory-to-device (SCSI write).
- **No IE/IF for SCSI**: there is no SCSI completion flag in `$50F2A008`; completion is
  detected through the 53C9x's own interrupt (device IFR bits 3/6). The ROM's SCSI HAL
  has no wait-for-DMA-completion call at all for this platform [4].

Writing the address register clears the channel's internal offset. The read-only current
address registers (`$50F32010/14`) read back base + offset (*inferred — unverified*). A
separate 16-bit byte-swapped pseudo-DMA data port for the SCSI chip sits at `$50F10100`
(*inferred — unverified*: an independent model decodes it; the ROM's handshaked pseudo-DMA
is exercised through the chip's registers and the DRQ bits instead) [4]. On the 6100 and
7100, channel B exists in the register file but is wired to nothing.

### 3.5 SCC channels

Four channels serve the SCC's two ports — transmit and receive for each. Register blocks
at `$50F32080 + n·$10` (n = 0–3, block layout: address long at `+$0`, count word at
`+$4`, control byte at `+$8`) [4], [5]:

- **Address** (`+$0–3`, byte-wide, MSB first): a small offset selecting the channel's
  fixed 8 KB ring inside the DMA window. On receive channels the low two bytes act as a
  **live transfer pointer** — the driver reads them back to learn progress [5].
- **Count** (`+$4` high — only 5 bits valid — `+$5` low): a 13-bit byte count, 8 KB
  maximum.
- **Control** (`+$8`): the common vocabulary of §2.5, with CONT in bit 2 and FROZEN
  (read-only) in bit 5. Bit 6 is set by the ROM's receive-arm sequence; its function
  (direction vs reload/circular control) is unresolved (§6).

Operation, as driven by the ROM's serial DMA hardware-abstraction layer [4] (construct
`$4099A390`, receive arm `$4099AA70`, transmit `$4099A930`), in agreement with MkLinux
[5]:

- **Reset**: write `RST`, poll until RUN reads 0. Reset re-points the address register to
  the channel's ring base.
- **Pause**: set `PAUSE`, poll until `FROZEN` reads 1 (a model must assert FROZEN or
  drivers spin); unpause by clearing PAUSE. Pausing brackets the driver's reads of the
  live receive pointer.
- **Receive**: runs in continuous mode — the driver writes count `$0400` and sets DIR, a
  mode bit (bit 6) and RUN; bytes land in the ring, the address pointer advances and wraps
  at `$2000` (8 KB), and the channel raises IF every `$400` bytes, with the count
  auto-reloading (*observed*: the ROM's receive interrupt handler never rewrites the
  count) [4]. Sub-`$400`-byte tails reach the client through a periodic flush that pauses
  the channel and samples the pointer [4].
- **Transmit**: the driver copies data into the ring, **adds** the length to the count
  register (low byte first, then high — the count accumulates bytes to send), sets RUN;
  the channel drains the ring to the SCC as the transmit request allows and raises IF at
  count exhaustion [4].
- **IF** is write-1-to-clear, and the driver's read-modify-write helper masks bit 7 to 0
  before writing back, so an unrelated bit never acknowledges a pending flag [4].

Each channel is paced by two request lines from the SCC: the W/REQ pin programmed as
receive request, and a separate transmit-request line from the host chip (a signal beyond
the stock 85C30 pins; *inferred* from the HAL's programming, which never selects
DTR/REQ request mode) [4]. DMA completion raises the channel's selector bit (0–3) in
`$50F2A008` at 68k level 4. The SCC chip interrupt is separate (ICR bit 2, dispatched at
level 4 before the DMA flags); the SCC's wait/request status is also exposed on VIA1 port
A bit 7.

**A/B naming caveat.** Apple's register equates name the blocks Tx A, Rx A, Tx B, Rx B in
address order (`$1080/$1090/$10A0/$10B0`), while the released MkLinux platform code
labels them the opposite way and notes its own interrupt wiring is flipped [5]. The safe
rule is to pair each register block with the ring the driver programs into its address
register rather than by fixed assumption (*observed*: the block at `$10A0`, driven for the
printer port by the Mac OS serial DMA layer, streams its transmit data from the ring at
window offset `+$20000`).

### 3.6 Floppy channel

The floppy channel behind SWIM III uses registers at `+$1060–1068`: a 16-bit buffer
offset (bits 15:8 at `+$1062`, 7:0 at `+$1063`), a 16-bit count (high at `+$1064`, low
at `+$1065`), and the control/status byte at `+$1068` [4], [5].

The channel is hard-wired into the window's second 64 KB: the effective memory address is
`(window_base \| $10000) + (offset & $FFFF)`, so the floppy buffer must sit there, and a
transfer that runs off the end of the 64 KB wraps inside it rather than walking into the
next region (*inferred*: established by model cross-checking; consistent with the
overlay of §3.3 and the ROM driver's use) [4], [5]. The address register's reset value is
`$15000` (*inferred — unverified*).

Transfers are byte-at-a-time on the SWIM III DMA request, incrementing the offset and
decrementing the 16-bit count; at count zero the channel sets IF (selector 6) and clears
RUN. Control bits: `RST` (clears offset and IF), `RUN`, `IE`, `DIR` (bit 6; 1 = write to
disk), `IF` (write-1-to-clear). The write-to-disk direction is used by the ROM's floppy
driver; its low-level behavior is assumed symmetric with the read path (§6) [4].

### 3.7 Ethernet channels

The Ethernet channels buffer through hardware-managed structures in the window rather
than raw address+count [4], [5]:

**Receive — a 192-page ring at window offset 0.** AMIC writes each incoming frame into
consecutive 256-byte pages; every frame starts on a fresh page boundary and is prefixed
by a 4-byte status header (MACE receive-frame status bytes, followed by 4 don't-care
bytes) — the driver reads word 0 = status bits 15:12 | byte count 11:0 and word 1 = runt
and collision counts [4]. The head pointer (`$50F32030`, page index 0–191) is advanced by
AMIC after each complete frame; software consumes frames and advances the tail
(`$50F32034`); the indices wrap at `$C0`, and head = tail means empty. If the head would
pass the tail, AMIC sets the overrun latch (control bit 6) and stops receiving; software
clears it by writing 1 (with IE and RUN restored). Control register `$50F32028`:
RST/RUN/IE/OVRRUN/IF; the enable value is `$8A` (IF|IE|RUN — the IF component clears any
stale flag). One receive interrupt (selector 4) is raised per completed frame. The status
header is written last — a non-zero status longword marks a complete frame, which is what
the driver polls; early silicon did not write the status of consumed pages back to zero,
and the ROM driver retains a workaround that clears it in software [4].

**Transmit — two fixed 2 KB buffers** at window `+$14000` (set 0) and `+$14800` (set 1).
The set-free status bits SET0 (bit 5) and SET1 (bit 6) in the Tx control register
`$50F31C20` read **1 when the buffer is free**. To send: copy the frame into the set's
buffer, write the byte count — **12-bit, high byte masked to `$0F`**, low byte first
(`+$1045/$1055`) then high (`+$1044/$1054`) — then write `$8A` (IF|IE|RUN) to the control
register. AMIC feeds the MACE's transmit FIFO, raises selector 5 on completion (the
finished SET flag identifies the buffer), and the count reads back zeroed [4], [5].
Reading the Tx control register clears IF per the MkLinux driver, while the ROM's
Ethernet handler writes the IF bit alone to clear it — both behaviors must be tolerated
(§6) [4], [5]. Both the ROM driver and MkLinux deliberately use only one buffer set,
because the MACE can buffer only one transmit status (a chip limitation) [4], [5].

The MACE chip's own interrupt is ICR bit 3, dispatched at 68k level 3, and is enabled
through the ICR's bit-3 write.

### 3.8 Sound DMA engine

With the output run bit set (`+$10` bit 0), the engine streams one 32-bit frame (16-bit
big-endian left | 16-bit right) per sample period from the active output buffer
(`window + $10000` or `+$12000`), advancing the phase counter; input mirrors this into
`+$0C000`/`+$0E000` under the `+$11` bit-7 run bit [4], [5]. When the phase wraps at the
programmed buffer size, the active buffer toggles and that buffer's completion flag
raises in `$50F14018` (out) or `$50F14014` (in). **If the previous flag for the incoming
buffer is still set at wrap time, the error flag (bit 5) raises instead** — the
under/overrun indication [4], [5]. The system may miss a final interrupt when stopping;
the driver clears the run bit as soon as its queue empties [5]. The phase counter must
advance and wrap whenever the engine runs — the ROM drains the startup sound by polling
it to zero, and a frozen counter hangs the boot [4].

The sample-rate field (bits 2:1 of `+$10`) selects 22 050 / 29 400 / 44 100 Hz
(dividers 16 / 12 / 8 of 45.1584 MHz ÷ 2 ÷ 64) [4], [5]. The AWACS bit clock is 256 ×
the sample rate [1] Table 3-16. Note the developer note's Table 2-3 prints the sound
oscillator as 44.1584 MHz; the board carries 45.1584 MHz, and only 45.1584 MHz divides
to exactly 44.1 kHz [3] sheet 11.

### 3.9 Video timing, VBL and monitor sense

AMIC generates the dot clock, horizontal and vertical sync, composite sync and blanking
for the built-in video, clocking pixel data fetched from DRAM through the Data Path
chips' video FIFOs (eight-beat bursts, arbitrated by the HMC just below DRAM refresh)
[1] pp. 16, 20, [3] sheets 5, 12. The Ariel II DAC at `$50F24000` receives AMIC's dot
clock and blanking and produces the analog outputs [1] p. 16, [3] sheet 12.

Monitor identification follows the Apple sense protocol on three lines driven and read
through the sense register (`$50F28002`): to sense, software tristates the lines (write 7,
or 0 on very early silicon), then drives one line low through the masks A = `%011`,
B = `%101`, C = `%110` and reads the other two, with about 500 µs of settle time between
steps (the ROM's delay loop polls the VIA 550 times per step) [4], [5]. A mode change is
programmed as: mode register, depth register, a delay, then the Ariel control register
[5]. VBL fires once per frame of the selected mode's refresh rate (60.15 / 66.67 / 75.00
/ 74.55 / 59.94 Hz depending on mode) into slot IFR bit 6 [1] Table 3-10.

### 3.10 Reset state and access side effects

At power-on or hardware reset (Cuda `RESET_IN` fan-out), the interrupt state is quiet:
VIA1 IFR and IER are 0, the VIA2 device IFR/IER are 0, the slot IER is 0, and the slot
IFR reads `$7F` (nothing asserted — active-low with the unused bits reading 1) [1]
Figure 2-2, [6]. The ICR powers up at 0, so INTMODE is 0 until the ROM selects the
change-latched mode. The video mode register's cold-boot state, as written by the ROM
before anything else touches video, is `$9F` (blanked, invalid code `$1F`) [4]
(`$FFF035CC`); its power-on value before that write is unverified.

A Mac OS restart does **not** reset AMIC: state survives, and the boot code compensates by
soft-resetting every channel itself on every restart (*observed*: the ROM writes `RST` to
all eight channel controls on each 68k boot; a hardware reset on restart would make that
redundant) [4].

Side-effect summary [4], [5]:

- **Write-1-to-clear**: channel `IF` bits; Ethernet `OVRRUN`; sound IF0/IF1/ERR; ICR bit
  7 (acknowledge); VIA1 IFR bits (VIA convention, bit 7 ignored); the VBL bit of the slot
  IFR (write `$40` deasserts).
- **Set/clear sense convention** (bit 7 of the written byte): VIA1 IER, slot IER
  (`$50F26012`), device IER (`$50F26013`).
- **Self-clearing**: SCSI `FLUSH` and the channel `RST` bits.
- **Read-only mirrors**: `$50F2A008/A` — flags clear only via the underlying channel IF.
- **No read side effects** anywhere in the interrupt path (all dispatchers re-read and
  loop); the exceptions elsewhere are the HMC serial port (reads post-increment the bit
  pointer) and the Ariel CLUT data register (accesses auto-increment the CLUT address).

## 4. Programming model

### 4.1 Cold-boot choreography

The ROM's PowerPC hardware-initialization code quiesces and configures AMIC before the
nanokernel ever enables interrupts [4] (HWInit: channel resets at `$FFF03580`, video off
at `$FFF035CC`, ICR mode write at `$FFF0506C`, VIA1 init at `$FFF0581C`):

1. Write `$01` (`RST`) to all eight DMA channel control registers (`$50F32028`,
   `$50F31C20`, `$50F32068`, the four SCC control bytes, `$50F32008`).
2. Write 0 to `$50F14010` and `$50F14011` — both sound DMA run bits off.
3. Write `$7F` to `$50F26012` and `$50F26013` — all interrupt enables cleared
   (set/clear form with bit 7 = 0).
4. Write `$9F` to `$50F28000` — video blanked.
5. Initialize pseudo-VIA1: `DDRA = $28`, `ORA = $20`, `DDRB = $B0`, `ORB = $B8`,
   `PCR = 0`, `ACR = $1C` (SR external-clock shift mode), `IER = $7F` (all interrupts
   disabled).
6. Write the DMA window base (physical 0 at cold boot).
7. At the moment machine interrupts are first enabled, write `$C0` to `$50F2A000` —
   select INTMODE 1 and clear any stale latch.

Later, the 68k-side boot code re-initializes the DMA engine for the operating system
[4]: soft-reset every channel again, clear both sound run bits, program the SCSI bus-speed
field from the measured bus clock, and re-point the DMA window base at the nanokernel's
160 KB buffer behind logical `$61000000`. At handoff or shutdown, the boot code disables
all interrupt sources: `$7F` to the VIA1 IER, `$7F` to the device IER, `RST` to the two
Ethernet channel controls, and `$FF` to the MACE's interrupt-mask register [4].

### 4.2 Interrupt service sequences

- **Level 1** (VIA1): the classic VIA1 handler — read IFR AND IER, dispatch by bit,
  acknowledge each serviced bit by writing it to the IFR.
- **Level 2** (VIA2): the ROM's RBV-style handler writes `$82` to `$50F26003`
  (set/clear form) to acknowledge the any-slot aggregate, then reads `$50F26002`,
  inverts the active-low slot bits, ANDs with the IER, and dispatches to the slot
  handlers — VBL included, since built-in video occupies a slot identity for the Slot
  Manager [4]. SCSI and floppy flags are level bits read from the same registers.
- **Level 3**: unconditional dispatch to the MACE Ethernet handler.
- **Level 4**: read `$50F2A00A:$50F2A008`; check the SCC chip interrupt first (reading
  the SCC's own registers to pick the port); then scan the DMA flags most-significant
  bit first; service; re-read; repeat until no flag remains (change-based interrupts
  mean a still-set flag would otherwise go unnoticed) [4].

The Cuda transport runs entirely through VIA1 at interrupt level 1: transactions poll the
TREQ bit on port B bit 3 and the IFR shift-register bit rather than using interrupts, and
the boot code even uses dummy IER reads as bus-timed delays [4].

### 4.3 SCSI driver sequences

The ROM's SCSI hardware-abstraction layer drives the channels as [4], [5]:

- **Start**: write the 32-bit buffer address MSB-first; set `DIR` (bit 6) for a SCSI
  write; set `RUN` (bit 1) — carefully with `bset`/`bclr` so the shared bus-speed bits
  are not disturbed.
- **Pseudo-DMA fallback**: for transfers below the 0x200-byte minimum useful DMA size
  (8-byte alignment required), poll the DRQ bit in `$50F26003` and move data through the
  chip's registers.
- **Read termination**: set `FLUSH`, poll until it self-clears (residual bytes written
  to memory), clear `RUN`, poll until `RUN` reads 0.
- **Stop**: clear `RUN`.
- **Completion**: none through AMIC — the 53C9x interrupt (device IFR bits 3/6) is the
  only completion signal.

### 4.4 Serial DMA sequences

See §3.5 for the channel semantics. The ROM's serial HAL construct soft-resets both
channels of a port, adopts the ring bases by reading the address registers back, and
installs its completion handlers into the level-4 dispatch slots [4] (`$4099A390`).
Receive arms with count `$0400` then control bits DIR, mode bit 2 and RUN; transmit
copies into the ring, adds the length to the count, and sets RUN; the receive interrupt
handler pauses the channel around its pointer sample, computes bytes received with the
`$2000` wrap fix-up, acknowledges IF, and delivers through deferred tasks [4].

### 4.5 Floppy driver sequence

The ROM's floppy driver sets up a transfer as: write the count (low byte, then high);
write the buffer offset (low byte, then high); read the control register and set or
clear `DIR` (1 = write to disk); OR in `RUN`; then arm the SWIM III itself to start the
action. Interrupt enable is ORed in separately. The completion handler clears IE and
RUN with an AND-immediate and then writes the `IF` bit to acknowledge; the still-set IF
would otherwise keep level 4 asserted [4].

### 4.6 Ethernet driver sequences

- **Receive enable**: write `$8A` to `$50F32028` (clears a stale IF, enables interrupts,
  runs).
- **Frame processing**: poll the page status (non-zero = complete), read the status
  words, process, advance the tail pointer; on overrun, write `$4A` (OVRRUN|IE|RUN) to
  clear and restart [4].
- **Transmit**: copy the frame into the free set's buffer, write the 12-bit count low
  byte then high, write `$8A` to `$50F31C20`; on completion, acknowledge IF (write it, or
  read the register — see §3.7) [4], [5].

### 4.7 Sound: the boot beep

The ROM plays its startup sound fully polled, before the operating system exists [4]
(`$FFF0551C–$FFF056C8`): it locates its sample table by scanning for the signature
`"joebritt"` below the 68k ROM top (entry 0 = the boot chime, 16-bit stereo; entry 1 =
the error chord, 8-bit, expanded ×4 and upsampled ×2; each entry carries the value to
write to `$50F14010`), programs the codec through the control registers (attenuation
from Cuda parameter RAM), writes buffer size `$0400`, clears the completion flags with
`$E0`, starts the engine, and then ping-pongs: acknowledge bit 6 (`$40`) for the
`+$10000` buffer or bit 7 (`$80`) for `+$12000`, refill that buffer, repeat. When
draining, it polls the phase counter (`+$0C` bits 1:0, `+$0D`, `+$0E` bits 6:5) for the
wrap to zero. Stop = clear the run bit in `$50F14010`, then write `$C0` to clear the
flags. Machine-state interrupts are managed around the buffer fills. The window base is
physical 0 at this point, so the buffers are at physical `$10000/$12000` [4].

After boot, sound is programmed exclusively by native driver code: the 68k ROM's
per-machine sound-control vector table for this platform is entirely no-op stubs [4].

## 5. Quirks & errata

1. **Change-based interrupts.** With INTMODE 1 the line latches on any *change* of the
   source picture — assertion or deassertion — and nothing else. A level-style model
   that re-raises while a source holds livelocks the early boot; an edge model that
   misses deassertions strands a stale posted level. The level-4 handler's loop-until-
   clear and the level-2 dispatcher's "no source" path both exist because of this [4].
2. **`nop` after write.** Drivers assume a write completes before the next access;
   read-after-write must observe the new value [4].
3. **SCSI bus-speed bits** (bits 3:2 of `$50F32008`) must be writable and read back —
   the ROM's AMIC-2 revision probe and the machine identity the OS reports depend on
   it [4].
4. **Machine-ID probe order**: a long-word write/read-back of `$5FFFFFFC` must fail while
   byte reads work; byte writes are ignored [4].
5. **SCSI `FLUSH` self-clears** (software polls it) and **Ethernet `OVRRUN` is
   write-1-to-clear** — exact bit behavior, or drivers spin [4].
6. **Ethernet receive status is written last**: the page status must become non-zero only
   when the frame is complete — the driver polls it as a "frame complete" flag [4].
7. **Ethernet Tx SET0/SET1 read 1 when free** — inverted "empty" semantics; and reading
   the Tx control register may itself clear IF (one driver depends on each behavior)
   [4], [5].
8. **Device IFR writes ignore bit 7** — the SCSI driver's `$88` "clear" write must be a
   harmless no-op for the level bits; enables use the bit-7 sense convention [4].
9. **DMA never touches addresses ≥ `$40000000`** — the SCSI driver double-buffers instead
   [4].
10. **Warm restart does not reset AMIC** — channel state survives a 68k restart and is
    cleaned up by the ROM's soft-reset writes [4].
11. **`$50F2C000` bit 0 must read 1** — a 0 diverts every boot into the ROM serial
    monitor / error chord [4].
12. **The sound phase counter must advance and wrap** while the engine runs — the ROM
    polls it to zero when draining the startup sound; a frozen counter hangs the boot
    [4].
13. **Slot IFR reads active-low with unused bits high** (reset `$7F`); handlers invert
    and AND with the IER. A model returning active-high sticks every slot interrupt on
    [1] Figure 2-2, [4].
14. **Sound under/overrun**: if a buffer's completion flag is still set when that buffer
    completes again, raise ERR (bit 5) instead — error recovery depends on seeing it
    [4], [5].
15. **The VIA2 bank's low-five-bit mirror** (§2.3) is mandatory: the level-2 dispatcher's
    classic-VIA-stride addresses must alias to the same registers the compact-address
    drivers use [4].
16. **Early-silicon errata, not to be modeled on production parts**: the
    "returns `$FF` on first read" register bug (EVT1), the missing Ethernet-receive
    status clearing on early silicon, and the pre-AMIC-2 non-writable speed bits and
    reversed slot wiring [4].
17. **The developer note's Figure 2-2 access annotations are partly wrong**: it draws
    the VIA1 IFR as read-only and the device IFR as read-only; ROM code writes both
    (write-1-to-clear and level-clear respectively) [4].

## 6. Open questions

1. **SCC control bit 6**: named "direction" in one recovered analysis and "reload" in
   another; the receive-arm path sets it. Is it a direction select, a reload/circular
   control, or something else? Unresolved.
2. **SCC channel-to-ring pairing**: Apple's block names (Tx A/Rx A/Tx B/Rx B) and the
   released MkLinux labels disagree (§3.5); the observed pairing is per-driver. A
   definitive physical assignment needs an end-to-end serial-DMA exercise on hardware.
3. **SCSI bus-speed field encoding** (bits 3:2 of `$50F32008`): written once at boot
   from the measured bus clock as an "inverse delay factor"; the exact four-value
   encoding (bus frequency ranges to values) is not fully pinned down.
4. **DMA bus-error registers** (`$50F32100/2`): two 16-bit registers exist; whether a
   DMA access to undecoded or ROM space latches there (per-channel bus-error behavior
   exists on the AV machines' PSC) is unknown. No driver uses them.
5. **Pseudo-VIA2 auxiliary registers**: `+$0` (data), `+$1` (RAM size) and `+$5` (speed)
   are defined for the Sonora chip; nothing on this platform reads or writes them.
   `+$10` (monitor parameters) layout unknown.
6. **Pseudo-VIA1 6522 fidelity**: timer reload/latch edge cases and shift-register mode-0
   quirks are unwired on this platform; exact MOS-6522 equivalence is unverified, as is
   the timer input rate (783.36 kHz is inferred from classic-Mac compatibility).
7. **ICR acknowledgment decode**: all observed writers write `$C0` or `$80`; the exact
   required bits and the effect of toggling INTMODE (latch clear is inferred) are
   unverified. INTMODE 0 behavior is inferred.
8. **Ethernet receive head advance**: per completed frame (implemented, and all that
   drivers depend on) vs also on intermediate pages of a long frame — unverified.
9. **Machine-ID 32-bit access**: the mechanism by which a long-word probe fails (byte
   lanes only? masking?) is unknown; only the ROM's requirement that it fail is
   established. The power-on value of the video mode register (before the ROM's `$9F`
   write) is likewise unknown.
10. **Floppy channel reset value** `$15000`, the SCSI current-address registers, the
    SCSI pseudo-DMA port at `$50F10100`, the sound buffer-size register width (11 vs 10
    bits), and the video beam/test registers: all established by model cross-checking
    only, unverified against hardware.
11. **Ethernet DMA arbitration position** — absent from the developer note's Table 2-4.
12. **6100 slot-interrupt bit 5**: the developer note italicizes SLT3 as absent on the
    6100/60, but the machine's single expansion slot (NuBus adapter, slot `$E`) maps to
    bit 5; the italics likely refer to the missing on-board PDS-video function, not the
    interrupt line. Modeled as present.
13. **AWACS codec identification values** (manufacturer/revision nibbles the codec status
    reports): read by drivers, but the required constants are not established.
14. **Floppy write-to-disk direction** low-level behavior: the ROM driver uses it; its
    hardware behavior is assumed symmetric with the (exercised) read path.

## References

1. Apple Computer, Inc., *Power Macintosh Computers* (Developer Note, March 1994),
   Developer Press. Covers the Power Macintosh 6100/60, 6100/60AV, 7100/66, 7100/66AV,
   8100/80 and 8100/80AV: AMIC description pp. 15–16; CPU-bus arbitration Table 2-4
   p. 20; physical memory Table 2-5 p. 22; interrupt emulation pp. 22–23 and Figure 2-2;
   fixed clocks Table 2-3 p. 20; machine identification Table 1-5 p. 10; built-in video
   Tables 3-8/3-10 pp. 36, 39; sound subsystem pp. 46–48 and Figures 3-12/3-13/3-14.
2. Apple Computer, Inc., *Enhanced Power Macintosh Computers* (Developer Note), Developer
   Press. Covers the Power Macintosh 6100/66, 7100/80, 8100/100 and 8100/110; no AMIC
   changes are listed for the Enhanced models.
3. Apple Computer, Inc., Power Macintosh 8100/110 main-logic-board schematics, drawing
   051-0333 (31 sheets). AMIC and its pin wiring on sheet 12; Cuda wiring sheet 16;
   SWIM III select decoding sheet 20; clock tree sheet 11; HMC and data-path sheets
   4–5; slot interrupt wiring sheet 22.
4. Power Macintosh 6100/7100/8100 ROM, version `$077D` (image checksum `$9FEB69B3`,
   March 1994) — full disassembly and declaration-data analysis. Cited addresses:
   nanokernel external-interrupt handler `$FFF10780` and its flag-to-level table
   `$FFF10740`; hardware init (channel resets `$FFF03580`, video off `$FFF035CC`,
   ICR mode write `$FFF0506C`, VIA1 init `$FFF0581C`); AMIC-2 revision probe
   `$FFF032B8`; diagnostic-register check `$FFF04E84`; machine-ID byte reads
   `$FFF0303C`; boot beep `$FFF0551C–$FFF056C8`; diagnostic LED module `$FFF30180`;
   68k level-2 dispatcher `$408104A0` and level-4 dispatch loop; serial DMA HAL
   `$4099A390/$4099A930/$4099AA70`; SCSI, floppy, Ethernet and sound driver sequences
   and the platform decoder/declaration records in the ROM's data.
5. Apple Computer, Inc. / OSF Research Institute, MkLinux microkernel sources (GPL
   release), POWERMAC platform files — `powermac_pdm.h`, `interrupt_pdm.c`,
   `scsi_amic.c`, `scc_amic.c`, `awacs_pdm.c`/`awacs_pdm_hw.h`, `enet_dma.h`,
   `if_mace.c`, `video_pdm.c`. An independently developed driver set for the same
   registers, written against the raw hardware.
6. Rockwell International, *R6522 Versatile Interface Adapter (VIA)* datasheet —
   baseline 6522 register semantics implemented by the pseudo-VIA1.
