# Grand Central — the TNT I/O controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, placement behind Bandit, division of labor, clocking
2. [Register file](#2-register-file) — the 128 KB window and its decode law, the interrupt block, the eleven DBDMA channels, the
   sixteen device apertures (53C94, MACE, both SCC ports, AWACS, SWIM3, VIA1, MESH, the Ethernet address PROM, the board
   registers and the two NVRAM apertures)
3. [Behaviour](#3-behaviour) — endianness, the 32 interrupt sources, the interrupt line and its two acknowledge modes, the
   NanoKernel dispatch into 68k interrupt levels, the VIA cascade, DBDMA channel execution, timing
4. [Programming model](#4-programming-model) — device-tree enumeration, the firmware bring-up sequence, the MkLinux model,
   the Mac OS interrupt tree, the firmware boot beep as a worked example
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**Grand Central** (Apple part **343S1125**) is the custom I/O controller of the PCI Power Macintosh platform: "an interface
between the standard Macintosh I/O devices and the PCI bus" [1] p. 18, [2] p. 11. It is the point where the whole legacy
Macintosh peripheral set — serial, floppy, ADB, Ethernet, sound, and both SCSI buses — meets the PCI bus, and it is the
machine's central interrupt collector: the functions Apple lists for it are support for the Cuda IC (the VIA registers),
central system interrupt collection, support for descriptor-based DMA for I/O devices, and the floppy disk interface
(SWIM III) [1] p. 18, [2] p. 11. Its DMA controller "provides DBDMA support for all I/O transfers, including transfers
through its internal I/O controllers as well as transfers through the Curio IC for other I/O devices" [2] p. 11; on the
7500/8500 the same sentence reads "supports DMA I/O transfers through that IC's internal I/O devices and through the Curio
IC" [1] p. 18.

Grand Central is thus simultaneously:

- a **PCI device** — "fully PCI compliant and therefore can re-position its memory space response to anywhere in PCI
  memory space" [3] §4.2.1 p. 12, addressed by firmware through configuration cycles;
- an **interrupt controller** — 32 interrupt sources in one four-register block, feeding the PowerPC external interrupt
  line (§3.2–§3.4);
- a **DBDMA controller** — eleven identical descriptor-based DMA channels on the TNT machines (§2.4, §3.6); and
- a **bus host for the legacy cells** — an internal generic bus ("GBus") with chip selects that reaches the nonvolatile
  RAM, the Ethernet address PROM, the board registers and, on the AV machines, the Sixty6 video-output IC [1] p. 18;
  [3] §2.2.2 p. 6, §4.6.1 p. 13.

The Open Firmware device tree publishes it as node `bandit/gc` with `model = "AAPL,343S1125"` and
`device_type = "dbdma"` [5]. Apple's own register-level reference for the part — the "Grand Central ERS" — is named by the
Network Server developer notes ("For detail on register function refer to the Grand Central ERS" [3] §4.6.1 p. 13) but was
never published; everything below rests on what Apple's firmware, Apple's developer notes, and shipping operating systems
actually do with the chip.

### 1.2 Machines that carry it

| Machine | Apple platform codename | Bandits | Internal SCSI (MESH) | On-board video | Notes |
|---|---|---|---|---|---|
| Power Macintosh 7500 | TNT | one | yes | Control/Chaos | see [pm7500.md](pm7500.md) |
| Power Macintosh 8500 | TNT | two | yes | Control/Chaos + AV (Sixty6, Plan B) | see [pm8500.md](pm8500.md) |
| Power Macintosh 9500 | TNT | two | yes | none (PCI video card required) | see [pm9500.md](pm9500.md) |
| Power Macintosh 7200 | Catalyst | one | **no** | Platinum | cheaper platform; same Grand Central |

All four machines run the same 4 MB mask ROM and the same Open Firmware 1.0.5 [5], and the Grand Central instance is
address-identical on all of them. The single inside-Grand-Central difference that the ROM itself knows about is the
internal SCSI bus: the Catalyst/7200 build has no MESH, so its 68k decoder table carries no entry for the `+$18000`
aperture at all, while the TNT build points it at the MESH controller (§2.5) [5]. The 1997 machines of the same lineage
(7300, 7600, 8600, 9600) carry the same Grand Central and reuse these identities — the 8600 reports itself to software
as an 8500 and the 9600 as a 9500 [5]. The Apple Network Server 500/700 is a 9500-derived design that also "will use
Grand Central to provide non-critical I/O" with "the internal Grand Central interrupt mapping... unchanged" [3] §2.2 p. 5,
§4.2 p. 16 — its external interrupt wiring differs (§3.2), which is why its ROM and the 9500's are not interchangeable.

### 1.3 Placement

Grand Central hangs off the first PCI bus, behind the Bandit host bridge (see [bandit.md](bandit.md)). Bandit owns a 16 MB
window at `$F2000000` and the next 16 MB is its I/O-side window; Grand Central's response is positioned by firmware at
`$F3000000`, the base of that window [5], [7]. Two things about this address are worth stating precisely:

1. **It is a convention, not a decode constant of the chip.** "Grand Central is fully PCI compliant and therefore can
   re-position its memory space response to anywhere in PCI memory space. The addresses defined... are defined for
   convention only (defined by the current OpenFirmware and Expansion Manager code for the TNT ROM releases)" [3]
   §4.2.1 p. 12. Every shipping piece of Apple software treats `$F3000000` as a constant anyway — the 68k ROM's decoder
   table hard-codes it [5], the kernel's configuration names it as the interrupt-controller address [5], and MkLinux
   hard-codes it as `GRAND_CENTRAL_BASE` [7] — but the chip itself only requires that firmware program its base before
   anything touches it.
2. **The window is 128 KB.** "To support backwards compatibility for VIA register offsets, 128KBytes of address space is
   consumed" [3] §4.2.1 p. 12. The size is not an accident of decode: the VIA's compatibility register file is strided
   across 8 KB of it (§2.5), and MkLinux carries `GRAND_CENTRAL_SIZE` as `$20000` [7].

Within the PCI arbitration of the machine, Grand Central sits at the top: a separate gate array "provides the priorities
for bus arbitration as follows: 1. Grand Central IC (I/O device controller; highest priority) 2. PCI slots and Bandit
master, in round-robin sequence: that is, each in turn, with equal priority" [1] p. 18, [2] p. 11. When a DBDMA channel is
running, it is Grand Central that masters the PCI bus on the device's behalf.

### 1.4 Division of labor

Grand Central is a controller of controllers, and most of the "devices" in its window are separate ICs reached through its
apertures. The split, per the developer notes and the device tree, is:

| Function | Owner |
|---|---|
| Interrupt collection, masking, 68k IPL delivery | Grand Central (§3) |
| Descriptor-based DMA for every device | Grand Central — eleven channels (§2.4) |
| Floppy control (SWIM III cell) | **inside** Grand Central — "an extension of the SWIM II design used in earlier Macintosh models... supports DMA data transfers and does not require disabling of interrupts during floppy disk accesses" [1] p. 18, [2] p. 11 |
| VIA1 register file (Cuda transport) | an "interface for" the Cuda microcontroller, hosted in Grand Central's window [1] p. 18; the 6522 core is driven through the `+$16000` aperture (§2.5) |
| External SCSI (53C94), Ethernet (MACE), serial (ESCC) | the **Curio** IC — "a multipurpose custom IC that contains a Media Access Controller for Ethernet (MACE), a SCSI controller, and a Serial Communications Controller (SCC)", with "8-byte FIFO buffers for both transmit and receive data streams" in its SCC section, and DMA "between its I/O ports and the computer's main memory" [1] p. 19, [2] p. 12 — reached entirely through Grand Central apertures and Grand Central DMA channels |
| Internal SCSI (MESH) | the **MESH** IC — "controls the SCSI bus to the internal SCSI devices", faster than the external bus "because this bus does not have to drive a long external bus": up to 10 MB/s internal vs 5 MB/s external [1] p. 19 |
| Sound codec (AWACS) | the **AWAC** IC, "a waveform amplifier with a 16-bit digital sound encoder and decoder (codec)" [1] p. 19, reached through the `+$14000` aperture and channels 8/9 |
| NVRAM, Ethernet address PROM, board registers, Sixty6 | external parts on Grand Central's 16-bit generic bus [1] p. 18; [3] §2.2.2 p. 6 |
| ADB, PRAM, real-time clock, soft power, system reset | the **Cuda** microcontroller (a custom Motorola MC68HC05) [1] p. 19, reached only through the VIA1 aperture and interrupt 18 |
| Memory, ROM, L2 cache | Hammerhead — see [hammerhead.md](hammerhead.md) |
| On-board video | Chaos/Control on their own bus, not through Grand Central (their interrupt, however, is — §3.2) |

Note what this means for the "non-critical I/O" phrase Apple uses for the part [3] §2.2 p. 5: on this platform Grand
Central's own interrupt output is the *only* path by which any device — including the built-in video controller, which
lives on a different bus — reaches the processor. "Central system interrupt collection" [1] p. 18 is meant literally.

### 1.5 Clocking

Grand Central "is connected to the PCI bus and uses the 33 MHz PCI bus clock" [1] p. 18, [2] p. 11. The PCI bus and the
processor bus run asynchronously — 33 MHz PCI against a 50 MHz processor bus on the 7500/8500 [1] p. 17 and 40/44 MHz
(one-third of the 120/132 MHz processor clocks) on the 9500 [2] p. 11 — with Bandit bridging the two domains [1] p. 18.
Software never sees the 33 MHz clock directly, but it does see the derived and unrelated clocks of the attached cells:

| Clock | Value | Where it surfaces |
|---|---|---|
| PCI bus | 33 MHz | Grand Central itself [1] p. 18 |
| 53C94 (external SCSI) | 25 MHz, published by the device tree as `clock-frequency = 25000000` | the external SCSI bus timing [5] |
| MESH (internal SCSI) | 50 MHz assumed by OS drivers | the internal SCSI bus timing [8] |
| SWIM3 timer | a 1 MHz countdown | floppy step-rate timing [8] |
| VIA1 timer 1 | 60.15 Hz tick observed behaviorally (`Ticks`-driving) | the Mac OS time base (§3.7) |

That the two SCSI cells run at different clocks is explicit in the tree: the `53c94` node carries the 25 MHz literal, the
MESH driver assumes 50 MHz [5], [8].

## 2. Register file

### 2.1 The window and its decode law

Grand Central decodes 128 KB of memory space, and the internal structure of that window is documented by Apple's Network
Server notes, which describe the same "TNT Grand Central Device Register Mapping" [3] §4.2.1 p. 12. The decode is a
three-way split on the offset bits:

```
0_0000_0000_RRRR_RRRR   DMA controller register space (RRRR selects a controller register)
0_1000_cccc_RRRR_RRRR   DMA channel register space (cccc selects a channel, RRRR a channel register)
1_dddd_RRRR_RRRR_rrrr   device register space (dddd selects a device, RRRR a register,
                         rrrr a sub-register — used only by the SCC compatibility port)
```

[3] §4.6.1 pp. 13–14. In plain terms: offsets below `+$8000` are controller-level registers (in practice, the interrupt
block of §2.2), `+$8000 + n×$100` is DBDMA channel *n*, and `+$10000 + d×$1000` is device *d*. Three further rules govern
the whole window:

- "Grand Central's registers live on 4 or 16 byte strides and can be accessed as 32-bit words modulo the SCC
  compatibility port" [3] §4.6.1 p. 13 — 4-byte centres for the true 32-bit registers, 16-byte (`$10`) centres for the
  byte-wide cells, with the legacy SCC aperture as the one exception (§2.5).
- "Writes to unmapped portions of the 128K space will have no affect. Reads of unmapped locations will return zeros"
  [3] §4.6.1 p. 13.
- Everything in the window is **little-endian** (§3.1).

The complete observed map, cross-attested by the firmware's device tree, the 68k decoder table and MkLinux's constants [5],
[7]:

| Offset | Size | Contents | Evidence |
|---|---|---|---|
| `+$0020`–`+$002C` | 4 × long | **Interrupt block**: Events, Mask, Clear, Levels (§2.2) | [5], [6], [7] |
| `+$002D`–`+$7FFF` | — | unmapped in the controller space | nothing touches it |
| `+$8000`–`+$8AFF` | 11 × $100 | **DBDMA channels 0–10** (§2.4) | [5], [7] |
| `+$10000` | $100 | device 0 — **53C94** external SCSI (Curio) | [5], [7] |
| `+$11000` | — | device 1 — **MACE** Ethernet (Curio) | [5], [7] |
| `+$12000` | — | device 2 — **SCC "compatibility" port** (68k serial) | [3] p. 14, [5] |
| `+$13000` / `+$13020` | — | device 3 — **SCC "MacRISC" port** (ESCC), channel B at `+0`, channel A at `+$20` | [3] p. 14, [5] |
| `+$14000` | — | device 4 — **AWACS** audio | [5], [7] |
| `+$15000` | — | device 5 — **SWIM3** floppy | [5], [7] |
| `+$16000`–`+$17FFF` | $2000 | devices 6–7 — **VIA1 / Cuda** (§2.5) | [5] |
| `+$18000` | — | device 8 — **MESH** internal SCSI (TNT only; absent on the 7200) | [5], [7] |
| `+$19000` | — | device 9 / GBus — **Ethernet address PROM** (§2.5) | [3] pp. 14–15, [5] |
| `+$1A000` | — | GBus device 1 — **Board Register 1 / BoxID** (§2.5) | [3] p. 15, [5] |
| `+$1B000` | — | GBus device 2 — **RaDACal colormap bank** on TNT (listed "Reserved" for the Network Server, which has no Control video) | [7], [3] p. 14 |
| `+$1C000` | — | GBus device 3 — **Sixty6** video-out on the AV machines; the LCD interface on the Network Server | [5], [3] pp. 14–15 |
| `+$1D000` | — | GBus device 4 — **NVRAM bank-select port** (§2.5) | [3] p. 14, [5], [9] |
| `+$1E000` | — | GBus device 5 — **Board Register 2** ("the same locations as in the 9500") | [3] pp. 14–15 |
| `+$1F000` | — | GBus device 6 — **NVRAM data window** (§2.5) | [3] p. 14, [5], [9] |

The device-number and channel-number assignments above are Apple's own: the Network Server notes publish both tables
explicitly — channels `$00`–`$09` as SCSI, Floppy, Ethernet Transmit, Ethernet Receive, SCC A TX, SCC A RX, SCC B TX,
SCC B RX, Audio Out, Audio In, and devices `$00`–`$0F` as listed [3] §4.6.1 p. 14. The TNT machines add the eleventh channel
(`$0A`, MESH) and the MESH device select (`$08`), which the Network Server tables omit because that machine has no MESH —
the TNT firmware's device tree is the authority for both additions [5] (see §5 on the ten-vs-eleven discrepancy).

### 2.2 The interrupt block

Grand Central's own control of the machine lives in four contiguous 32-bit registers at the head of the controller
register space. The offsets are attested three times over — in Apple's prototype equates (`+$20/24/28/2C`, which survive
unchanged from the 1994 bring-up tree [6]), in the shipped kernel's interrupt handler, which reads and writes them
byte-reversed at exactly these offsets [5], and in MkLinux, whose `GRAND_CENTRAL_BASE`-relative `GC_INTERRUPT_EVENTS`,
`..._MASK`, `..._CLEAR` and `..._LEVELS` constants are `$20/$24/$28/$2C` [7]:

| Offset | Register | Access | Function |
|---|---|---|---|
| `+$20` | InterruptEvents | read / write-1-to-clear | edge-latched event flags (§3.3) |
| `+$24` | InterruptMask | read / write | 1 = source enabled |
| `+$28` | InterruptClear | write | write-1-to-clear pending sources; bit 31 is the mode bit (§3.3) |
| `+$2C` | InterruptLevels | read | live, ungated level state of every source |

A source's *interrupt number* is its bit position in these registers under **little-endian** numbering — bit 0 is the
least significant bit of the little-endian longword. This is the number the firmware publishes in each device node's
`AAPL,interrupts` property and the number every operating system uses [5]. Because the processor bus is big-endian,
PowerPC software reaches all four registers with byte-reversed accesses (`lwbrx`/`stwbrx`), and a bit test in kernel code
performed on the byte-reversed word sees source *N* at PowerPC bit `31−N` (§3.4) [5].

Bit 31 of the Mask register is not a source: Apple's prototype equates name it `ifMode1Clear` [6], and the shipped kernel
writes `$80000000` to InterruptClear on every single interrupt [5] — it is the mode/acknowledge bit whose semantics are
§3.3's subject.

### 2.3 The DBDMA channel register file

Each of the eleven channels occupies a 256-byte slot at `+$8000 + n×$100`, and all channel registers are 32-bit
little-endian. The published architecture (CHRP I/O Device Reference, chapter 15) defines the channel registers as [4]
§15.4.1 p. 173, Table 321:

| Offset | Register | Required | Function |
|---|---|---|---|
| `+$00` | ChannelControl | required | the write port for the status bits (mask/value, below) |
| `+$04` | ChannelStatus | required | the channel's state (read-only; writes ignored) |
| `+$08` | reserved | — | — |
| `+$0C` | CommandPtrLo | required | physical address of the next 16-byte-aligned command descriptor |
| `+$10` | InterruptSelect | optional | condition select for the `i` modifier |
| `+$14` | BranchSelect | optional | condition select for the `b` modifier |
| `+$18` | WaitSelect | optional | condition select for the `w` modifier |

Grand Central "implements only the base DB DMA architecture" [3] §2.2.1 p. 5, and Apple's notes enumerate what is *not*
there: no 64-bit addressing (no CommandPtrHi/DataPtrHi), no memory-to-memory transfers (no OffsetHi/OffsetLo), a single
transfer mode ("memory addresses always increment, block transfer size is always system defined, transfers are always
coherent"), and no special event/management channels [3] §2.2.1 p. 5. Whether the three condition-select registers are
implemented at all, or hardwired so that conditions always read true, is not attested on either side (§6) — no driver in
the shipped-software corpus programs them [7], [8].

**ChannelControl (`+$00`)** is written as one 32-bit word whose upper half is a mask and lower half a value: "Bits in the
lower half of the ChannelControl register are written only if the corresponding bits in ChannelControl.mask are set" [4]
§15.4.2 p. 174. This is why every driver writes paired constants such as `$80008000` (set Run) and `$80000000` (clear
Run). The same convention appears verbatim in Apple's prototype interface header [6] and in the shipped ROM's own driver
literal pools [5].

**ChannelStatus (`+$04`)** reports the channel's state [4] §15.4.3 p. 174, Table 326:

| Bit | Name | Meaning |
|---|---|---|
| 15 | run | set by software to start execution; cleared to abort ("data transfers are terminated, status is returned, and an interrupt is generated if requested") |
| 14 | pause | set by software to suspend; hardware clears active and waits for pause to be reset |
| 13 | flush | set by software to force a partial input to memory; hardware writes back status, then clears flush |
| 12 | wake | set by software to make an idle channel "refetch the command pointed to by CommandPtr and continue processing"; hardware resets it after each command fetch |
| 11 | dead | set by hardware on a catastrophic event "such as a bus or device error"; the channel stops, writes status back, and interrupts unconditionally; cleared only when software clears run (§3.6) |
| 10 | active | set by hardware while the channel executes; cleared on stop, pause, dead, or software clearing run |
| 9 | reserved | — |
| 8 | bt | set by hardware at command completion to record that a branch was taken |
| 7–0 | s7–s0 | up to eight general-purpose status bits, "channel-specific", written through ChannelControl and by the attached device |

The low byte is the *device* status byte: each attached cell drives its own meaning onto s7–s0, and the command word's
`i`/`b`/`w` modifiers test it (§3.6). Apple's 1993 pre-release interface header assigned a different set of names to the
`$0400`–`$2000` range (one bit position off); the CHRP names above match the shipped silicon, whose firmware poll loops
only terminate under them (§5) [4], [6], [5].

**CommandPtrLo (`+$0C`)** "specifies the address of the next command entry to be fetched", must be 16-byte aligned ("the
four least-significant bits... must always be written with zeros"), and "writes to the CommandPtrLo register are ignored
unless the ChannelStatus.run and ChannelStatus.active bits are both 0" [4] §15.4.4 p. 176 — with the caveat of §5 on what
the shipped ROM actually gets away with.

### 2.4 The command descriptors (memory-resident)

The per-channel registers point into a memory-resident command list. Each descriptor is 16 bytes, little-endian, and must
be 16-byte aligned [4] requirement 15-2, §15.7.1 p. 182:

| Word | Bits | Field |
|---|---|---|
| 3 | 31–28 | cmd (OUTPUT_MORE 0, OUTPUT_LAST 1, INPUT_MORE 2, INPUT_LAST 3, STORE_QUAD 4, LOAD_QUAD 5, NOP 6, STOP 7) |
| | 26–24 | key (KEY_STREAM0–3 = 0–3, KEY_REGS 5, KEY_SYSTEM 6, KEY_DEVICE 7) |
| | 21–20 | i — interrupt modifier (0 never, 1 if condition set, 2 if clear, 3 always) |
| | 19–18 | b — branch modifier (same four values) |
| | 17–16 | w — wait modifier (same four values) |
| | 15–0 | reqCount |
| 2 | 31–0 | address (physical data address) |
| 1 | 31–0 | cmdDep (command-dependent: branch target for a branching NOP, immediate data for LOAD/STORE_QUAD) |
| 0 | 31–16 | xferStatus — written back by the channel |
| | 15–0 | resCount — written back by the channel, the residual byte count |

[4] §15.7.1 p. 182, Tables 333–342. There is no separate JUMP or WAIT opcode: a jump is a NOP with `b = 3` and the target
in cmdDep ("branching... can be... encoded as a NOP command that specified a branch" [4] §15.5.2 p. 180 — Apple's Network
Server primer calls this same idiom a "Jump command... an abbreviated version of the 'full-up' one in the DBDMA document"
[3] §2.2.1.3 pp. 5–6), and waiting is the `w` modifier carried on any command. On completion the channel writes word 0
back: resCount is the residual, and xferStatus "is an image of the low 16 bits of ChannelStatus at completion" — the
OS drivers test active/dead/bt and the device bits in it [7], [8]. Apple's own descriptor-building macros write the
result, cmdDep and address words first and the operation word last, with a barrier between: the operation word is the
commit point of a descriptor [6].

### 2.5 The device apertures

The `1_dddd` half of the window hosts the device cells. Each is summarized here only far enough to place it in Grand
Central's map; the cells themselves are the Curio, MESH, AWACS, SWIM3 and VIA parts of the platform.

**Device 0 — 53C94 external SCSI (`+$10000`).** Sixteen byte-wide registers on `$10` centres (count, FIFO, command,
status/interrupt/sequence step/flags, configuration, clock factor, test), with the standard 53C9x read/write aliasing in
the `+$40`–`+$70` range [8]. The device tree gives it `reg = $10000` (size $100), DBDMA channel 0 at `+$8000` (size $200),
interrupt **12**, and `clock-frequency = 25000000` [5]. The 68k decoder table places it at `$F3010000` [5].

**Device 1 — MACE Ethernet (`+$11000`).** Byte-wide Am79C940 registers on `$10` centres; DBDMA channels 2 (transmit,
`+$8200`) and 3 (receive, `+$8300`); interrupt **14** [5], [7]. The decoder table places it at `$F3011000` [5].

**Device 2 — SCC "compatibility" port (`+$12000`).** The legacy Macintosh SCC aperture that the 68k Serial Driver uses:
the 68k decoder table points both its SCC-read and SCC-write entries at `$F3012000` [5], and MkLinux names the same
block `PCI_SCC_BASE` [7]. It exists so that 68k-era serial code — which expects the classic SCC access idiom — runs
unmodified; it is the one aperture exempted from Grand Central's "accessed as 32-bit words" rule [3] §4.6.1 p. 13.

**Device 3 — SCC "MacRISC" port (`+$13000`).** The ESCC aperture used by Open Firmware and the native drivers, published
as the `escc` node with channel B at `+$13000` and channel A at `+$13020` — channel A sits at the *higher* address, the
classic Macintosh Z8530 layout [5]. Four DBDMA channels serve it: 4/5 for channel A transmit/receive, 6/7 for channel B;
interrupts **15** (ch A) and **16** (ch B) [5].

**Device 4 — AWACS (`+$14000`).** The sound control registers: sound control, codec control, codec status, clipping
count, and a byte-swap register that selects the sample endianness [8]. The byte-swap bit is the one Grand Central-level
concession to endianness: "for 16 bit audio samples transferred through Grand Central; a register bit can be written which
will do a byte lane swap so that the data will appear correctly in a big endian main memory system" [3] §2.2 p. 5. DBDMA
channels 8 (output) and 9 (input); interrupt **17** [5].

**Device 5 — SWIM3 (`+$15000`).** The internal floppy controller, sixteen registers on `$10` centres (`$F3015010`…
`$F30150E0` in the ROM's own literals) [5]. DBDMA channel 1; interrupt **19** [5].

**Devices 6–7 — VIA1/Cuda (`+$16000`, 8 KB).** A full 6522 register file for the Cuda transport, strided at `$200` —
sixteen byte-wide registers spanning the declared `$2000` window, so that the two device selects 6 and 7 are consumed by
one VIA [3] p. 14; [5]:

| Index | Register | Offset |
|---|---|---|
| 0 | port B data | `+$16000` |
| 1 | port A data | `+$16200` |
| 2 | data direction B | `+$16400` |
| 3 | data direction A | `+$16600` |
| 4–7 | timer 1 counter/latches | `+$16800`–`+$16E00` |
| 8–9 | timer 2 counter | `+$17000`/`+$17200` |
| 10 | shift register | `+$17400` |
| 11 | auxiliary control | `+$17600` |
| 12 | peripheral control | `+$17800` |
| 13 | interrupt flag | `+$17A00` |
| 14 | interrupt enable | `+$17C00` |
| 15 | port A, no handshake | `+$17E00` |

The stride and the individual absolute addresses are literals in the firmware's own Cuda driver [5], and the decoder
table's VIA1 entry is `$F3016000` [5], [9]. Interrupt **18** (§3.5).

**Device 8 — MESH (`+$18000`).** The internal SCSI cell, TNT only: interrupt **13** plus DBDMA channel **10** at
`+$8A00` [5]. The Catalyst decoder table has no entry here at all [5].

**Device 9 — Ethernet address PROM (`+$19000`).** A 16-byte GBus-attached PROM on `$10` centres. Apple publishes its
layout [3] §4.6.1 pp. 14–15: bytes `+$00`–`+$20` carry the three "Group ID" bytes, `+$30`–`+$50` the three
hardware-address bytes, `+$60` is `$AA` ("signifies normal bit ordering"), `+$70` an inverted XOR checksum of the first
seven bytes, `+$80`–`+$D0` the inverted copies of the address bytes, `+$E0` is `$55` ("signifies reverse bit ordering")
and `+$F0` the non-inverted checksum. Both bit orderings of the hardware address are thus readable from the PROM, with
their signatures and checksums. On the Network Server this same chip select doubles as the second processor's
interprocessor-interrupt doorbell [3] §4.2 p. 16.

**GBus device 1 — Board Register 1 / BoxID (`+$1A000`).** The machine-identification register. The ROM reads it with a
byte-reversed load and decodes model bits from it ([5]; detail in §3.4 and §4.1): with the Network Server's naming, bits
11–12 are `BoxId0`/`BoxId1` [3] §4.6.1 p. 15. Bit 8 is a factory-test strap that must read clear for a normal boot [5].

**GBus device 2 (`+$1B000`).** Listed "Reserved" in the Network Server table [3] p. 14; on the TNT machines the space
carries the RaDACal colormap bank of the built-in video — MkLinux's Control driver constant `CONTROL_CLUT_BASE_PHYS` is
`$F301B000` [7]. The aperture is not published in the device tree.

**GBus device 3 (`+$1C000`).** On the AV machines this is the Sixty6 RGB-to-YUV convolver for the second video output
(OF node `sixty6`, interrupt **27**) [5]; on the Network Server the same chip select hosts a write-only front-panel LCD
interface and the dual-processor timebase-enable register [3] §4.6.1 p. 15. One chip select, per-machine hardware.

**GBus device 4 — NVRAM bank-select port (`+$1D000`).** One byte-wide cell that selects a 32-byte bank of the nonvolatile
store (§2.6).

**GBus device 5 — Board Register 2 (`+$1E000`).** "The same locations as in the 9500" [3] §4.6.1 p. 15. The Network Server
fills its top byte with server-monitoring status bits (fan fail, thermal trips, supply faults) [3] §4.6.1 p. 15; what the
9500's own bits are is not published (§6).

**GBus device 6 — NVRAM data window (`+$1F000`).** The 32 data bytes of the selected bank, each on a `$10` centre — byte
*j* of the bank at `+$1F000 + j×$10` [9].

### 2.6 The nonvolatile store

Grand Central's NVRAM is reached through the banked two-aperture form: the firmware's device tree gives the `nvram` node
*two* `reg` entries, `+$1D000` and `+$1F000` [5], and NetBSD's driver, which dispatches on the reg count, selects the bank
with an 8-bit write of (byte offset ÷ 32) to the port and then reads byte *j* of the bank at `data + j×$10` [9]. The store
is 8 KB (256 banks of 32 bytes) as far as the drivers see it [9]. Open Firmware keeps its environment here
(`boot-command`, `boot-device`, `input-device`, `output-device`, `real-mode?`, `little-endian?` and the rest of the
`options` node) [5], [9]; the power-on diagnostics log their results into the same store, through the same two apertures
[5]; and Mac OS keeps its extended parameter RAM here as well, at store offset `$1300` and up — which is why resetting
the parameter RAM from the keyboard and resetting the firmware's environment are different operations on this machine
[5]. This NVRAM is distinct from the 256-byte classic PRAM, which lives inside the Cuda microcontroller and is reachable
only through VIA1 packets [1] p. 19; [5].

One law of the port matters enough to state here (and again in §5): the bank-select cell is **one byte wide**. The ROM's
own parameter-RAM trap path selects the bank with a 16-bit store of the byte-swapped bank number, which delivers the bank
on the `+$1D000` byte lane and `$00` on the adjacent lane — and the adjacent lane must hit nothing [5] (*observed* on the
shipping ROM's boot path).

### 2.7 Reset state

No Apple document describes the power-on values of the interrupt block, the channel registers, the board registers or
the NVRAM port. What the software contract pins is narrower: the firmware's bring-up explicitly zeroes the interrupt mask
and flushes the clear register before enabling anything (§4.2), MkLinux's equivalent sequence disables-all, clears-all,
disables-all again [7], and each DBDMA channel is reset with the full clear-mask write before use (§4.2) — so no shipping
software relies on any reset value beyond "a freshly cleared channel stops". The one observed reset-adjacent fact is that
the NVRAM store is nonvolatile across resets, and a blank (all-zero) store is reformatted by the firmware on first boot
[5] (*observed*).

## 3. Behaviour

### 3.1 Endianness — the one structural rule

The processor bus is big-endian; the PCI bus is little-endian; "the Bandit IC performs the appropriate byte swapping and
address transformations to translate between the two addressing conventions" [1] p. 18, [2] p. 11. Grand Central sits on
the PCI side of that line, so **all of Grand Central's 32-bit registers are little-endian** — reached by big-endian
software with byte-reversed accesses. The shipped kernel uses `lwbrx`/`stwbrx` for every interrupt-block access [5];
MkLinux uses its `in_le32`/`out_le32` accessors [7]; the CHRP DBDMA convention is explicit that "only little endian
addressing for DBDMA is supported... the data byte with the smallest address is the least significant byte" [4] §15.3
p. 172; and Apple's Network Server notes state the whole rule in one sentence: "All register accesses will be default
swapped by Bandit. Multi-byte data structures (such as DBDMA commands) will need to be byte reversed by software for
consistency" [3] §2.2 p. 5.

The `$10`-centre spacing of the byte-wide cells is the same rule in hardware: a byte has no byte order, so a byte-wide
cell on a 32-bit little-endian bus needs no lane logic — it is simply given its own aligned longword slot, sixteen to a
`$100`-byte device select [3] §4.6.1 p. 13. That is why nearly every legacy aperture in §2.5 reads
`base + index × $10`.

The two exceptions to the 32-bit regime are the SCC compatibility port (byte-oriented by design, §2.5) and the audio
byte-swap register, which swaps the *sample data* lane order so 16-bit samples land correctly in big-endian memory [3]
§2.2 p. 5.

### 3.2 The interrupt sources

Thirty-two sources feed the four registers of §2.2. Each has a number, and the numbers come from three agreeing bodies of
evidence: the firmware's `AAPL,interrupts` properties (one number per device node), the shipped kernel's own dispatch
(which classifies exactly these bits), and MkLinux's commented interrupt table [5], [7]. The identity that makes the map
legible is that **a DBDMA channel's interrupt number is its channel number**: channel 0 interrupts on source 0, channel
10 on source 10, and the firmware's MESH node declares both `reg` offset `$8A00` and interrupt `10` in the same breath
[5].

| N | Source | Notes |
|---|---|---|
| 0–10 | DBDMA channels 0–10 | 0 = 53C94 SCSI DMA, 1 = SWIM3, 2/3 = Ethernet TX/RX, 4/5 = SCC ch-a TX/RX, 6/7 = SCC ch-b TX/RX, 8 = audio out, 9 = audio in, 10 = MESH DMA [5] |
| 11 | — | unassigned; MkLinux parks its NMI constant here, but its own comment on the entry reads "Reserved" [7] (§5) |
| 12 | 53C94 chip | [5], [7] |
| 13 | MESH chip | [5], [7] |
| 14 | MACE chip | the only source at 68k level 3 [5], [7] |
| 15 | SCC channel A | [5], [7] |
| 16 | SCC channel B | [5], [7] |
| 17 | AWACS | [5], [7] |
| 18 | VIA1 / Cuda | a cascade — seven VIA-level sources behind one bit (§3.5) [7] |
| 19 | SWIM3 chip | [5], [7] |
| 20 | **External Interrupt 0 — the Cuda NMI** | "EXT0 = Cuda_NMI" for the 9500 [3] §4.2 p. 16; the kernel's only level-7 source [5] |
| 21 | External Interrupt 1 | "Reserved" on the 9500 [3] p. 16 |
| 22 | External Interrupt 2 | Bandit 1's timeout interrupt on the 9500 ("Bandit only interrupts on bus timeouts") [3] p. 16 |
| 23–25 | External Interrupts 3–5 | PCI slots A, B, C on the 9500 [3] p. 16 |
| 26 | External Interrupt 6 | Bandit 2 on the 9500 [3] p. 16; **the built-in Control video VBL on the TNT 7500/8500** — the shipping System 7.6 video driver toggles mask bit 26 to enable and disable its vertical-blanking interrupt [5] (*observed*; §4.5) |
| 27 | External Interrupt 7 | PCI slot D on the 9500 [3] p. 16; **Sixty6** video-out on the 8500, per its device-tree node [5] |
| 28–29 | External Interrupts 8–9 | PCI slots E, F on the 9500 [3] p. 16 |
| 30 | External Interrupt 10 | the second processor's interprocessor interrupt on the 9500 [3] p. 16; **Platinum video VBL on the Catalyst 7200**, whose tree publishes `AAPL,interrupts` 30 for the display node [5] |
| 31 | reserved | MkLinux returns −1 [7] |

Two facts fall out of the external rows. First, "the internal Grand Central interrupt mapping is unchanged" between the
9500 and the Network Server, while the *external* wiring differs machine by machine [3] §4.2 p. 16 — the external
interrupt lines are board wiring, not chip semantics, and Apple reused them freely (on-board video occupies a
"PCI-slot-range" line on the TNT machines). Second, the 9500's own table settles the NMI question: external interrupt 0,
source 20, is the Cuda NMI line [3] p. 16, matching the kernel's level-7 assignment of source 20 [5]; MkLinux's placement
of NMI on 11 contradicts both Apple documents and its own "Reserved" comment, and should be treated as wrong (§5).

### 3.3 The interrupt line and its two acknowledge modes

The register offsets and the kernel's use of them are pinned by disassembly. The *semantics* of the line are not
described in any Apple document, and what follows was established from what the shipped software requires of the hardware
(*observed*; the mode bit's name, `ifMode1Clear`, survives from Apple's prototype equates [6]):

- **Mode 0** — the power-on behavior, and the model MkLinux's driver was written against: the interrupt line to the CPU
  is combinational, the OR of live levels and latched events, gated by the mask — `(events | levels) & mask` — and
  events are cleared by explicit write-1-to-clear writes to InterruptClear [7].
- **Mode 1** — selected by a Clear write with bit 31 set, which is what the shipped kernel issues on every interrupt
  [5]: the line becomes an **output latch** asserted on any *change* of an enabled level source — assertion **or
  deassertion** — on an enabled event edge, or on unmasking an already-pending source; the bit-31 acknowledge clears the
  latch, and only the next change re-asserts it.

The deassertion half is load-bearing, and it is the least obvious contract on the chip. The shipped kernel classifies each
interrupt from `levels & mask` and stores the resulting 68k interrupt level into the emulated-environment state; nothing
between kernel re-entries ever lowers that stored level on its own. When the 68k environment services the VIA's 60 Hz
tick and quiets the source, the *only* thing that re-enters the kernel — and thereby lets the stored level fall to zero —
is the change interrupt generated by the VIA line's **deassertion** [5] (*observed*: with the deassertion change
suppressed, the machine delivers one interrupt and never delivers another; with it present, the tick chain runs at its
proper rate through a full boot).

Equally important is what the bit-31 acknowledge is *not*: it is not a source clear. A write of `$80000000` to
InterruptClear acknowledges the mode-1 latch and must leave pending device bits standing — the kernel never clears
individual sources, because each device's driver does that against the device itself [5]. A DBDMA channel's completion,
for instance, is a *level* on its source, held until acknowledged: the Mac OS 8.1 Sound Manager writes
`InterruptClear = $00000100` (bit 8, no mode bit) both before starting channel 8's ring and again from its completion
handler — an acknowledge of a standing request [5] (*observed*; the level-until-acknowledged reading is *inferred* from
that driver idiom, no Apple document states it). MkLinux's handler instead reads InterruptEvents and writes those same
bits back to InterruptClear [7] — an events-driven acknowledge that drops the levels with them; it works under either
mode.

### 3.4 Delivery to the CPU: the kernel dispatch

The shipped NanoKernel selects its board-specific external-interrupt handler from a configuration constant — kind 2,
which indexes the TNT handler `ExtIntHandlerTNT` — and names the controller address `$F3000000` as
`LA_InterruptCtl` in the same configuration [5]. The handler's procedure, as disassembled [5]:

1. Save the scratch registers and exception state into the kernel data page; bump the external-interrupt count.
2. Enable data translation — Grand Central is only reachable translated.
3. Write `$80000000` byte-reversed to `+$28` — the mode-1 acknowledge of §3.3.
4. Read `+$24` (Mask) and `+$2C` (Levels) byte-reversed, AND them: the live set is `levels & mask`.
5. Disable translation; restore state.
6. Classify the live set into a 68k interrupt priority level and store the halfword for the emulated environment.
7. If the level is non-zero, post the interrupt condition; return.

The classification works on the byte-reversed word, so PowerPC bit *b* is interrupt number `31−b`. In priority order
[5]:

| PowerPC bits | Interrupt numbers | 68k IPL |
|---|---|---|
| 11 | 20 | **7** — the Cuda NMI alone |
| 15–16, 21–31 | 15, 16, and 0–10 | **4** — both SCC channels and all eleven DBDMA channels ("all DMA IRQs will get the priority level 4", in the kernel's own comment) |
| 17 | 14 | **3** — MACE Ethernet, alone |
| 1–10, 12, 14, 18, 19 | 30–21, 19, 17, 13, 12 | **2** — the SCSI chips, AWACS, SWIM3, the external interrupt lines |
| 13 | 18 | **1** — the VIA1/Cuda cascade: the tick, ADB, PRAM/RTC |
| — | 11, 31 | **0** — nothing pending |

Every device's tree-published interrupt number lands in the class its 68k driver expects, with no leftovers and no
collisions [5]. The same level set — 1, 2, 3, 4 and 7, and no others — is what Mac OS's own interrupt-tree library builds
(§4.5), from a different piece of Apple software entirely.

### 3.5 The VIA1 cascade

Source 18 is not one interrupt but seven. The 6522's own interrupt flag register distinguishes them, and MkLinux
demultiplexes with a second, VIA-indexed table [7]:

| VIA IFR bit | Source |
|---|---|
| 0 | the cascade itself |
| 1 | the 60 Hz tick |
| 2 | Cuda (ADB traffic) |
| 3 | VIA data |
| 4 | the VIA clock source |
| 5 | timer 2 |
| 6 | timer 1 |

So the Mac OS time base, every ADB transaction, and both VIA timers all arrive at 68k level 1 behind source 18, and the
VIA's flag register is the only way to tell them apart [7]. The 60 Hz tick itself comes from VIA timer 1 in free-run
mode; at the classic 783.36 kHz VIA timer input rate this yields the familiar 60.15 Hz Macintosh tick, and the shipping
ROM's own timer calibration and tick chain run at exactly that rate [5] (*observed*; the actual input clock Grand Central
feeds the VIA is not documented — §6).

### 3.6 DBDMA channel execution

A channel runs a command list by the CHRP rules [4] §15.5, §15.6, §15.7, with Grand Central's restrictions ("only the base
DB DMA architecture" [3] §2.2.1 p. 5) applied on top:

- **Fetch and execute.** With run set, the channel fetches the descriptor at CommandPtrLo, performs it, and advances the
  pointer by 16, to the branch target, or not at all (STOP). Command lists may loop on themselves, link linearly, or
  mix both; software appends work by overwriting a trailing STOP with a NOP or branch and setting wake [4] §15.5.2
  p. 179.
- **Conditional actions.** At each command completion the channel evaluates the `i`, `b` and `w` modifiers against
  condition bits computed from the device status byte and the three select registers — each condition is masked equality:
  `(s7..s0 & select.mask) == (select.value & select.mask)` [4] §15.4.5–§15.4.7, §15.7.1 pp. 184–185. A wait suspends the
  channel before completion processing until the condition resolves — the flow-control primitive the SCC and SCSI
  drivers rely on.
- **Status write-back.** On completion the channel writes xferStatus and resCount into the descriptor: resCount the
  residual byte count, xferStatus the low half of ChannelStatus. A partial input flushed by software is marked by flush
  still set in xferStatus; the final write-back has it clear [4] §15.4.3 p. 174. Drivers read resCount to detect short
  transfers [7], [8].
- **Errors.** An unrecoverable system-bus error during a transfer sets dead, terminates the current command, writes
  status back, and interrupts unconditionally; recovery requires software to clear run and set it again [4] §15.6.2
  p. 181.
- **Coherence.** "Transfers are always coherent" [3] §2.2.1 p. 5 — descriptor write-backs and data are visible to the
  processor without an explicit flush, which is the paired fact of Bandit's coherency configuration (see
  [bandit.md](bandit.md)).

Two behaviors are pinned by the firmware's own use rather than by any prose. First, **a channel parks *on* its STOP
descriptor**: after Open Firmware's boot program ends in STOP, the channel sits with CommandPtrLo still pointing at the
STOP, run still set and active clear [5] (*observed*; §4.6). Second, **command descriptors are fetched through the full
physical map**: the firmware's own boot-beep program and its sample data live in the ROM at `$FFE00090`-class addresses,
and the engine fetches them there [5] (*observed*) — there is no RAM-only path in the descriptor fetch.

The `i` modifier is how a channel signals completion: an interrupt is requested at command completion per the `i` field,
and CHRP's requirement is that "the PCI devices described herein are allowed one interrupt each to the system. DBDMA
interrupts must be shared with the other interrupt requirements of the device" [4] requirement 15-3, §15.9 p. 191.
Grand Central is an on-board controller rather than a plug-in card, and it deviates in the programmer's favor: **every
channel has its own interrupt source** — channel *n* is source *n* — each independently maskable (§3.2), so a device
with running DMA raises two interrupts (its device number and its channel number), and both must be independently
maskable [5].

### 3.7 Timing

Grand Central's own clock domain is the 33 MHz PCI bus [1] p. 18, [2] p. 11. The timing software actually depends on:

- the **60.15 Hz tick** (§3.5), which drives every Mac OS time-out;
- the **53C94's 25 MHz** and **MESH's 50 MHz** clocks, which set the two SCSI buses' transfer arithmetic (§1.5);
- the **SWIM3 timer's 1 MHz** countdown for floppy head-step timing [8];
- the **AWACS sample clocks**, selected by a rate field among the 44.1 kHz family (44100, 29400, 22050, 17640, 14700,
  11025, 8820, 7350 Hz) [8] — with the audio DMA channels consuming samples at the selected rate or drifting.

Interrupt latency through the chip is not documented anywhere in the evidence set (§6).

## 4. Programming model

### 4.1 Enumeration: the device tree is the contract

No operating system probes for Grand Central in PCI configuration space. All of them — Mac OS, MkLinux, Linux, NetBSD —
attach it and its children from the Open Firmware device tree, reading every base address, size and interrupt number from
node properties [5], [7], [8], [9]. The tree the TNT ROM builds is [5]:

```
device-tree  (model "Power Macintosh", compatible "AAPL,7500" | "AAPL,8500" | "AAPL,9500" | "AAPL,7300" | "AAPL,????", then "MacRISC")
├── chaos@F0000000                          the display bus (not Grand Central's)
├── bandit@F2000000                         pci1
│   └── gc                                  model "AAPL,343S1125", device_type "dbdma"
│       ├── 53c94          scsi              reg $10000, DMA $8000,      AAPL,interrupts 12
│       ├── mace           network,ethernet  reg $11000, DMA $8200/$8300, AAPL,interrupts 14
│       ├── escc
│       │   ├── ch-a       serial,modem      reg $13020, DMA $8400/$8500, AAPL,interrupts 15
│       │   └── ch-b       serial,printer    reg $13000, DMA $8600/$8700, AAPL,interrupts 16
│       ├── escc-legacy                      the $12000 compatibility aperture
│       ├── awacs          sound             reg $14000, DMA $8800/$8900, AAPL,interrupts 17
│       ├── swim3          block,floppy      reg $15000, DMA $8100,       AAPL,interrupts 19
│       ├── via-cuda       cuda              reg $16000 (size $2000),     AAPL,interrupts 18
│       │   ├── adb → keyboard, mouse
│       │   ├── pram
│       │   └── rtc
│       ├── mesh           scsi              reg $18000, DMA $8A00,       AAPL,interrupts 13, 10
│       ├── sixty6         display,video-out reg $1C000,                  AAPL,interrupts 27   [AV]
│       ├── nvram                            reg $1D000 and $1F000
│       └── power-mgt
└── bandit@F4000000                         pci2 [8500/9500]
```

The `aliases` node names the paths software uses — `fd` (floppy), `kbd` (keyboard), `ttya`/`ttyb` (serial), `enet`,
`scsi`, `scsi-int` — all of them through Grand Central [5]. Every `reg` offset above is a window offset from §2; every
`AAPL,interrupts` value is a source number from §3.2; and the ROM's 68k half carries the same addresses again in its
decoder tables (VIA1 `$F3016000`, SCC read/write `$F3012000`, 53C94 `$F3010000`, MESH `$F3018000`, MACE `$F3011000`,
SWIM3 `$F3015000`, AWACS `$F3014000`, Grand Central itself `$F3000000`) [5], so the 68k Toolbox and the native
environment agree device-for-device.

The tree is also the machine-identity mechanism: Open Firmware reads the Board Register 1 location (`+$1A000`) with a
byte-reversed halfword access and, together with the Hammerhead identification registers, selects the `compatible`
string — and with it whether the display nodes exist at all; an unrecognized box publishes `AAPL,????` and gets no
`chaos`/`control` nodes [5]. The lesson generalizes across the family: the interrupt wiring and the tree are per-machine
contracts, not constants of the chip.

### 4.2 The firmware bring-up sequence

Open Firmware performs the hardware bring-up on this platform (the ROM's power-on self-test is a diagnostics blob, not an
initializer) [5]. The Grand Central-relevant part of a cold boot runs in this order [5] (*observed* on the shipping
firmware's boot path):

1. The Bandit bridge is configured, including its PCI coherency mode (see [bandit.md](bandit.md)) — this must precede any
   DMA, since descriptor write-backs depend on it.
2. Grand Central's base is programmed through configuration cycles so its 128 KB response lands at `$F3000000` [3]
   §4.2.1 p. 12.
3. InterruptMask is cleared, then InterruptClear is written to flush stale events [5], [7].
4. Each DBDMA channel is reset — the full clear-mask write
   `(active|dead|wake|flush|pause|run) << 16` to ChannelControl, then polled until run drops — and, for the boot beep,
   loaded and started (§4.6) [7], [5].
5. Devices are probed in device-tree order and published with their `reg` and `AAPL,interrupts` properties [5].
6. InterruptMask is set per enabled device as drivers install [5], [7].

The power-on self-test, for its part, logs its results into the Grand Central NVRAM through the two apertures — a
byte-reversed longword store to the bank-select port followed by byte stores into the data window — before the operating
system ever runs [5]. The machine-identification bits of Board Register 1 are read with a byte-reversed load early in
the same path [5].

### 4.3 The shipped kernel's contract

The NanoKernel drives the interrupt block exclusively in the mode-1 style of §3.3–§3.4: one `$80000000` acknowledge per
interrupt, classification from `levels & mask`, never a source clear. Its configuration names the controller address and
the handler kind; its interrupt masks (the conditions it tests to decide whether an interrupt is pending) are
`$00200000`-family constants over the byte-reversed picture [5]. No shipped Apple software ever reads the Events register
on this platform — the kernel does not, and the native drivers do not [5]; the event-latch half of the block is
effectively reserved for the events-driven style of §4.4.

### 4.4 The MkLinux model

MkLinux (Apple/OSF's port of Linux to this platform) treats Grand Central as its system interrupt controller and, in the
same sources, as the DMA engine for everything. Its published constants match the window of §2 entry for entry —
`GRAND_CENTRAL_BASE $F3000000`, `GRAND_CENTRAL_SIZE $20000`, DMA at `+$8000`, the 53C94 at `+$10000`, Ethernet at
`+$11000`, the Ethernet address PROM at `+$19000`, audio at `+$14000`, floppy at `+$15000`, the VIA at `+$16000`, MESH at
`+$18000` — and its interrupt controller is the four contiguous longwords at `+$20` [7]. Its initialization sequence is
worth quoting as the canonical mode-0 style [7]:

```c
gc_ints->mask  = 0;            /* disable all sources   */
gc_ints->clear = 0xffffffff;    /* clear everything       */
gc_ints->mask  = 0;            /* disable all again      */
```

and its handler reads Events, then writes those same bits back to Clear — the events-driven acknowledge of §3.3 [7]. Its
interrupt tables (`powermac_pci_interrupts[]`, 32 entries commented by bit number, and the seven-entry VIA cascade table)
are the independent confirmation of the source map of §3.2 [7].

### 4.5 Mac OS: the interrupt tree

Mac OS builds its interrupt dispatch from a ROM-resident native library, `InterruptTreeTNT`, whose exported symbols name
the whole structure: `CreateGCLevel1Tree`/`GCLevel1TreeDispatcher` through `CreateGCLevel4Tree`, plus
`CreateGCLevel7Tree` — **levels 1, 2, 3, 4 and 7, and no others**, exactly the set the kernel's classification produces
(§3.4), from a completely separate piece of Apple software [5]. The library consumes the device-tree properties
`AAPL,address`, `name`, `device_type`, `AAPL,interrupts`, `driver-ist` and `pci-bridge`, and installs each device's
handler at the level its source number implies [5]. In operation:

- **The tick** — VIA timer 1 → VIA flag bit 1 → source 18 → level 1 → the Ticks handler. The VIA dispatcher's
  "no source" entry (a plain return) exists precisely to absorb the mode-1 change interrupts that arrive with no VIA bit
  set [5].
- **Audio** — the Sound Manager loads a two-buffer ring onto channel 8, acknowledging source 8 with a plain
  `InterruptClear = $00000100` before each start and from each completion handler [5] (*observed*). The ROM's own native
  sound driver, notably, writes channel 8's CommandPtrLo once at initialization while the channel is still parked on the
  firmware beep's STOP — run still set, active clear — without clearing run and without starting that program [5]
  (*observed*; whether that write is meaningful on silicon is §6).
- **Built-in video** — the Control display driver enables its vertical-blanking interrupt by toggling mask bit **26**
  (source 26) in the same breath as it writes the video controller's own interrupt-enable register, and its handler
  performs an enable-register gate dance every frame rather than write-1-to-clear any status — the Grand Central line for
  the VBL is a level mirroring the video controller's status-and-enable, and dropping the gate is the acknowledge [5]
  (*observed*; this is how the TNT VBL wiring of §3.2 was pinned).
- **Serial** — the 68k Serial Driver reaches the SCC through the compatibility aperture at `+$12000`, while Open Firmware
  and the native drivers use the MacRISC aperture at `+$13000`; both are decoded, and both are the same Curio ESCC cell
  [5].

### 4.6 A worked example: the firmware boot beep

The clearest single demonstration of the DMA engine is the beep Open Firmware plays at every cold boot. Its channel
program is built into the ROM image itself, at ROM logical `$FFE00090`, and decodes as [5] (*observed* on the shipping
firmware's boot path):

```
$FFE00090:  OUTPUT_MORE   key STREAM0, reqCount $8000, address $FFE200A0
$FFE000A0:  OUTPUT_MORE   key STREAM0, reqCount $8000, address $FFE280A0
$FFE000B0:  OUTPUT_LAST   key STREAM0, reqCount $2B60, address $FFE300A0
$FFE000C0:  STOP
```

Three OUTPUTs moving 76,640 sample bytes out of ROM, then STOP — meaningful only under the §2.4 command encoding, and
(note the addresses) fetched and read by the engine from ROM physical space. The firmware loads channel 8's CommandPtrLo
with the program's address, starts it with a single ChannelControl write of `$F0008000` (mask run|pause|flush|wake,
value run), and polls the channel to completion; the boot does not proceed until the channel completes [5]. Afterwards
the channel sits parked *on* the STOP descriptor — CommandPtrLo unchanged, run still set, active clear (§3.6) — which is
the state the Mac OS sound driver later finds and (apparently) writes over (§4.5).

## 5. Quirks & errata

- **A little-endian island behind a big-endian bus.** Every 32-bit register in the window is little-endian; software
  swaps, with `lwbrx`/`stwbrx`, and swaps the memory-resident descriptors too ("Multi-byte data structures (such as DBDMA
  commands) will need to be byte reversed by software" [3] §2.2 p. 5). There is no lane swapper inside the window except
  the audio sample byte-swap bit.
- **The `$10`-centre stride is the endianness workaround.** Byte-wide cells sit one per aligned longword slot so an 8-bit
  part can hang off a 32-bit little-endian bus with no lane logic [3] §4.6.1 p. 13; the SCC compatibility port is the one
  exception to 32-bit access.
- **Channel number equals interrupt number.** DBDMA channel *n* raises source *n* — an on-board deviation from CHRP's
  one-interrupt-per-PCI-device rule [4] req. 15-3, and the reason a device with running DMA raises two independently
  maskable interrupts.
- **Two SCC apertures, both real.** `+$12000` for the 68k Serial Driver, `+$13000` for Open Firmware and native drivers;
  a model of one without the other breaks one half of the ROM's software [5]. And channel A is at the *higher* address
  (`+$13020`), the classic Macintosh layout [5].
- **The NVRAM bank-select port is one byte-wide cell.** The ROM's parameter-RAM trap path selects a bank with a 16-bit
  store of the byte-swapped bank number, so the adjacent byte lane carries `$00` and must hit nothing; a model that
  latches any byte in the block silently selects bank 0 for every trap access [5] (*observed*).
- **`$80000000` to InterruptClear is an acknowledge, not a source clear.** It selects and clears the mode-1 latch;
  clearing pending device bits with it loses interrupts, because the shipped kernel never clears individual sources [5].
- **The mode-1 latch fires on deassertion too.** Without deassertion-change interrupts, the stored 68k interrupt level
  never falls after a level-1 VIA service and the machine starves after its first unmask [5] (*observed*). This mirrors
  the INTMODE=1 deassertion latch of the previous generation's AMIC controller — the same-era Apple I/O parts share the
  law.
- **The VBL is not one interrupt.** Source 26 on the TNT machines (Control video), source 30 on the Catalyst 7200
  (Platinum), source 27 for Sixty6 on the 8500 — an "external interrupt" line in the slot range is on-board video on the
  machines that have it [5]. MkLinux's generic table calls sources 21–30 `CARD1`–`CARD10` and is wrong for the TNT VBL
  [7].
- **The NMI is source 20, not 11.** Apple's own 9500 wiring table puts `Cuda_NMI` on EXT0 [3] p. 16, and the kernel's
  only level-7 source is 20 [5]; MkLinux parks `PMAC_DEV_NMI` on 11 against its own "Reserved" comment [7].
- **The 1994 prototype bit numbering does not match shipping silicon.** Apple's bring-up equates carry a full `gcif*`
  constant set (`gcifDevMACE`, `gcifDmaSCSI0`, …) whose bit positions are prototype-board values and map to nothing
  consistent on the shipping chip; only the register offsets and the level assignments from that tree survived [6]. Do
  not use the `gcif*` numbers.
- **Apple's 1993 pre-release DBDMA interface names the status bits one position off.** `Halted`/`Dead`/`Active`/
  `Paused` in the early interface header [6] against `flush`/`wake`/`dead`/`active` in the published architecture [4] —
  and the shipped silicon follows the published one: the firmware's own poll loops terminate only under the CHRP
  meanings [5].
- **STOP and the run bit disagree across documents.** The Network Server primer says Stop "will clear... the Run bit in
  the Status register" [3] §2.2.1.3 p. 6; the firmware's boot channel parks on STOP with run still set [5] (*observed*).
  The observed behavior matches the published architecture's wake semantics (an idle channel with run set is re-armed by
  wake refetching the STOP) [4] §15.4.3 p. 174.
- **Ten channels or eleven.** The Network Server tables list channels `$00`–`$09` [3] p. 14; the TNT machines carry
  eleven, adding channel `$0A` for MESH [5]. Same chip, one more device — count per machine.
- **`$F3000000` is a convention.** The window is a re-positionable PCI memory response; the address is fixed only by the
  TNT ROM releases' firmware and the OS constants that copied them [3] §4.2.1 p. 12, [5], [7].
- **Unmapped space answers.** Writes to unmapped offsets in the 128 KB window are discarded and reads return zero [3]
  §4.6.1 p. 13 — a probe will not fault, it will just read nothing.
- **Board Register 1's bit 8 is a factory strap.** Set, it diverts the boot into the ROM's serial test monitor [5]
  (*observed*); it must read clear on a production machine.
- **Grand Central wins PCI arbitration, always.** It is priority 1 against the round-robin of the slots and the Bandit
  master [1] p. 18, [2] p. 11 — a saturated PCI bus cannot starve the DMA engine, though it can starve everything else.

## 6. Open questions

1. **The Grand Central ERS.** Apple's Network Server notes defer all register-level detail to "the Grand Central ERS"
   [3] §4.6.1 p. 13, which was never published. Everything above §2 is reconstructed from firmware, OS drivers and the
   CHRP/developer-note prose; the ERS is the one document that would settle the rest of this list.
2. **Grand Central's own PCI header.** Whether the chip presents a vendor/device ID in configuration space, and what it
   is, is unverified: the device ID pairing `106B:0002` circulates in secondary literature, but the shipped ROM's own
   bridge-enumeration literal pool carries only the Bandit (`$0001106B`), Chaos (`$0003106B`) and an unidentified
   `$0004106B` — no Grand Central pair [5]. The ROM's Open Firmware reaches the chip through its programmed base
   address, not through a header match, and both Mac OS and MkLinux enumerate it from the device tree [5], [7].
3. **Interrupt 11.** Unassigned in every Apple source; MkLinux's `PMAC_DEV_NMI` placement there is contradicted by its
   own comment and by both Apple documents (§5). Whether any machine wires a source to bit 11 is unknown.
4. **The condition-select registers.** Whether Grand Central implements InterruptSelect/BranchSelect/WaitSelect or
   hardwires the conditions true; no driver in the corpus programs them, and Apple's notes list neither among the
   explicitly unimplemented optionals [3] §2.2.1 p. 5, [7], [8]. The same question covers Apple's earlier optional
   register set (`+0x10`–`+0x2C` in the prototype header) [6].
5. **External-interrupt wiring on the 7500/8500.** The 9500's slot-to-EXT assignment is published [3] p. 16; the 7500's
   and 8500's are not, and nothing pins where the 8500's second Bandit's timeout interrupt lands (EXT6 is Bandit 2 on
   the 9500 but the built-in video VBL on the 8500). PCI slot interrupt numbers are assigned at firmware probe time and
   were not recoverable statically; the device tree is the contract, per machine.
6. **Board Register 2 (`+$1E000`).** Its existence on the 9500 is attested [3] p. 15, but only the Network Server's
   fields (fan, thermal, supply bits) are documented. What the 9500/7500/8500 boards carry there is unknown, as is
   whether any TNT software reads it.
7. **The controller register space below `+$8000`.** Whether anything lives between `+$30` and `+$7FFF` — in particular
   whether Grand Central has device power/enable gating registers — is open; no OS driver source programs any, and the
   ROM appears to do power management through Cuda instead [5], [7], [8].
8. **Events vs levels.** The Events register is never read by any shipped Apple software on this platform [5]; the
   edge-latching half of the block is exercised only by MkLinux's events-driven style [7]. Which sources are
   edge-latched into Events at all, and how Events behaves in mode 1, is unpinned.
9. **The NVRAM store's extent and type.** 8 KB is the attested size through the banked window [9]; whether the part is
   larger, whether the bank-select port carries bits beyond the bank number, and whether the store is battery-backed
   SRAM or an EEPROM with a write-enable sequence are all unknown.
10. **Board Register 1's remaining bits.** Bits 8, 11–13 are decoded (§2.5); the rest of the register's per-machine
    field layout — and the exact bit-numbering convention Apple's own tables use for it — is not published.
11. **The VIA timer input clock.** The 60.15 Hz tick is behaviorally confirmed [5] (*observed*), but the actual rate
    Grand Central feeds the VIA timers is not documented, and it is the number every Mac OS time-out ultimately derives
    from.
12. **The parked-channel pointer write.** The ROM's native sound driver writes channel 8's CommandPtrLo while the
    channel is parked on the firmware's STOP with run set [5] (*observed*) — against the published rule that
    CommandPtrLo writes are ignored unless run and active are both clear [4] §15.4.4 p. 176. Whether silicon accepts the
    write, ignores it, or something subtler happens is unresolved.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500 Computers*, Developer Press,
   1995 — §"Bus Bridge" p. 17 (asynchronous 33 MHz PCI / 50 MHz processor buses); §"Bandit PCI Bridge IC" p. 18
   (bridge buffering, 32-byte bursts, arbitration priorities, big/little-endian byte swapping); §"Grand Central I/O
   Subsystem IC" p. 18 (interface to PCI, functions list, SWIM III, bus interfaces to Curio/Cuda/MESH/AWAC, 16-bit bus
   to NVRAM and Sixty6, 33 MHz PCI clock); §"Curio I/O Controller IC", §"Cuda Microcontroller IC", §"MESH High-Speed
   SCSI Interface", §"AWAC Sound IC" p. 19; block diagram p. 15; Chapter 4 "Expansion Features" pp. 49–51 (§"PCI
   Expansion Slots", Table 4-8 PCI signals, §"DAV Connector").
2. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers* (Power Macintosh 9500/120 and Power Macintosh
   9500/132), Developer Press, 1995 — §"Bandit Bus Bridge ICs", §"Bus Clock Rates", §"Big-Endian and Little-Endian Bus
   Addressing", §"Grand Central I/O System IC" p. 11 (two Bandits, 33/40/44 MHz, functions list, DBDMA support for all
   I/O transfers, 16-bit bus to NVRAM); §"Curio I/O Controller IC" and §"Cuda Microcontroller IC" p. 12; §"AWAC Sound
   IC" and §"MESH SCSI Controller IC" p. 13; Chapter 4 §"PCI Expansion Slots" p. 33.
3. Apple Computer, Inc., *Network Server Hardware Developer Notes* (Apple Network Server 500/700), 1996 — §2.2
   "Network Server Quick I/O Primer" pp. 5–6 (Grand Central as non-critical I/O, "appears as a PCI 2.x-compliant
   device", the 128 KB space, Bandit's default swapping, the audio byte-lane-swap bit, base-DBDMA-only with the
   unimplemented-optional list, DBDMA register format and command primer, GBUS chip selects); §4.2.1 "Grand Central
   Device Registers" p. 12 (VIA compatibility as the reason for 128 KB, re-positionable PCI memory response,
   `$F3000000`–`$F301FFFF` as convention); §4.6.1 "Network Server Device Address Map" pp. 13–15 (register strides,
   unmapped-access behavior, the three-way decode law, channel-number and device-number tables, GBUS register layouts
   including the Ethernet address PROM, board registers 1 and 2, the LCD interface); §4.2 "Network Server External
   Interrupt Map" p. 16 (EXT0–EXT10 wiring for the 9500 and Network Server, Cuda_NMI, Bandit timeout interrupts,
   unchanged internal mapping); "Grand Central ERS" cited p. 13.
4. Apple Computer, Inc., International Business Machines Corporation, Motorola, Inc., *PowerPC Microprocessor Common
   Hardware Reference Platform: I/O Device Reference*, Version 1.0, May 1996 — Chapter 15 "Descriptor-Based DMA"
   pp. 171–194: §15.1–§15.3 pp. 171–172 (characteristics, little-endian convention); requirements 15-1 and 15-2 p. 172;
   §15.4.1 Table 321 p. 173 (channel registers); §15.4.2 Table 324 p. 174 (ChannelControl mask/value); §15.4.3 Table 326
   p. 174 (ChannelStatus bits, run/pause/flush/wake/dead/active/bt/s7–s0); §15.4.4 pp. 176 (CommandPtrLo alignment and
   write-ignore rule); §15.4.5–§15.4.7 pp. 176–177 (the three select registers and their condition equations);
   §15.5.2 pp. 179–180 (command-list structure, STOP, appending, wake); §15.6.2 p. 181 (system bus errors and the dead
   bit); §15.7.1 pp. 182–186, Tables 333–342 (command descriptor format, cmd/key/i/b/w encodings); §15.9 p. 191,
   requirement 15-3 (one interrupt per PCI device, DBDMA interrupts shared).
5. Power Macintosh 7200/7500/8500/9500 boot ROM, revisions `96CD923D` and `9630C68B` (August 1995; 4 MB mask ROM based
   at `$FFC00000`) — decoded contents, disassembled and read as data: the Open Firmware 1.0.5 device tree (the `gc`
   node with `model "AAPL,343S1125"` and `device_type "dbdma"`; per-node `reg`, `AAPL,interrupts`, `clock-frequency`
   and `aliases`; the machine-identity decode that reads Board Register 1 byte-reversed and selects the `compatible`
   string); the NanoKernel v01.01 with `ExtIntHandlerTNT` and its board-handler table, the `$80000000` acknowledge,
   the `levels & mask` classification and the level table of §3.4; the ConfigInfo page (`LA_InterruptCtl $F3000000`,
   `InterruptHandlerKind = 2`, pre-seeded low-memory values including the VIA1 base); the 68k decoder tables (TNT and
   Catalyst variants); the HWInit/POST blob (BoxID read with `lwbrx`, bit-8 test, NVRAM logging through `+$1D000`/
   `+$1F000`); the `InterruptTreeTNT` native library and its level-tree symbol set; the native sound driver's channel-8
   literals; the System 7.6-era Control video driver's Grand Central mask writes (source 26) alongside its video
   interrupt-enable dance; the boot-beep DBDMA program and its ROM-resident descriptors at `$FFE00090` with the
   `$F0008000` channel-start write; the XPRam trap path's 16-bit byte-swapped bank-select write.
6. Apple Computer, Inc., SuperMario Macintosh ROM project source snapshot, internal, dated 1994-02-09 — <!-- lint-allow: SuperMario -->
   `Internal/Asm/GrandCentralPriv.a` (the prototype Grand Central equates: interrupt-register offsets `+$20/24/28/2C`,
   the `ifMode1Clear` bit name, and the prototype `gcif*` interrupt constants that do *not* match shipping silicon);
   `Interfaces/CIncludes/DBDMA.h` (the 1993 pre-release DBDMA interface: channel register layout, mask/value control
   constants, the earlier status-bit names, and the `MakeCCDescriptor` commit-order macros);
   `OS/UniversalTables.a` (the decoder-table field order).
7. The MkLinux Project (Apple Computer, Inc. / OSF Research Institute), MkLinux DR3 source release —
   `POWERMAC/powermac_pci.h` (`GRAND_CENTRAL_BASE`, `GRAND_CENTRAL_SIZE`, the window-offset constants, the
   `GC_INTERRUPT_EVENTS/MASK/CLEAR/LEVELS` offsets); `POWERMAC/interrupt_pci.c` (`powermac_pci_interrupts[]` with its
   commented 32-bit table, the VIA cascade table `pci_via1_interrupts[]`, the controller init sequence and the
   events-driven handler); the Control video driver constant `CONTROL_CLUT_BASE_PHYS`.
8. Linux kernel source, PowerPC "powermac" platform — `arch/powerpc/platforms/powermac/pic.c` (Grand Central-class
   interrupt controller); `drivers/scsi/mac53c94.{c,h}` (53C94 register map on `$10` centres); `drivers/scsi/mesh.h`
   (50 MHz MESH clock assumption); `drivers/block/swim3.c` (1 MHz timer countdown); `drivers/macintosh/via-cuda.c` (the
   `$200`-stride VIA indices and the Cuda handshake bits); `sound/ppc/awacs.h` (AWACS register layout and rate codes).
9. NetBSD source, `sys/arch/macppc` — `dev/nvram.c` (the two-aperture banked NVRAM law: bank select at `+$1D000`, byte *j*
   at `+$1F000 + j×$10`, 8 KB total); `dev/cuda.c` (Cuda transport over the VIA); `dev/esp.c` (53C94 driver for this
   platform's aperture layout).
