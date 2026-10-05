# The PDM family — Power Macintosh 6100/7100/8100

**Contents:**

1. [Overview & membership](#1-overview--membership) — the platform, its two ROMs, the machines that run it, how
   this page splits against the machine and device pages
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the one-board layout,
   the 601, the memory subsystem, the I/O subsystem, endianness, machine identity, the ROM image and the boot
   contract, reset and power
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family) —
   physical map, DRAM banks, ROM decode, the I/O block, the slot windows, the logical (MMU) map, the
   framebuffer and the DMA buffer
4. [Device roster](#4-device-roster) — every chip on the board, its function, its device page
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the single interrupt
   collector, CPU-bus arbitration, bus errors, the clock set, reset distribution
6. [Per-machine index](#6-per-machine-index) — the 6100, the 7100, the 8100, the Enhanced variants, adjacent
   machines and upgrade cards
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the platform is

The **PDM** platform is Apple's first-generation Power Macintosh logic board: the first Macintosh
platform whose main processor is a **PowerPC 601**, the first to run the classic 68k Mac OS through
a software emulator hosted by a PowerPC "nanokernel" in ROM, and the last to reach expansion cards
through NuBus. Apple's developer note dates the generation to March 1994 and names its machines
the Power Macintosh 6100/60, 7100/66 and 8100/80, each with an AV variant [1] cover, §"Models and
Configurations". The platform name is Apple's own: PDM is the project codename the machines carry
inside their ROMs — the 6100's board is codenamed PDM ("Piltdown Man"), the 7100's **Carl Sagan**
and the 8100's **Cold Fusion** [4].

The architecture is a two-chip split of the functions a Quadra-era Macintosh spread across a
handful of parts. The **HMC** ("high-speed memory controller"; the 8100 logic-board schematic
titles the part "Half A Memory Controller") owns the DRAM, the ROM, the second-level cache, the
601 bus protocol and address-bus arbitration [1] §"High-Speed Memory Controller" p. 15. The
**AMIC** ("Apple Memory-Mapped I/O Controller"), a 160-pin gate array, owns everything else on
the I/O side: all I/O-space address decode, the classic-Mac interrupt model (emulated, not a
physical 6522 in sight), a ten-channel DMA engine, video timing and monitor sense, and the sound
transport [1] §"Apple Memory-Mapped I/O Controller" pp. 15–16. Expansion slots reach the CPU bus
through the **BART** NuBus controller [1] §"BART NuBus Controller" p. 17, and two **data path**
chips bridge the 64-bit CPU/memory buses to the 16-bit I/O bus and feed the video FIFOs
[1] §"Data Path Chips" p. 16. The peripheral set — SCC serial, 53C94-class SCSI, MACE Ethernet,
the SWIM III floppy controller, the AWACS sound codec, the Ariel II RAMDAC and the Cuda
microcontroller — is described device by device in §4.

One ROM image, 4 MB, serves the whole generation; it contains the 68k Mac OS ROM, the PowerPC
exception vectors, a hardware-initialisation routine, the nanokernel, the 68k emulator and the
emulator's 512 KB opcode dispatch table, in that order (§2.7). Apple shipped two releases of the
image: the original March 1994 ROM (header checksum `$9FEB69B3`, bootstrap version "Boot PDM
601 1.0") and the January 1995 ROM (checksum `$9B7A3AAD`, "Boot PDM 601 1.1") that accompanies
the Enhanced models [4], [5].

The subject of this page is what every board in the family shares. Register-level and
behaviour-level detail lives in the device pages (§4), and per-machine deltas belong to the
machine pages (§6); the three levels together are the specification of the platform.

### 1.2 Membership

Membership is defined by the machine-ID register at the top of I/O space, `$5FFFFFFC`: every
board answers `$A55A30xx`, with the low half identifying the model — `$3010` for every 6100,
`$3012` for every 7100, `$3013` for every 8100 (§2.6). Both ROM releases read this register on
every boot and pick their configuration tables from the answer [4], [5]; the register values and
the Gestalt machine types Apple documents for the three original models are [1] Table 1-5 p. 10:

| Machine | Board codename | CPU / bus (MHz) | Machine-ID low word | Gestalt machine type | Machine page |
|---|---|---|---|---|---|
| Power Macintosh 6100/60, /60AV | PDM ("Piltdown Man") | 60 / 30 | `$3010` | `$4B` | [pm6100.md](pm6100.md) / §6.1 |
| Power Macintosh 6100/66 | PDM | 66 / 33 | `$3010` | `$4B` | [pm6100.md](pm6100.md) / §6.1 |
| Power Macintosh 7100/66, /66AV | Carl Sagan | 66 / 33 | `$3012` | `$70` | [pm7100.md](pm7100.md) / §6.2 |
| Power Macintosh 7100/80 | Carl Sagan | 80 / 40 | `$3012` | `$70` | [pm7100.md](pm7100.md) / §6.2 |
| Power Macintosh 8100/80, /80AV | Cold Fusion | 80 / 40 | `$3013` | `$41` | [pm8100.md](pm8100.md) / §6.3 |
| Power Macintosh 8100/100 | Cold Fusion | 100 / 33.3333 | `$3013` | `$41` | [pm8100.md](pm8100.md) / §6.3 |
| Power Macintosh 8100/110 | Cold Fusion | 110 / 36.6667 | `$3013` | `$41` | [pm8100.md](pm8100.md) / §6.3 |

Clock rates are from [1] Table 2-2 p. 19 and [2] Table 1-2 p. 3; the Enhanced models (6100/66,
7100/80, 8100/100, 8100/110) are the January 1995 refresh of the same three boards [2]. Two
edges of the membership are worth stating precisely. First, the **Enhanced 6100/66 runs the
original ROM** — Apple's own comparison table marks it "new ROM: no" [2] Table 1-3 p. 5 — so the
ROM split is 7100/80 + 8100/100 + 8100/110 (new) versus everything else (old), not "original vs
Enhanced". Second, the same two ROM images also carry decoder records for the **PowerPC processor
upgrade cards** (the 601 PDS cards for the Quadra 610/650/700… class), whose host tables describe
classic Quadra-style I/O maps — one image, several platforms; see §6.5.

### 1.3 The two ROMs

| Image | Header checksum | Bootstrap version | Machines |
|---|---|---|---|
| March 1994 | `$9FEB69B3` | "Boot PDM 601 1.0" | 6100/60, 7100/66, 8100/80 (all +AV), 6100/66 |
| January 1995 | `$9B7A3AAD` | "Boot PDM 601 1.1" | 7100/80, 8100/100, 8100/110 |

The 1995 image is the 1994 image with four documented behavioural additions, all in the NuBus
domain: minor-slot addressing extended from slots $9–$E down to slot $1, major slot $8 made
addressable, per-slot NuBus cacheability control through three `_HWPriv` selectors, and —
together with the BART 21 bridge part — slot-specific burst transactions and burst reads by
NuBus masters [2] pp. 3–5. The LocalTalk driver in ROM was also updated [2] §"LocalTalk" p. 5.
Everything else in this document — the address map, the ASICs, the interrupt model, the boot
sequence — is common to both releases; where a difference matters it is called out at the point
of use.

### 1.4 How to read this page set

The family split is: this page holds what every board shares; the device pages hold the chips;
the machine pages hold the per-box deltas. Nothing in the device pages is restated here — they
are cited by section, e.g. [hmc.md](hmc.md) §3.5 for bus arbitration,
[amic.md](amic.md) §3.2 for the interrupt level model, [bart.md](bart.md) §3.1 for the slot
address decode. A re-implementation of a machine reads this page first, then the device pages it
points at, then the machine page for the box in question.

## 2. Board architecture common to the family

### 2.1 The board and its two buses

All of the circuitry of the three machines sits on one multilayer main logic board whose units
are summarised by the developer note's block diagram (Figure 2-1, drawn for the 8100/80, with
the parts omitted on the smaller machines enclosed in dotted lines) [1] §"System Architecture"
p. 13. Two dotted-line groups define the family's internal variation: the BART NuBus controller
and the single NuBus slot connector of the 6100/60 live on a plug-in adapter card rather than
the board, and the fast internal SCSI bus exists only on the 8100 [1] §"System Architecture"
p. 13. Every other block — 601, HMC, AMIC, the two data path chips, Curio, SWIM III, Ariel II,
AWACS, Cuda, the ROM SIMM, the DRAM, the cache connector — is on all three boards.

There are two buses. The **CPU bus** is "the standard 64-bit nonpipelined bus used by the
PowerPC 601 processor"; the processor, the HMC, the AMIC (as DMA master), the data path chips
and any expansion card mastering through the processor-direct slot all hang off it, with the
HMC arbitrating the address part (§5.2) [1] §"CPU Bus" p. 20. The **NuBus** is a separate,
10 MHz NuBus '90 bus reached only through BART; the two are bridged, not continuous — NuBus
cards master into RAM and ROM through BART acting as a CPU-bus master, and "it is not designed
to let plug-in cards gain access to peripheral devices directly" [1] §"NuBus Interface" p. 50.
Register-level detail of the bridge and the slot windows is [bart.md](bart.md) §1, §3.

### 2.2 The main processor

The main processor is a PowerPC 601: a RISC part with parallel integer and floating-point units,
a branch manager that usually folds branches into the incoming instruction queue, an internal
MMU, and 32 Kbit of on-chip cache [1] §"Main Processor" p. 14. For the 601's architecture —
register set, MMU, exception model, cache organisation — the reference is the manufacturer's
user's manual [6]; this page set covers only what the platform adds around the chip.

Three platform facts bind the processor to the board:

1. **The 601's external input, `RTC`, runs at 7.8336 MHz** — the 31.3344 MHz I/O oscillator
   divided by four. The boot ROM hard-codes the resulting decrementer rate (1,002,700,800
   RTCL-units per second) and *measures* the CPU and bus clocks against it at every boot rather
   than reading a table (§5.4) [4].
2. **The 601 has one external interrupt input**, level-sensitive, driven by AMIC's interrupt
   control register; the entire classic 68k seven-level interrupt structure is a software
   emulation behind it (§5.1) [1] pp. 22–23.
3. **The 601's extended transfer protocols are not supported** by HMC or AMIC — a transfer
   that uses them raises a transfer error exception [1] p. 16. Only single-beat and four-beat
   32-byte cache-line transfers exist on this platform.

The 601 also supplies the reset entry point: with `MSR[IP]` set at power-on, the processor
fetches its first instruction from `$FFF00100`, inside the ROM's high alias (§2.7) [4].

### 2.3 The memory subsystem

The memory subsystem has three levels: the 601's 32 Kbit internal cache, an optional
second-level cache, and DRAM [1] §"Memory Organization" p. 22.

**DRAM.** Every board solders the first 8 MB of 80-ns DRAM and expands through 72-pin SIMMs
installed **in pairs** — the PowerPC data bus is 64 bits wide, the SIMMs store 32-bit words
[1] §"Random-Access Memory" p. 14; [2] §"RAM SIMMs" p. 9. The RAM system supports all 601
single-cycle transactions and the four-cycle cache-line transaction, and nothing else
[1] §"RAM Access" p. 22. Refresh is CAS-before-RAS every 15.6 µs [1] p. 22, with a
software-programmed rate divider set by the boot ROM from the measured bus clock [4] (full
detail in [hmc.md](hmc.md) §3.3, §4.4). The physical bank layout is the platform's single most
famous oddity: each SIMM pair decodes as a 64 MB bank in which only the middle 32 MB is usable,
so physical RAM is discontiguous with 32 MB gaps — a layout Apple documented specifically
because DMA NuBus cards see it untranslated (§3.2) [2] Appendix pp. 9–10.

**Second-level cache.** All models accept an external L2 cache on a 160-pin SIMM, 128 KB to
256 KB; the HMC senses the size through two connector pins at startup, and pull-ups on the
connector make the HMC disable external cache operations when no SIMM is installed
[1] §"Cache Memory" p. 15. The 8100/80 ships with its full 256 KB installed; the 8100/110 uses
a new cache SIMM design for the 36.6667 MHz bus [1] §"Cache Memory" p. 15; [2] §"New Cache"
p. 8. The cache is not snooped — the HMC "does not support… cache snooping" [1] §"High-Speed
Memory Controller" p. 15 — which is why DMA coherency is a driver obligation on this platform
(§5.2).

**ROM.** A 4 MB, 100-ns ROM SIMM [1] §"Read-Only Memory" p. 14, controlled by the HMC and —
unusually for a Macintosh — writable when the HMC's configuration register enables it: the
board carries a ROM write strobe, and the boot ROM probes for Intel-command-set flash memory
on the SIMM at every startup, falling back silently when it finds a mask ROM [3] sheet 4; [4]
(§3.3, [hmc.md](hmc.md) §3.8).

### 2.4 The I/O subsystem

The I/O side of the board belongs to the AMIC and its satellites [1] pp. 15–16:

- **Interrupts**: the 601's single interrupt input, two emulated VIA register banks, and a
  top-level interrupt control register that latches changes in the interrupt picture and drives
  the CPU interrupt line (§5.1; [amic.md](amic.md) §2.2–§2.4).
- **DMA**: channels for Ethernet (receive and transmit), SWIM III floppy, SCSI (one channel on
  the 6100/7100, two on the 8100), SCC (transmit and receive per port), and sound input and
  output — ten channels in total on the 8100 — all described in [amic.md](amic.md) §2.5, §3.3–§3.8.
- **Sound**: the serial sound engine that runs the 256-bit frame bus to the AWACS codec and
  DMAs sound data to and from RAM [1] pp. 46–48; [awacs.md](awacs.md) §3.
- **Video**: built-in video timing, dot-clock selection, sync and blanking generation, monitor
  sensing, and the RAM-fetch side of the video FIFOs — the framebuffer itself is ordinary DRAM
  at the bottom of the physical address space (§3.7) [1] p. 16; [ariel.md](ariel.md) §3.
- **Address decode**: AMIC receives the full 32-bit CPU address bus and decodes the whole I/O
  region internally; the peripheral chips see only chip selects and low-order buffered address
  lines [3] sheet 12; [amic.md](amic.md) §2.1.

The peripherals themselves are the **Curio** combo chip (SCC serial + 53C94-class SCSI + MACE
Ethernet [1] §"Curio I/O Chip" p. 16), the discrete 53CF96 fast SCSI controller of the 8100,
the **SWIM III** floppy controller [1] §"SWIM III Floppy Disk Drive Controller" p. 16, the
**Ariel II** RAMDAC [1] §"Ariel II Video Chip" p. 16, the **AWACS** codec, and the **Cuda**
microcontroller, which manages power, reset, parameter RAM, the ADB, the real-time clock, and
GeoPort wake-up [1] §"Cuda Microcontroller Chip" p. 17. Two **data path** chips bridge the
8/16-bit peripheral world to the 64-bit CPU bus and hold the video FIFOs [1] §"Data Path Chips"
p. 16; §4.9.

### 2.5 Endianness and data width

The platform is big-endian, full stop. The HMC "does not support… little-endian transfer
mode" [1] §"High-Speed Memory Controller" p. 15, and the 601 is run with its default
big-endian byte ordering throughout. NuBus byte order follows the classic Macintosh rule —
byte lane *n* of the processor maps to NuBus byte lane *n*, "only the bytes are swapped, not
bits within bytes" — so a little-endian NuBus card's longword `$12345678` reads as `$78563412`
[7] pp. 136–137; the slot-space detail is [bart.md](bart.md) §3.3. This is the structural
difference from the PCI-based generation that followed, where the host bridge carries
little-endian PCI memory space and the address map grows an endianness rule; on PDM there is
nothing to decide.

Register accesses across the whole I/O block are **byte-wide**; multi-byte quantities (DMA
addresses, counts) are transferred as consecutive byte accesses, most significant byte first,
and the AMIC's DMA window is physically constrained (256 KB-aligned base, small offsets)
because the channel registers are bytes [amic.md](amic.md) §2.5, §3.3.

### 2.6 Machine identity

The identity register is a 32-bit read-only register at **`$5FFFFFFC`**, the top of I/O space.
Apple documents its three low bits — 6100 `%000`, 7100 `%010`, 8100 `%011` [1] Table 1-5
p. 10 — and the full value reads `$A55A30xx`: an `$A55A` signature in the upper half and the
model code in the lower half, `$3010`/`$3012`/`$3013` as shipped [4], [5]. The register must
support **byte** reads: the ROM's hardware-init code reads bytes 2 and 3 individually and
compares the assembled halfword against `$3012`/`$3013`, treating everything else — including
every 6100's `$3010` — as the 6100 case [4]. The register-level detail (reset value,
`$A55A` signature) is [hmc.md](hmc.md) §2.5, whose register it is.

One identity value is **software-only and trips up every reader of the register**: `$3011`.
The 68k machine-identification routine reads `$3010` from a 6100's register, probes the AMIC
revision, and *promotes* the answer to `$3011` before matching the ROM's product records —
production 6100s never answer `$3011` from hardware, and code that tests the raw register value
against the product table gets the wrong answer [4] (promotion verified at ROM file offset
`$10070`). The 1995 ROM adds a fourth product record, `$3014`, for an as-yet unidentified
member of the family [5]; see §7.

On top of the register, the ROM synthesises the Gestalt machine type (the `$4B`/`$70`/`$41`
values of Table 1-5) from the machine-ID low bits plus the *measured* CPU clock — one identity
column per board, one row per speed grade, so a single ROM covers every clock variant [4].
The AV variants are not distinguished by any of these registers: the AV card is a PDS card
whose declaration ROM the Slot Manager reads, not a different motherboard (§6.1–§6.3).

### 2.7 The ROM image and the boot contract

The 4 MB image decomposes into five components, laid out in file order [4], [5]:

| Image offset | Size | Component |
|---|---|---|
| +$000000 | 3 MB | the 68k Mac OS ROM: header (checksum longword at +0, ROM version word `$077D` at +8), the Toolbox, the Universal tables, the drivers |
| +$300000 | 48 KB | PowerPC exception-vector stubs and **HWInit**, the hardware bring-up routine; the hard-reset vector at `$FFF00100` branches to HWInit at `$FFF03000` |
| +$30C000 | 4 KB+ | **ConfigInfo**, the kernel's configuration record (below) |
| +$310000 | 64 KB | the **nanokernel** (the ROM identifies itself as "PDM 601 1.0" / "1.1") |
| +$320000 | 64 KB | the **68k emulator** code (entry offset `$E0F8` old ROM, `$E104` new ROM) |
| +$380000 | 512 KB | the emulator's **opcode dispatch table** |

The image must decode at least at the aliases the software actually uses: repeated every 4 MB
across `$40000000–$4FFFFFFF` (HWInit deliberately re-bases itself to run at `$40300000`; the
operating system runs the 68k ROM at `$40800000`) and at the high alias `$FFC00000–$FFFFFFFF`,
where the reset fetch happens [4].

**ConfigInfo** is the contract between the ROM's components. Key fields, with their shipped
values [4], [5]:

| Field | Old ROM | New ROM | Meaning |
|---|---|---|---|
| ROM base/size | `$FFCF4000` / `$00400000` | same | locates the whole image |
| Kernel code offset/size | +$4000 / 64 KB | same | the nanokernel, at `$FFF10000` |
| Emulator code offset/size | +$14000 / 64 KB | same | at `$FFF20000` |
| Opcode table offset/size | +$74000 / 512 KB | same | at `$FFF80000` |
| BootstrapVersion | "Boot PDM 601 1.0" | "Boot PDM 601 1.1" | the version string |
| Emulator entry offset | `$E0F8` | `$E104` | within the emulator code block |
| KernelTrapTable offset | `$E140` | `$E140` | the kernel-call return trap, inside the emulator block |
| Test/Clear/Post interrupt masks | `$00200000`/`$FF9FFFFF`/`$00E00000` | same | CR-bit handshake between kernel and emulator |
| `LA_InterruptCtl` | `$50F2A000` | same | the AMIC master interrupt register |
| `InterruptHandlerKind` | 1 | same | selects the PDM external-interrupt handler |
| `LA_InfoRecord` | `$5FFFE000` | same | the info-record page |
| `LA_KernelData` / `LA_EmulatorData` | `$68FFE000` / `$68FFF000` | same | kernel and emulator data pages |
| `LA_DispatchTable` / `LA_EmulatorCode` | `$68080000` / `$68020000` | same | the logical windows for §3.6 |
| PageAttributeInit | `$00000002` | same | the default PTE attribute word |

**The boot sequence**, shared by all three machines, runs in four phases [4], [5]:

1. **HWInit** (PowerPC, at `$FFF03000`, fetched through `$FFF00100`): verify the ROM checksum
   (summing the image in eight byte lanes plus a 64-bit doubleword sum), re-base to the
   `$40300000` alias, install three forced-I/O segment registers for untranslated access to
   RAM and I/O, quiesce the platform (soft-reset every AMIC DMA channel, stop both sound DMA
   controls, clear both pseudo-VIA2 interrupt enables, blank video), measure the CPU clock
   against the decrementer and snap it to a table of standard frequencies, measure the CPU:bus
   ratio by timing loads with and without an HMC wait state, program the HMC's 35-bit serial
   configuration register (timing, refresh divider, bank sizes — [hmc.md](hmc.md) §2, §4),
   read the machine ID, probe the RAM windows, encode the SIMM bank sizes back into the HMC,
   probe for flash on the ROM SIMM, size the L2 cache, and jump to the nanokernel with pointers
   to ConfigInfo, a ProcessorInfo record and a SystemInfo record.
2. **Nanokernel init**: trim bank 0 so logical zero relocates to the kernel's low-memory
   mirror, steal the top of a RAM bank for the page tables and the kernel/emulator data pages,
   build the page tables from ConfigInfo's segment and BAT maps, and start the 68k emulator at
   the recorded entry offset with the 68k ROM mapped at `$40800000`.
3. **The 68k emulator + Start Manager**: the classic Macintosh boot — machine
   identification through the Universal tables (reading `$5FFFFFFC`, promoting `$3010` to
   `$3011`), Gestalt box-flag synthesis, interrupt disable of every AMIC source, the Cuda
   handshake and PRAM read, the memory split ([ariel.md](ariel.md) §4.3 for the framebuffer
   carve-out, [amic.md](amic.md) §3.3 for the DMA buffer), the boot beep ([awacs.md](awacs.md)
   §4.2), built-in video bring-up, the Slot Manager's NuBus pass ([bart.md](bart.md) §4.2–§4.3),
   and SCSI/floppy probe and boot-device selection.
4. **System handoff**: the System file's 'boot' resources load the machine-support components
   and may replace the ROM's nanokernel with a newer image from disk [4].

The ROM also carries its native PowerPC half inside the "68k" 3 MB: a run of PEF code
fragments (the boot-time CFM scanner, Mixed Mode, InterfaceLib and the other native libraries)
indexed by a table of contents inside the image; 68k↔native transitions go through a kernel
call that the emulator dispatches from an F-line opcode [4]. The pragmatic consequence for a
re-implementer: a PowerPC interrupt/exception architecture faithful to the 601 and to
[6] is mandatory — the ROM's kernel runs on the real exception model, not on an emulator
convention.

**Reset and warm boot.** A 68k `RESET` instruction or a restart re-enters HWInit with
translation on; HWInit detects the warm state, locates the ROM image through the live page
tables, copies itself to RAM and re-runs everything — checksum, HMC configuration and the RAM
probe included [4]. The warm path is only reachable through the running kernel's page tables: a
synthetic jump to the reset vector with no resident ROM mappings checksums garbage and takes
the failure path (§5.5) (*observed*).

### 2.8 Reset and power

The Cuda microcontroller owns the power-on sequence: it turns system power on and off, manages
resets from external and software commands, keeps parameter RAM, drives the ADB and the
real-time clock, and can power the machine up from a signal on either GeoPort [1] §"Cuda
Microcontroller Chip" p. 17. On power-up Cuda releases the system reset, which fans out
through AMIC to every peripheral; the programmer's-switch NMI enters through a dedicated Cuda
line and surfaces as level 7 (§5.1) [3] sheet 12; [amic.md](amic.md) §2.4. The transport
between Cuda and the processor environment is register emulation: no physical 6522 exists —
the five Cuda wires land on AMIC pins that implement the VIA1 shift-register handshake
[3] sheets 12, 16; [amic.md](amic.md) §1.

## 3. Memory map & address decode shared by the family

### 3.1 The physical map

The platform has a single 32-bit physical address space with the allocations of Table 2-5
[1] §"Physical Memory Allocations" p. 22, refined by the decode the ROM's own tables carry
[4], [5]:

| Physical range | Contents |
|---|---|
| `$00000000–$0FFFFFFF` | RAM (256 MB architectural window; real RAM extends to `$1EFFFFFF` on a full 8100 — §3.2) |
| `$10000000–$3FFFFFFF` | RAM aliases (the simple-configuration view; a full 8100 uses `$10000000–$1EFFFFFF` for real RAM) |
| `$40000000–$4FFFFFFF` | ROM, the 4 MB image repeated every 4 MB (§3.3) |
| `$50000000–$5FFFFFFF` | I/O, decoded internally by AMIC (§3.4); the machine-ID register at the very top |
| `$60000000–$7FFFFFFF` | not assigned |
| `$80000000–$8FFFFFFF` | major slot $8 — addressable only with the 1995 ROM [2] p. 5 |
| `$90000000–$EFFFFFFF` | major (super) slot space, slots $9–$E, 256 MB per slot [7] p. 133 |
| `$F0000000–$F00000xx` | BART's register window, inside slot $0's standard space ([bart.md](bart.md) §2) |
| `$F1000000–$FEFFFFFF` | minor (standard) slot space, 16 MB per slot; old ROM slots $9–$E, new ROM slots $1–$E [2] p. 5 |
| `$FF000000–$FFFFFFFF` | ROM alias: the image at `$FFC00000`, reset fetch at `$FFF00100` |

Apple's Table 2-5 labels everything from `$60000000` to `$FEFFFFFF` "not assigned"; the slot
windows within it are real decode — the ROM's decoder tables carry their bases, and the
Slot Manager walks them — but the developer note simply does not break them out
([bart.md](bart.md) §3.1 gives the slot-space detail with citations). Two error contracts
bound the map: an address the MMU does not map raises a recoverable MMU error, while an
address the MMU *maps* but no hardware decodes makes AMIC assert an error signal after **40 µs**,
"not generally recoverable; forces the user to restart the computer" [1] §"Address Errors"
p. 21.

### 3.2 The DRAM bank layout

The 8 MB of soldered DRAM sits at `$00000000–$007FFFFF`. Each SIMM pair then decodes as one
64 MB bank in which only the middle 32 MB is usable, so bank *n* occupies a 32 MB window
starting 16 MB into a 64 MB stride: banks at `$01000000`, `$05000000`, `$09000000`,
`$0D000000`, `$11000000`, `$15000000`, `$19000000`, `$1D000000` — stride `$04000000` [2]
Appendix, Table A-2 p. 10. The 8100, with four SIMM pairs, is the only machine that can fill
all eight windows; its maximum is 264 MB (8 MB + 4 × 64 MB usable) reaching `$1EFFFFFF`, the
7100 manages two pairs (136 MB), the 6100 one (72 MB) [2] Table A-1 p. 9.

Two platform behaviours live on top of the layout. **At power-on the HMC decodes the banks in
their fixed windows and aliases undersized SIMMs inside them**; the boot ROM probes the
windows (a 2 MB stepping pattern walk that detects aliasing) and, for the 6100's single pair,
writes a SIMM-size code into the HMC that rebases the pair contiguously after the soldered
RAM — the 7100/8100 banks never move [4]. The probe windows are model-dependent from the
machine ID alone: three windows on the 6100 path, the soldered window plus the eight Table A-2
windows on the 7100/8100 path [4]. **The gap structure is DMA-visible**: NuBus masters reach
RAM through BART with no translation (there is no IOMMU), so a DMA card that assumes
contiguous physical RAM walks into the 32 MB gaps — the reason the Enhanced developer note
dedicates its RAM appendix to card designers [2] pp. 9–10; [bart.md](bart.md) §3.2.

The operating system's page tables, not the HMC, make this discontiguous physical map into
the contiguous logical RAM the 68k environment sees (§3.6); the ROM's own platform record
declares the whole physical window as one region and leaves the bank map to the memory
descriptors it builds at boot [4].

### 3.3 The ROM decode

The 4 MB image is decoded throughout `$40000000–$4FFFFFFF` — repeated every 4 MB — and at
`$FFC00000–$FFFFFFFF` in the high alias [4]. Three particular aliases are load-bearing and
must all work: `$FFF00100` (the 601 reset fetch), `$40300000` (where HWInit re-bases itself)
and `$40800000` (the "canonical" base the ROM's decoder record names, at which the operating
system runs the 68k ROM) [4]. The choice of `$40800000` is not arbitrary: at `$40800000` the
image's high addresses alias into the 24-bit slot-space view in a way that keeps classic
24-bit-era addressing conventions working [4].

The ROM SIMM is flash-capable in hardware — the board carries the write strobe — and HWInit
probes for Intel-command-set flash at every startup by writing the read-ID command and
comparing two reads; a mask ROM simply returns array data and the probe records "no flash"
[3] sheet 4; [4]. The HMC controls the SIMM's enables and two extra address lines
([hmc.md](hmc.md) §3.8).

### 3.4 The I/O block

The I/O region `$50000000–$5FFFFFFF` is decoded entirely inside AMIC. The useful live block
is the 256 KB at **`$50F00000`**, which the system maps cache-inhibited; every device on the
board answers inside it at a fixed base [4], [5]; [amic.md](amic.md) §2.1 owns the decode
detail. The bases, shared by every machine in the family:

| Base | Device / register block | Detail |
|---|---|---|
| `$50F00000` | pseudo-VIA1 (classic VIA register layout, stride `$200`) | [amic.md](amic.md) §2.2 |
| `$50F04000` | SCC (the ESCC cell of Curio): control B/A, data B/A at +0/+2/+4/+6 | [scc.md](../../hardware/scc.md) |
| `$50F08000` | Ethernet ID PROM (32 bytes; MAC address, odd-byte-lane reads) | [mace.md](../av/mace.md) §2 |
| `$50F0A000` | MACE Ethernet registers (16-byte stride) | [mace.md](../av/mace.md) §2 |
| `$50F10000` | SCSI channel A — the 53C94-class cell of Curio (16-byte stride) | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §2 |
| `$50F11000` | SCSI channel B — 53CF96 fast SCSI, **8100 only** | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §1 |
| `$50F14000` | sound block: AWACS codec command port + sound DMA control (`$20` bytes) | [awacs.md](awacs.md) §2.1 |
| `$50F16000` | SWIM III floppy controller (512-byte register stride) | [swim3.md](swim3.md) §2.1 |
| `$50F24000` | Ariel II RAMDAC (four byte registers) | [ariel.md](ariel.md) §2.1 |
| `$50F26000` | pseudo-VIA2 interrupt bank (slot flags, device flags, enables) | [amic.md](amic.md) §2.3 |
| `$50F28000` | AMIC video control (mode, depth, monitor sense, beam counters) | [ariel.md](ariel.md) §2.4 |
| `$50F2A000` | interrupt control register + DMA flag registers | [amic.md](amic.md) §2.4 |
| `$50F2C000` | diagnostic register | [amic.md](amic.md) §2.8 |
| `$50F31000` | AMIC DMA register file (window base, channel controls, pointers, counts) | [amic.md](amic.md) §2.5 |
| `$50F40000` | HMC serial configuration port (35-bit, one bit per access) | [hmc.md](hmc.md) §2.1 |
| `$5FFFFFFC` | machine-ID register | [hmc.md](hmc.md) §2.5 |

### 3.5 The slot windows

The two NuBus address windows — minor (standard) slot space `$Fs000000`–`$FsFFFFFF`, 16 MB
per slot, and major (super) slot space `$s0000000`–`$sFFFFFFF`, 256 MB per slot — and BART's
register window at the bottom of slot $0's space are described in [bart.md](bart.md) §3.1,
which also reconciles Apple's two naming schemes for them. What is family-level is the
population: the 6100 answers one real slot, `$E`, and only when the adapter card is
installed; the 7100 and 8100 answer slots `$B`, `$C` and `$D` on their three connectors, with
slot `$E` owned by the PDS video card rather than BART — the boot ROM disables BART's slot-$E
path on exactly the machines whose PDS claims it [4]; [bart.md](bart.md) §1.4, §2.5. The 1995
ROM extends minor-slot addressing to slots `$1–$E` and makes major slot $8 addressable
[2] p. 5.

### 3.6 The logical (MMU) map

The logical address map is built by the nanokernel from ConfigInfo at every boot; the
fixed windows it installs, identical on all three machines [4], [5]:

| Logical | Contents |
|---|---|
| `$00000000` | 68k low memory and RAM: the contiguous logical view of the discontiguous physical banks, offset by the framebuffer carve-out at physical 0 (§3.7) — logical zero lands on the kernel's relocated low-memory mirror |
| `$40310000` | nanokernel code window (kernel PCs are `$4031xxxx`, distinct from the ROM's `$40800000`) |
| `$40800000` | the 68k ROM (identity to the physical alias) |
| `$50F00000` | the I/O block, identity-mapped, cache-inhibited |
| `$5FFFE000` | the info-record page (system/processor/diagnostic info pointers the ROM's native code and the OS read) |
| `$60B00000` | the built-in framebuffer, 604 KB, write-through (§3.7) |
| `$61000000` | the AMIC DMA buffer, 160 KB, cache-inhibited (§3.7) |
| `$68020000` | the 68k emulator code (physical source: ROM offset `$320000`); the kernel-call trap table at +$E140, i.e. `$6802E140` |
| `$68080000` | the emulator's opcode dispatch table (physical source: ROM offset `$380000`) |
| `$68FFE000` | the kernel data page (KDP), near the top of RAM |
| `$68FFF000` | the emulator data page (EDP): the pending-interrupt level, the emulator context block, the bootstrap version |

The kernel builds these from ConfigInfo's four 32-entry segment maps and BAT ranges with a
default PTE attribute word of `$00000002` [4]. Before the kernel runs, HWInit's own temporary
mapping covers physical RAM and I/O through three forced-I/O segment registers and one
instruction BAT for the ROM block [4]; the MMU model required is the 601's, per [6].

A re-implementation note that falls out of the carve-out at physical zero: the 68k logical RAM
view is *not* identity even before the MMU plays tricks — with the 604 KB framebuffer
allocated, every 68k logical address maps to physical address + `$97000` (*observed* on a
booted 24 MB configuration: low-memory globals, heap and stack all shifted by exactly that
amount).

### 3.7 The framebuffer and the DMA buffer

Two special allocations shape the map shared by every machine:

**The built-in framebuffer is ordinary DRAM at physical address `$00000000`**, with no
programmable fetch-base register anywhere on the board. The Start Manager asks the nanokernel
for the allocation with an alignment argument that forces physical zero, sizes it at 604 KB
(the largest of the five built-in video modes plus headroom), maps it write-through at logical
`$60B00000`, and allocates it only when a monitor is sensed on the built-in port (or a
factory burn-in PRAM signature says otherwise) [4]. Which of the two possible scan bases the
HMC uses is a single bit of its 35-bit configuration register — the ROM's constant state
selects physical `$0`; MkLinux runs the same machines with the bit clear and scans from
`$100000` [4], [8]. The video pipeline itself — AMIC timing, data path FIFO bursts at
arbitration priority just below DRAM refresh, Ariel II DAC — is [ariel.md](ariel.md) §3.

**The AMIC DMA buffer is one physically contiguous 160 KB window**, 256 KB-aligned, mapped
cache-inhibited at logical `$61000000`, and *every* DMA channel except the two SCSI channels
addresses its buffers as offsets inside it — the layout (Ethernet ring, four sound buffers,
transmit buffers, four SCC rings) and the reason the window must be cache-inhibited are
[amic.md](amic.md) §3.3. At cold boot the ROM points the window at physical zero and plays
the startup chime out of it before the Start Manager's allocation exists [4]; [amic.md](amic.md)
§3.3.

## 4. Device roster

| Part | Function | Page |
|---|---|---|
| HMC ("Half A Memory Controller") | DRAM, ROM, L2 cache, 601 bus protocol, address-bus arbitration, machine ID | [hmc.md](hmc.md) |
| AMIC | I/O decode, pseudo-VIAs, interrupt control, 10-channel DMA, sound engine, video timing | [amic.md](amic.md) |
| BART / BART 21 | the NuBus '90 bridge and its register window | [bart.md](bart.md) |
| AWACS | the sound codec on the AMIC frame bus | [awacs.md](awacs.md) |
| Ariel II | the video RAMDAC (CLUT + DAC) | [ariel.md](ariel.md) |
| SWIM III | the floppy controller | [swim3.md](swim3.md) |
| Curio | combo chip: SCC serial + 53C94-class SCSI + MACE Ethernet | [scc.md](../../hardware/scc.md), [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md), [mace.md](../av/mace.md) |
| 53CF96 | fast SCSI controller, 8100 only | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §1 |
| Cuda | power/reset/PRAM/ADB/RTC microcontroller | [cuda.md](../av/cuda.md) |
| Data path chips (×2) | 64↔16-bit bus bridging, video FIFOs | [amic.md](amic.md) §1 |
| Squidlet / ICS9178 | clock generation | §5.4 of this page |
| NuBus slots, declaration ROMs | the expansion fabric | [nubus.md](../../hardware/nubus/nubus.md), [declaration-rom.md](../../hardware/nubus/declaration-rom.md) |

Each device's page is the authority for its part; the subsections below give only the
family-level placement.

### 4.1 The memory controller: HMC

One ASIC owns the north side of every board in the family (§2.3). Its single software-visible
programming interface is a 35-bit configuration register shifted bit-serially through a port
at `$50F40000` inside the AMIC decode — a write strobe at `$50F40008` followed by 35 single-bit
accesses — carrying DRAM timing, the refresh divider, bank sizes, L2 configuration, the video
scan-base bit and the machine's speed-grade strap [hmc.md](hmc.md) §2. It also exposes the
machine-ID register (§2.6). The HMC generates no interrupts of its own.

### 4.2 The I/O controller: AMIC

The 160-pin gate array that makes the platform feel like a Macintosh: all I/O decode
(§3.4), the emulated VIA1 and the slot/device interrupt bank, the top-level interrupt
control register that drives the 601's one interrupt input, the ten DMA channels, the sound
engine's register window, the video timing block, and the Cuda transport emulation
[amic.md](amic.md) §1, §2. Three silicon revisions matter to software and all are detectable
through register behaviour; every shipped board is revision 3 or later [4]; [amic.md](amic.md)
§1 ("Silicon revisions").

### 4.3 The NuBus bridge: BART

The only bridge between the CPU bus and NuBus '90: a CPU-bus slave for slot-space
transactions, a CPU-bus master for NuBus cards mastering into RAM/ROM, and the owner of the
`$F0000000` register window (reset pulse, wait-state bit, ID register, slot-$E mask, per-slot
burst enables) [bart.md](bart.md) §1–§3. The 8100/100 and 8100/110 carry the second
generation, **BART 21**, which honours the per-slot burst enables and handles burst reads by
NuBus masters [2] p. 4; [bart.md](bart.md) §3.7.

### 4.4 Sound and video: AWACS and Ariel II

The sound path is a pair: the **AWACS** codec (gain, attenuation, input-port sensing) sits on
the serial frame bus AMIC runs, and AMIC's DMA engine moves the data — the register window at
`$50F14000` interleaves both halves [awacs.md](awacs.md) §2.1. The video path is the same
shape with the roles reversed: AMIC owns timing and fetch ([ariel.md](ariel.md) §2.4 for the
`$50F28000` control block), the **Ariel II** chip owns the CLUT and the DAC ([ariel.md](ariel.md)
§2.2), and the framebuffer is system DRAM (§3.7). The AV machines replace this path with the
AV card on the PDS slot (§6.1–§6.3).

### 4.5 The Curio combo chip: SCC, SCSI and MACE

Curio packs the three communications cells on one die: an SCC serial controller with 8-byte
FIFOs per direction [1] §"Curio I/O Chip" p. 16, a 53C94-class SCSI controller (the 6100's and
7100's only SCSI engine; the 8100's standard-speed channel), and the MACE Ethernet controller.
Each cell has its own page — [scc.md](../../hardware/scc.md) for the ESCC,
[ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) for the SCSI cell and the discrete 53CF96,
[mace.md](../av/mace.md) for the Ethernet — and AMIC supplies each with a DMA channel and,
for the SCC and MACE, an interrupt position (§5.1).

### 4.6 The floppy controller: SWIM III

The third generation of Apple's floppy controller, an extension of the SWIM II with DMA
support, no need to disable interrupts during transfers, GCR and MFM on 1.44 MB media, and
manual-inject drive support [1] §"SWIM III Floppy Disk Drive Controller" p. 16; register and
behaviour detail in [swim3.md](swim3.md) §2–§3. Its chip interrupt lands in the pseudo-VIA2
device bank at level 2, its DMA completion in the DMA flag registers at level 4 (§5.1).

### 4.7 The system microcontroller: Cuda

The always-on MCU that owns power, reset, parameter RAM, the ADB and the real-time clock
(§2.8). Transport, packet protocol and PRAM/ADB behaviour are [cuda.md](../av/cuda.md) §2–§4;
on PDM the five-wire transport is register-emulated by AMIC's pseudo-VIA1 rather than a
physical 6522 [amic.md](amic.md) §2.2.

### 4.8 Clock generation: Squidlet and ICS9178

Two clock-generator generations serve the family and are described in §5.4: the Apple
"Squidlet" on the 1994 boards and the commercial ICS9178 synthesizer on the 8100/110
[1] §"Squidlet Chip" p. 17; [2] p. 8; [3] sheet 11. Neither is software-visible — there are
no programmable clock registers on the platform — so neither has a device page beyond this
section.

### 4.9 The data path chips

Two data path chips buffer between the 8/16-bit peripheral world and the 64-bit cached CPU
bus: they route byte lanes for I/O and DMA data and hold the FIFOs that video refresh fills
from RAM in eight-beat bursts [1] §"Data Path Chips" p. 16. They have no software-visible
registers; their architectural consequence — why the DMA buffer pages are mapped
cache-inhibited and what the byte-assembly path means for drivers — is covered with the DMA
window in [amic.md](amic.md) §3.3.

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupts: one collector, two pseudo-VIAs

The 601 has a single, level-sensitive interrupt input. AMIC concentrates every source behind
it in three register layers [1] pp. 22–23, Figure 2-2:

1. **Pseudo-VIA1** (`$50F00000`, classic VIA layout at a `$200` stride): the 60.15 Hz tick,
   the Cuda one-second line, the Cuda transport handshake, and the two emulated VIA timers —
   level 1 of the classic model [amic.md](amic.md) §2.2.
2. **Pseudo-VIA2** (`$50F26000`): two banks — a slot bank (the three NuBus slots, the PDS
   slot, and the built-in video vertical blanking, all active-low live levels) and a device
   bank (SCSI interrupt and DRQ lines, the SWIM III interrupt, and the any-slot aggregate) —
   level 2 [amic.md](amic.md) §2.3. The slot-bit to slot-number mapping, and the fact that
   BART is *not* in the slot-interrupt path (each connector's line runs from the slot to an
   AMIC pin), is [bart.md](bart.md) §3.6.
3. **The interrupt control register** (`$50F2A000`): a summary register whose low six bits
   mirror the VIA1, VIA2, SCC, Ethernet, DMA and NMI pictures, whose bit 6 selects the latch
   mode, and whose bit 7 is the CPU-interrupt latch — write-one-to-clear — that drives the
   601's INT pin [amic.md](amic.md) §2.4.

On every external interrupt the ROM's nanokernel acknowledges the latch (write `$C0`, keep
the mode bit), reads the summary, maps the source bits through a 64-entry table to a classic
68k interrupt level, posts that level into the emulator's pending-interrupt variable, and
returns; the 68k emulator then takes the autovector when the emulated status register allows
[amic.md](amic.md) §3.2. The installed levels [4]:

| 68k level | Sources |
|---|---|
| 1 | pseudo-VIA1: 60.15 Hz tick, one-second, Cuda transport, emulated timers |
| 2 | pseudo-VIA2: SCSI-A/B interrupt and DRQ, SWIM III interrupt, slot lines, built-in VBL |
| 3 | MACE Ethernet chip interrupt (the only source; dispatched unconditionally) |
| 4 | SCC chip interrupt plus all ten DMA channels' completion flags |
| 5, 6 | unused |
| 7 | NMI — the programmer's switch, arriving through Cuda |

The latch is **change-driven**: any change in the source picture — assertion or deassertion —
sets it, and a merely-asserted source does not re-latch after the acknowledge; the
service-until-clear behaviour the classic OS relies on is carried by the emulator's posted
level, not by the line (*observed*; both failure modes of getting this wrong hang the boot —
[amic.md](amic.md) §3.1 documents the two livelocks). At boot, HWInit and then the 68k ROM
each disable every enable bit in turn before any driver installs a handler [4]; [amic.md](amic.md)
§4.1.

### 5.2 The CPU bus and its arbitration

The CPU bus is the 601's standard 64-bit nonpipelined bus. The HMC arbitrates **the address
part** of it in a fixed priority order, with the processor holding default access; access to
the data part follows the address part [1] §"CPU Bus Arbitration" p. 20:

| Priority | Requestor |
|---|---|
| highest | DRAM refresh |
| | video refresh |
| | I/O DMA for SWIM III |
| | I/O DMA for AWAC (sound) |
| | I/O DMA for SCSI |
| | I/O DMA for SCC |
| | expansion card |
| lowest | main processor |

The expansion-card row is the PDS: a card can master the CPU bus subject to Apple's published
rules — it must check that the bus is free, suffers a one-CPU-cycle latency on original
access, is recognised only for basic cycles and cache-line operations, must decode its own
space, and must expect video refresh to absorb much of the bus capacity at times [1]
§"CPU Bus Arbitration" p. 21. The Ethernet DMA channels are absent from Apple's table; their
arbitration position is unknown (§7) — [amic.md](amic.md) §1 notes the same gap. The
request/grant wiring, and BART's and the PDS's paths into the arbiter, are visible on the
8100 schematic [3] sheets 4, 23 and summarised in [hmc.md](hmc.md) §3.5.

Because the HMC does not snoop the caches, DMA coherency is entirely a software matter: the
system keeps DMA buffers write-through or cache-inhibited (§3.7) and drivers must not leave
dirty copy-back cache over a buffer a device reads or writes [1] p. 15; [amic.md](amic.md)
§3.3.

### 5.3 Bus errors and unsupported transfers

Three failure contracts bound every bus transaction on the platform [1] pp. 16, 21:

1. **MMU-unmapped addresses** raise the 601's translation exceptions — recoverable, and the
   normal path (the ROM's kernel installs page-table entries lazily; first-touch faults are
   ambient, healthy traffic) [4].
2. **MMU-mapped but undecoded addresses** make AMIC assert an error signal after **40 µs**;
   Apple documents this as not generally recoverable. The one family of exceptions is
   BART-claimed space, where an empty slot must fault through the recoverable machine-check
   path instead — the presence probe, the early video-card probe and the Slot Manager's
   declaration-ROM search all depend on it ([bart.md](bart.md) §3.5).
3. **Extended 601 transfer protocols** (anything beyond single-beat and four-beat cache-line
   transfers) raise a transfer error exception from HMC or AMIC [1] p. 16.

Misaligned read actions are translated by the HMC into double-word read actions, and
address-only transactions are implemented [1] §"High-Speed Memory Controller" p. 15;
transaction-level detail is [hmc.md](hmc.md) §3.4.

### 5.4 The clock architecture

Two clock-generator designs serve the family, and neither is programmable: clock
configuration is straps and PLLs, invisible to software [1] §"Squidlet Chip" p. 17; [2]
§"Clock Chips" p. 5. The 1994 boards use **Squidlet**, "a 28-pin chip that provides a set of
synchronized system clocks" [1] §"Squidlet Chip" p. 17; the 8100/110 "uses a new clock chip"
[2] §"Clock Speeds" p. 8, identified on the 8100/110 schematic as a commercial ICS9178
synthesizer fed by a 36.6667 MHz reference oscillator [3] sheet 11. The model-dependent
clocks [1] Table 2-2 p. 19; [2] Table 1-2 p. 3:

| Model | CPU (MHz) | Bus (MHz) | CPU:bus |
|---|---|---|---|
| 6100/60 (+AV) | 60 | 30 | 2:1 |
| 6100/66 | 66 | 33 | 2:1 |
| 7100/66 (+AV) | 66 | 33 | 2:1 |
| 7100/80 | 80 | 40 | 2:1 |
| 8100/80 (+AV) | 80 | 40 | 2:1 |
| 8100/100 | 100 | 33.3333 | 3:1 |
| 8100/110 | 110 | 36.6667 | 3:1 |

The fixed oscillators [1] Table 2-3 p. 20, as the 8100/110 schematic wires them [3] sheet 11:

| Oscillator | Frequency | Consumers |
|---|---|---|
| I/O | 31.3344 MHz | AMIC; ÷2 = the 15.6672 MHz Curio PCLK and 12" monitor dot clock; ÷4 = the 7.8336 MHz 601 RTC input |
| VGA | 25.175 MHz | AMIC; the VGA-mode dot clock |
| Dot | 57.2832 MHz | AMIC; the 16" and portrait-mode dot clock |
| Ethernet/SCSI | 40 MHz | ÷2 = 20 MHz for Curio's SCSI cell and MACE; 40 MHz direct to the 53CF96 (8100) and to BART's processor-side logic |
| Sound | 45.1584 MHz | AMIC's sound engine; ÷1024 = exactly 44.1 kHz (the developer note's Table 2-3 prints 44.1584, which cannot produce 44.1 kHz — a typo; the schematic prints 45.1584 [3] sheet 11) (*inferred*) |
| Cuda | 32.768 kHz | the Cuda MCU's real-time clock |

The software-visible consequences of the tree are few and exact [4]:

- The **601 RTC/decrementer input is 7.8336 MHz** (31.3344 ÷4). The boot ROM hard-codes
  1,002,700,800 RTCL-units per second into the processor info record and *measures* the CPU
  clock against the decrementer (a 1024-iteration dependent-add loop), snapping the result to
  a table of standard frequencies within ±1/1024 — 60, 66, 80 MHz and so on — rather than
  trusting a per-model table. The bus ratio is then measured by timing loads with and without
  an HMC wait state. A machine whose RTC rate is off reports wobbling CPU/bus speeds and
  picks the wrong HMC timing entry; the RTC is the one trusted clock in the system [4];
  [hmc.md](hmc.md) §4.4–§4.5.
- The **60.15 Hz tick** (pseudo-VIA1 bit 1) is generated inside AMIC, at the classic
  Macintosh vertical-retrace rate; the built-in video VBL (pseudo-VIA2 slot bank bit 6) fires
  at the actual refresh rate of the selected monitor mode — 60.15 to 75 Hz per mode
  [1] Figure 2-2 p. 23, Table 3-10 p. 39; [ariel.md](ariel.md) §3.6.
- The **emulated VIA timers** are expected to run at the classic 783.36 kHz VIA rate
  (7.8336 MHz ÷10), preserving Time Manager compatibility; ROM-side confirmation of the exact
  rate is still pending (*inferred — unverified*; §7) [amic.md](amic.md) §3.2.
- The **SCC baud clocks** come from the 15.6672 MHz Curio PCLK domain, with AMIC
  synthesising 3.672 MHz onto the RTxC lines; the ROM's serial driver constants name both
  rates [4].
- The **sound frame rate** is 45.1584 MHz ÷1024 = 44.1 kHz exactly, with 22.05 kHz and
  29.4 kHz available through the sound control register's divider field [4]; [awacs.md](awacs.md)
  §3.4.

### 5.5 Reset distribution and the warm path

Power-on reset comes from Cuda (§2.8) and fans out through AMIC to every peripheral; at
release the 601 fetches from `$FFF00100` and HWInit runs (§2.7) [4]. A running system can
re-enter the same code two ways. A **soft restart** (the 68k `RESET` instruction or a restart
request) re-enters HWInit with translation on, which relocates itself to RAM, re-verifies the
ROM checksum, re-programs the HMC and re-runs the RAM probe — but does *not* re-assert the
NuBus reset line in hardware, which is why the boot ROM pulses NuBus reset in software on
every start ([bart.md](bart.md) §2.2, §4.2). A **checksum failure** is not fatal either: HWInit
flags the bad ROM in the HMC configuration, pokes the Ariel II CLUT as a screen indication,
hammers low RAM, and *continues booting* [4]; [hmc.md](hmc.md) §4.8. Diagnostic failure is
polite in the same spirit: the boot ROM checks the AMIC diagnostic register and the
nanokernel's diagnostic record, and detours to a monitor or an alert instead of booting when
they record faults [4].

## 6. Per-machine index

The machine pages carry the per-box deltas; this section is the index. All three machines
share the ASIC set, the address map, the interrupt model and the boot sequence of §2–§5 —
the rows below are what differs.

### 6.1 Power Macintosh 6100 — [pm6100.md](pm6100.md)

The smallest box of the family (16.3 × 14.8 × 3.2 inches [1] Table 2-1 p. 12) and the one the
platform is named after: machine ID `$A55A3010`, Gestalt `$4B`, one SIMM pair, 72 MB maximum
[1] Table 1-5 p. 10; [2] Table A-1 p. 9. It omits the fast internal SCSI bus entirely — one
Curio SCSI channel only [1] §"System Architecture" p. 13 — and it carries no NuBus controller
on the board: BART and the single NuBus connector (slot `$E`) live on the optional PDS
adapter card, so a 6100 without the adapter has no NuBus at all and a 6100 *with* it has
exactly one short-card slot [1] §"NuBus Cards for the Power Macintosh 6100/60" p. 52;
[bart.md](bart.md) §1.2. The same PDS carries the AV card on the 6100/60AV. The 6100/66 is
the Enhanced refresh at 66/33 MHz; it keeps the original ROM and therefore original NuBus
semantics — write-through slot cacheability only [2] Table 1-3 p. 5.

The 6100's identity quirk is the `$3011` promotion of §2.6, and its boot quirk is the RAM
probe: the ROM probes three windows for the single SIMM pair rather than the eight of
Table A-2, and rebases the pair contiguously after the soldered 8 MB once the HMC has the
size code [4] (§3.2).

### 6.2 Power Macintosh 7100 — [pm7100.md](pm7100.md)

The midrange box (13.0 × 16.6 × 6.0 inches [1] Table 2-1 p. 12): machine ID `$A55A3012`,
Gestalt `$70`, two SIMM pairs, 136 MB maximum, one Curio SCSI channel [1] Table 1-5 p. 10;
[2] Table A-1 p. 9. It is the smallest machine with the full expansion set: three NuBus
connectors on the board (slots `$B`, `$C`, `$D`; [bart.md](bart.md) §1.4) plus the PDS video
slot `$E` that the boot ROM hands to the HPV VRAM card or the AV card [1] §"VRAM Expansion
Card" p. 40. The 7100/80 refresh runs at 80/40 MHz and takes the 1995 ROM (§1.3), with
write-through, copy-back and cache-inhibited NuBus slot cacheability [2] pp. 4–5.

### 6.3 Power Macintosh 8100 — [pm8100.md](pm8100.md)

The big box (7.8 × 16.0 × 14.3 inches [1] Table 2-1 p. 12): machine ID `$A55A3013`, Gestalt
`$41`, four SIMM pairs, 264 MB maximum — the only machine that fills all eight DRAM bank
windows of §3.2 [1] Table 1-5 p. 10; [2] Table A-1 p. 9. It is the only machine with two
SCSI buses: the Curio's standard channel plus the discrete 53CF96 fast internal bus
[1] §"System Architecture" p. 13; [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §1 — and
the only machine shipped with the L2 cache installed (256 KB [1] §"Cache Memory" p. 15). Its
Enhanced variants are the most changed machines in the family: the 8100/100 at 100 MHz
(3:1 bus ratio) and the 8100/110 at 110 MHz take the 1995 ROM, the BART 21 bridge, and — on
the /110 — the ICS9178 clock generation and a new cache SIMM design [2] pp. 3–5, 8.

### 6.4 The Enhanced variants

The January 1995 refresh (6100/66, 7100/80, 8100/100, 8100/110) changed clock-generator
vendors and part numbers ("transparent to third-party software"), bus speeds, and — on the
8100/100 and /110 — the NuBus bridge generation; the ROM split of §1.3 follows the models,
not the marketing label [2] pp. 3–8, Table 1-3 p. 5. The 6100/66 answers the same machine-ID
register value as the 6100/60, so software distinguishes them only through the
clock-derived Gestalt speed grade (§2.6) [4].

### 6.5 Adjacent machines and upgrade cards

The same two ROM images carry decoder and product records beyond the three boxes:
**PowerPC processor upgrade cards** (601 cards for PDS-equipped Quadra-class machines) with
classic Quadra-style I/O maps — the upgrade card boots from the same 4 MB image and
identifies as its own platform record [4] — and the 1995 ROM's fourth product record
`$3014` [5] (§7). The Workgroup Server variants of the three boxes are reported to share the
platform (*inferred — unverified*; no server unit is in the evidence set). The AV variants of
all three machines are the same motherboards with the AV card occupying the PDS slot; the
card's components and the DAV connector that lets NuBus cards tap the AV audio and video
streams are described with the AV subsystem [1] §"AV Card" p. 42, §"DAV Interface" p. 53.

## 7. Open questions

1. **The 6100 and 7100 boards are absent from the schematic evidence set** — the only board
   drawing available is the 8100/110 (drawing 051-0333). Every pin-level claim about the
   6100 and 7100 (adapter-card interrupt routing, PDS wiring, SIMM wiring) is *inferred*
   from the 8100 and the developer note's block diagram.
2. **The first-generation clock tree.** Squidlet is named in the developer note but no
   first-generation schematic exists; whether the 1994 boards' fixed oscillators match the
   8100/110's Table 2-3 wiring exactly is assumed, not verified.
3. **The Ethernet DMA channels' arbitration position** — absent from Apple's Table 2-4
   (§5.2); no document places them relative to the SWIM III/sound/SCSI/SCC rows.
4. **The emulated VIA timer rate.** 783.36 kHz is the classic rate and the expected value,
   but no ROM constant naming it has been located; Time Manager timing on PDM is consistent
   with it but does not pin it (*inferred — unverified*).
5. **The ConfigInfo segment maps and BAT ranges** — the raw tables exist in both ROM images
   but a complete logical-to-physical decode of the runtime page tables (beyond the fixed
   windows of §3.6) has not been written out.
6. **The ConfigInfo region at ROM offset +$44** (`$FFF30000`, 64 KB): its contents and purpose
   are unidentified (suspected emulator data/tables).
7. **The `$3014` product record of the 1995 ROM** — which machine it identifies is unknown;
   the 8100/110 class is the natural candidate but is not established (*inferred —
   unverified*).
8. **The pseudo-VIA2 monitor-parameters register** (`$50F26010`) — its bit layout is
   undecoded (also open in [amic.md](amic.md) §6).
9. **The Enhanced models' silicon deltas.** Apple states that some of the ASIC chips "have
   been updated" [2] §"Revised ROM and ASICs" p. 3, but beyond BART 21 and the clock
   chip no document says which; whether the 6100/66, 7100/80 and 8100/100 AMICs and HMCs
   differ from the 1994 parts is unverified.
10. **Workgroup Server membership.** The server variants of the three boxes are reported to
    be PDM machines; the evidence set contains no server documentation, so their
    membership, IDs and deltas are unverified.
11. **The warm-restart details on the 68k side** — which low-memory flag shortens the 68k
    RAM test on a warm boot, and precisely how the 68k ROM re-enters after a restart, are
    traced only partially.
12. **BART's clock generation on the 8100/110** — the frequency of the synthesiser output
    that clocks BART's processor-side logic (the 40 MHz oscillator feed is on the drawing;
    the synthesiser output's rate is not annotated) (also open in [bart.md](bart.md) §6).
13. **AV-model bring-up.** The AV machines run the same ROM as the rest of the family;
    where in the boot sequence the AV card is initialised (its slot-$E declaration ROM,
    its video path) is described only at the Slot Manager level; the full AV bring-up
    sequence is not pinned to ROM addresses.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh
   6100/60, 6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March
   1994 — §"Machine Identification" Table 1-5 p. 10; §"Physical Forms" Table 2-1 p. 12;
   Figure 2-1 p. 13 (block diagram, the two dotted-line variations); §"Main Processor",
   §"Read-Only Memory", §"Random-Access Memory", §"Cache Memory", §"High-Speed Memory
   Controller", §"Apple Memory-Mapped I/O Controller", §"Data Path Chips", §"Ariel II Video
   Chip", §"SWIM III Floppy Disk Drive Controller", §"Curio I/O Chip", §"Cuda
   Microcontroller Chip", §"Squidlet Chip" pp. 14–17; §"System Clocks" Tables 2-2 and 2-3
   pp. 19–20; §"CPU Bus", §"CPU Bus Arbitration" Table 2-4 and §"Address Errors" pp. 20–21;
   §"Memory Organization", §"Physical Memory Allocations" Table 2-5, §"RAM Access" p. 22;
   §"Emulated Interrupt Handling" Figure 2-2 pp. 22–23; §"VRAM Expansion Card" p. 40,
   §"AV Card" p. 42; Chapter 4: §"NuBus Slot Connections" pp. 50–53, §"DAV Interface" p. 53,
   §"PDS Expansion Cards" p. 54; built-in video Tables 3-8/3-10 pp. 36, 39; sound subsystem
   pp. 46–48; Glossary p. 79.
2. Apple Computer, Inc., *Developer Note: Enhanced Power Macintosh Computers* (Power
   Macintosh 6100/66, 7100/80, 8100/100, 8100/110), Developer Press, 1994 — Table 1-1
   shipping configurations p. 2; Table 1-2 model-specific clocks p. 3; §"Revised ROM and
   ASICs", §"NuBus Support" (BART 21, per-slot cacheability `_HWPriv` selectors, minor-slot
   extension, Table 1-3 NuBus changes) and §"LocalTalk" pp. 3–5; §"Clock Chips" p. 5;
   Chapter 2 "Power Macintosh 8100/110" (configuration, clock speeds, new cache, NuBus and
   ROM changes) p. 8; Appendix "Power Macintosh RAM Layout" (§"RAM SIMMs", Tables A-1 and
   A-2, discontinuous physical addressing) pp. 9–10.
3. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing 051-0333
   rev A (31 sheets) — sheet 4 (HMC, ROM SIMM control), sheet 5 (data path), sheet 12 (AMIC:
   address decode, interrupt pin wiring, Cuda transport, reset fan-out), sheet 11 (clock
   oscillators and the ICS9178 synthesizer), sheet 16 (Cuda), sheet 22 (NuBus connectors,
   slot interrupt lines), sheet 23 (BART).
4. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3 (March
   1994; 4 MB image, based at $FFC00000) — annotated disassembly and data-table analysis.
   Cited: the hard-reset vector and HWInit ($FFF03000 onward: checksum and re-base, MMU
   pre-init, platform quiesce, CPU-clock measurement and the standard-frequency snap table,
   bus-ratio measurement, HMC serial configuration and timing tables, machine-ID byte reads
   and the 6100/7100/8100 probe-window dispatch, the RAM probe, bank-size encoding, the
   flash-ROM probe, the L2 cache sizing); the ConfigInfo record (all fields of §2.7,
   `LA_*` windows, interrupt masks and handler kind, page-attribute default); the
   nanokernel's external-interrupt handler and its flag-to-level table; the 68k identity
   routine promoting $3010 to $3011 (file offset $10070); the Gestalt box-flag synthesis
   from the machine-ID low bits and the measured CPU clock; the decoder/product tables
   (including the PowerPC-upgrade-card host records); the startup chime played out of the
   DMA window at physical zero; the framebuffer allocation sequence (forced physical zero,
   write-through, 604 KB); the logical-RAM +$97000 framebuffer shift; the warm-restart
   page-table dependence; the diagnostic checks; the native PEF fragment table and the
   68k↔native kernel-call mechanism; the serial driver's baud-clock constants; and
   boot-time observation of the whole sequence under System 7.5.
5. Power Macintosh boot ROM, header checksum $9B7A3AAD (January 1995 revision, shipped with
   the Power Macintosh 7100/80, 8100/100 and 8100/110) — disassembly: bootstrap version
   "Boot PDM 601 1.1", the emulator entry-offset change, the extended CPU-frequency snap
   table, and the fourth product record (cpuID $3014) at file offset $15350.
6. Motorola, Inc. and International Business Machines Corporation, *PowerPC 601 RISC
   Microprocessor User's Manual* (MPC601UM/D), 1995 — the 601 architecture: register set, MMU
   and segment/BAT model, exception vectors and MSR (including MSR[IP] and the `$FFF00100`
   reset fetch), cache organisation, RTC/decrementer behaviour, and the single-beat/burst
   transfer protocols the platform's bus agents implement.
7. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley Publishing Company, 1992 — Chapter 7 "NuBus Card Memory Access"
   pp. 132–137 (standard and super slot space, slot allocations, the byte-lane and
   byte-swap rules of §2.5 and §3.1).
8. Apple Computer, Inc. and Prime Time Freeware, *MkLinux DR3* kernel sources, Power
   Macintosh platform support (GPL release), 1997 — independently developed drivers for
   the same silicon: the HMC configuration state that places the framebuffer at physical
   $00100000 (configuration bit 33 clear, the contrast case for §3.7), the AMIC DMA
   register offsets and buffer layout, and the pseudo-VIA interrupt model.
