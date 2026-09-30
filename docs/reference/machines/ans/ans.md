# The Apple Network Server family

The Apple Network Servers — the 500/132 and the 700 — are Apple's only non-Macintosh computers: AIX 4.1 servers built on the Macintosh TNT logic-board platform, sold between 1996 and 1997, and the one machine in Apple's line whose boot ROM refuses to start the Mac OS at all.

**Contents:**

1. [Overview & membership](#1-overview--membership) — what the family is, its membership and identity, the two ROM images, how this page splits against the TNT family doc and the machine pages
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the delta against the TNT platform; the processor card and clocking; parity memory and the 512 MB decode cap; the L2 cache; the 54M30 video; the front panel, power and cooling; power-on, POST and the boot chain
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family) — the physical map, bridge cycles and configuration ports, the IDSEL and slot map, the Grand Central window, the GBUS devices in full
4. [Device roster](#4-device-roster) — the silicon set and the device tree it produces
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the external interrupt remap, the AIX interrupt contract, the system bus and multiprocessor doorbell, the clock set
6. [Per-machine index](#6-per-machine-index) — the 500/132; the 700/150, 700/200 and 700/200SMP
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the family is

The Network Server is Apple's first computer designed as a server rather than as a Macintosh: "the first servers from Apple to be designed as high performance, scalable, reliable, and serviceable servers targeted at Unix accounts serving Macintosh Desktops" [1] §1 p. 2. Its target operating system is stated without qualification — "AIX 4.1, keeping pace with IBM updates as soon as practical" [1] §1.2.1 p. 2 — and the machine is designed around a redundant-everything brief: hot-swappable drive bays, hot-swappable power supplies (700), hot-swappable fans, parity memory, environmental monitoring, and a locking, sliding chassis. The two product price points are distinguished by "More drive Bays, Redundant Power Supplies, Higher Clock frequency, More Cache memory" [1] §1.1.2 p. 2.

Architecturally the machine is a Power Macintosh 9500 derivative, and Apple says so on page 2: "This specification is unfortunately not one-stop shopping, owing to the architectural origins of the Network Servers in the PowerMac 9500 family. Therefore much of the hardware detail which is fully documented in the PowerMac family is not repeated here. Instead, unique hardware interfaces are described" [1] §1 p. 2. This page follows the same split. The whole TNT base — Hammerhead, the two Bandit PCI bridges, Grand Central and its I/O cells (53C94, MACE, ESCC, AWACS, SWIM III, Cuda/VIA), DBDMA, the 60x system bus, the $F-segment address decode — is documented once in the TNT family doc and its device pages, and is cited here by section, never restated: [tnt.md](../tnt/tnt.md) §2–§5 carries the platform, [hammerhead.md](../tnt/hammerhead.md), [bandit.md](../tnt/bandit.md), [grand-central.md](../tnt/grand-central.md) and [dbdma.md](../tnt/dbdma.md) carry the chips. Everything the servers change or add is this page's subject: the different I/O set, the GBus devices, parity memory, the front panel, power and cooling, and the AIX-oriented firmware. The three-level split then applies as on the TNT set: the machine pages ([ans500.md](ans500.md), [ans700.md](ans700.md)) hold what is unique to each box, the device pages hold the chips, and a re-implementation reads [tnt.md](../tnt/tnt.md) first, then this page, then §4's device pages, then the machine page — between the layers nothing is missing.

Two hardware-level facts define the family against every other Apple machine. First, the machine-identity signal is the **pair of Symbios 53C825A fast/wide SCSI PCI devices**: the boot ROM probes them at startup and, on finding them, sets its "ESB" flag, publishes the root identity `AAPL,ShinerESB`, drops the `mesh` node, and rewires the slot map (§1.3, §5.1) — the only Apple platform whose identity is derived from a third-party SCSI part. Second, the machine has **no Mac OS Toolbox ROM**: the production ROM's software contract is the Open Firmware device tree and an AIX bootstrap, and it carries the string `MacOS is not supported` [7] (*observed* in the ROM image). The Network Server is consequently the one OldWorld-format Apple machine that no Mac OS release, and no ROM-based Mac OS boot path, can target.

### 1.2 Membership

| Machine | Apple codename | Processor card as shipped | System bus | L2 cache | Machine page |
|---|---|---|---|---|---|
| Apple Network Server 500/132 | Shiner | PowerPC 604 at 132 MHz | 44 MHz | 512 KB | [ans500.md](ans500.md) / §6.1 |
| Apple Network Server 700/150 | Shiner | PowerPC 604 at 150 MHz | 50 MHz | 1 MB | [ans700.md](ans700.md) / §6.2 |
| Apple Network Server 700/200 | Shiner | PowerPC 604e at 200 MHz | 50 MHz | 1 MB | [ans700.md](ans700.md) / §6.2 |
| Apple Network Server 700/200SMP | Shiner | two PowerPC 604e at 200 MHz | 50 MHz | 1 MB | [ans700.md](ans700.md) / §6.2 |

The Service Source manual names the three shipping models — "Network Server 500/132, Network Server 700/150, and Network Server 700/200" — and, for the SMP configuration, "two 200-MHz PowerPC 604e microprocessors (Network Server 700/200SMP)" [3] Basics/Overview pp. 1–3. Membership has two edges worth stating. The **700/200** is a late 1996 addition — "now the highest-performance member of the Network Server product line" — delivered as a processor-card upgrade installed in an existing 700 [5] p. 2, [6]. And an unshipped sibling exists: the **Network Server 300**, the rack-mount model Apple never released, is named alongside the 500 and 700 in the prototype ROM's build designation [8] (*observed*; the image's dump set is titled for the "ANS 300 & 500 & 700"). Its hardware is not in evidence and it is outside this page's scope.

The family is not "64-bit ready": Apple states flatly that the machine "is not meant to accommodate a 64-bit implementation based on the 620 microprocessor" [1] §1.1.1 p. 2.

### 1.3 The ROM: one production image, one prototype

Every Network Server runs a 4 MB ROM at $FFC00000, structurally the TNT image's close cousin (the reset vector $FFF00100 is fetched through Hammerhead exactly as on the 9500, §2.7; [1] §3 p. 10). Two distinct images exist:

| Image | Checksum @0 | Version field | Boots | Character |
|---|---|---|---|---|
| Production ROM (Open Firmware 1.1.22) | $962F6C13 | $077D.28F2 | AIX | no Mac OS Toolbox; `MacOS is not supported` |
| Unreleased prototype ("2.0", build dated 1998-01-14) | $49B2BE8F | $077D.7DD0 | Mac OS | adds Mac OS drivers for the 54M30 and the 53C8xx |

The production image is the machine's software identity. Its Open Firmware is version 1.1.22 — against the 9500's 1.0.5 ([tnt.md](../tnt/tnt.md) §2.7) — and its detokenized Forth source establishes the boot contract this page cites throughout [7]: the root `compatible` property is set, gated on the ESB flag, to `AAPL,ShinerESB` followed by `MacRISC`; the flag itself is set by the 53C825A probe (§5.1); the `screen` alias points at `/bandit/54m30@F` and the `lcd` alias at `/bandit/gc/lcd`; the disk aliases bind `disk0`–`disk3` to `/bandit/53c825@11/sd@N,0` and `disk4` onward to `/bandit/53c825@12/sd@N,0` — the seven-bay backplane split across the two controllers, four bays on bus 0 and three on bus 1, exactly as the hardware wires it (§2.8, §4). One identification trap is worth pinning here: the production ROM's version field $077D.28F2 is **byte-identical to the Power Macintosh 9500 v2 ROM's**, and only the checksum at offset 0 distinguishes the images [7] (*observed* in both images). Identify Network Server ROMs by checksum, never by version.

The prototype image is Apple-original but never shipped: it carries a native Mac OS driver for the Cirrus 54M30 (`.Display_Video_Cirrus_54M30`) and one for the 53C8xx (`SimNCR53c8xx`), its Open Firmware is version 2.0, and its model table lists only `AAPL,7500`/`8500`/`9500`/`7300` — it deliberately presents as a Power Macintosh and contains no `ShinerESB` string at all [8] (*observed* in the reassembled image; the ROM socket is a 64-bit DIMM built from four byte-swapped 16-bit lanes, so the four chip dumps must be interleaved rather than concatenated). It boots Mac OS, cannot boot AIX, and misconfigures the L2 cache — evidence that Apple built Mac OS support for this hardware and withheld it, nothing more.

## 2. Board architecture common to the family

### 2.1 The delta against the TNT platform

Everything the Network Server changes relative to the Power Macintosh 9500, in one table. Each row's detail is the section given in the last column; the TNT baseline is cited to the TNT set.

| Area | Delta against the TNT baseline | Where |
|---|---|---|
| Internal fast SCSI | MESH ([tnt.md](../tnt/tnt.md) §4.3) **removed**; two Symbios 53C825A PCI devices instead | §4, [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) |
| On-board video | **no video bus** — the Control/Chaos subsystem ([tnt.md](../tnt/tnt.md) §2.4) replaced by a Cirrus 54M30 PCI device, 1 MB, no interrupt | §2.5 |
| Slot-to-bridge split | slots 1–2 on Bandit 1, 3–6 on Bandit 2 (the 9500 splits 1–3 / 4–6); six devices on Bandit 1 | §3.3 |
| External interrupts | the 53C825As take EXT2/EXT6; both Bandits ganged onto EXT1 as `Error_Int` | §5.1 |
| DBDMA channels | **ten**, not eleven — channel 10 (MESH DMA) gone with MESH | §3.4 |
| Memory | **byte parity**, with a parity-driven DRAM timing switch | §2.3 |
| L2 cache | 8500-pinout cache DIMM; "fast L2" below 44 MHz | §2.4 |
| Processor card | ANS-specific connector and form factor; dual-CPU designed in | §2.2 |
| Board devices | keyswitch, LCD, environmental sensors, Ethernet PROM doorbell on the GBus | §3.5 |
| Firmware | Open Firmware 1.1.22 and an AIX bootstrap; no Mac OS Toolbox | §1.3, §2.7 |
| Chassis | locking sliding drawer, seven-bay hot-swap backplane, front LCD | §2.6, §2.8 |

Two things do **not** change and are worth saying explicitly because secondary coverage gets them wrong. The **AWACS audio path is unchanged** — the codec, its Grand Central apertures and DBDMA channels 8/9 are exactly the TNT ones ([grand-central.md](../tnt/grand-central.md) §2.5); the machine sounds the same Macintosh-family boot beep, and AIX's startup sounds the system boot "quack" through it (§2.7). And **Ethernet is the same Curio/MACE cell** — with the one wiring difference that the ANS brings it out on an AAUI connector only, at 10 Mb/s [1] §1.3 p. 2; there is no 10BASE-T PHY and the 9500's `ETH10BT_Link` board-register bit "is not supported" [1] §4.6.1 p. 15.

The enclosure is Apple's "Hendy" product design, "approximately 17 W x 23 H x 18 D", about 60 pounds with one hard drive, built around "30 second component access" — the entire logic module slides out of the rear on a drawer [1] §1.3 p. 2, §8.1 p. 30; [3] Basics/Front View p. 23.

### 2.2 The processor card and clocking

The Network Server keeps the TNT platform's replaceable-processor-card architecture ([tnt.md](../tnt/tnt.md) §2.2) and inverts its clocking: the motherboard takes its system clock **from** the card. "To enable processor daughtercard upgrades, a means is required by which the motherboard acquires clocking from the processor card. Processor clocking is synchronous to ASICs on the main logic board... Supported system speeds are 40-50 MHz" [1] §2.10 p. 9. The card supplies `refClk`, which "needs to be a fast transition signal (min 1v/ns)" — F-family or better — and the board returns `clkToProc` "asynchronous to refClk but synchronous to the system ASICs but in advance by approximately 700 ps (4 typical trace)"; processors with 0 ns output hold time, which includes the 604, "will require inserted clock delay of at least 1 ns" [1] §6.1 pp. 19–21. A 604-class card is therefore mandatory equipment in a different sense too: the connector note that "for 601 processors, a local timebase is required" [1] §6.1 p. 21 is evidence the connector was designed while 601 cards were still contemplated, though no 601 Network Server shipped.

The card is mechanically specific to the family even though its software interface is not: "the way software accesses the dual processor hardware implementation is the same as in the PowerMac 9500 family, although the physical connector and card form factor are specific to the Network Server" [1] §3 p. 10, and the system bus is "essentially identical to the 9500 system bus... The pinout and form factors are not compatible with the 9500 however" [1] §5 p. 17 — which is why real 9500 CPU cards do not fit an ANS and vice versa. The connector is a 180-signal-contact AMP surface-mount dual-90 edge connector (part 94-7831-08) [1] §6.1 p. 19, with current limits of 0.4 A at +12 V, 12 A at +5 V and 24 A at +3.3 V [1] §6.1 p. 21.

Dual-processor support is designed in from the start, unlike the 9500 where it is a card option: the block diagram is labelled "CPU CARD — Supports 2 CPU's", the cards are "implemented on a large form factor to accomodate multiple CPU's and greater power dissapation. Power and cooling for up to about 70 Watts is achievable" [1] §2.11 p. 9, and the reset flow has the secondary processor "enter a spin-wait for an interprocessor interrupt" while the primary proceeds with initialization [1] §3 p. 10. The multiprocessor surface — `WhoAmI`, the `IntReg` doorbell, the bring-up order — is Hammerhead's and is already documented with the Network Server's own evidence: [hammerhead.md](../tnt/hammerhead.md) §2.5–§2.7, §4.5. The two ANS-specific MP facts are in §5.3: the doorbell is an address, not a register, and the timebases can be stopped and restarted in lock-step from a GBus bit. "Enabling of the second processor in a two processor system will be operating system dependent" [1] §3 p. 10 — the 700/200SMP is the shipping expression of that sentence, and no AIX release in evidence is known to enable it (§7.3).

One reset-timing constant belongs to the family: the "processor card hard reset de-asserts approximately 500 ms after Main Logic Board hard reset" [1] §6.1 p. 21 — a real bring-up constraint, since anything the card's logic does in that half-second happens under reset.

### 2.3 Memory: parity, timing, and the 512 MB decode cap

The memory array is the TNT one — 168-pin DIMMs on interleaved pairs, made contiguous by Hammerhead's bank base registers ([tnt.md](../tnt/tnt.md) §2.3; [hammerhead.md](../tnt/hammerhead.md) §3.2) — with eight slots arranged as four bank pairs, labelled 1A/1B through 4A/4B on the logic-board diagram [1] §2.4 p. 7. Three facts are the family's own.

**Byte parity.** "Parity memory is supported by a modification of the PowerMac 9500 data path chip. It writes and reads byte-wide parity for memory accesses only. ROM and SRAM are not parity protected" [1] §2.4.1 p. 7. Parity is not a Hammerhead function ([hammerhead.md](../tnt/hammerhead.md) §3.2); it lives in the datapath, which is why it costs a DRAM timing grade rather than a register bit:

> "The boot ROM sets DRAM timing based on two factors: the detected bus speed and detected parity. If parity is detected, 60 ns timing is set. If parity is not detected, 70 ns timing is set. At 50 MHz, 70 ns timing is approximately a 20% memory bandwidth penalty." [1] §2.4.1 p. 7

The direction of the effect is the counterintuitive part: installing one non-parity DIMM does not merely disable parity checking on that bank, it drops **all** memory to 70 ns. Parity FPM is therefore the faster configuration on this machine — a real behavioural difference from every other TNT box. A parity error is not silent: "Memory parity errors generate MCP to the 60x. This may or may not be gracefully handled by the operating system, however it should normally prevent further execution" [1] §5.2 p. 18; the `ParityErrL` line is present on the processor connector and "would normally be routed to MCP on the processor" [1] §6.1 p. 21.

**The decode cap.** "Eight DIMM slots are provided; production ROMs through (TBD) August 1996? provide decoding of up to 512 Mbytes (Eight 64 Mbyte or four 128 Mbyte DIMM's)" [1] §2.4.1 p. 7 — Apple's own "(TBD)" is in the original. The 512 MB ceiling is a **ROM decode limit, not a Hammerhead limit**: the controller itself reaches 1.5 GB ([tnt.md](../tnt/tnt.md) §2.3), but the Network Server ROM stops at 512 MB, and the documented failure mode of exceeding it is a hang during the long RAM test rather than a clean error. The later software baseline lifts the floor beneath the cap ("Support has been added for up to 512 megabytes (MB) of RAM" [5] p. 3) — i.e. makes 512 MB real — without ever raising it.

**Device rules.** "Sixty-four megabit technology is recommended for DIMMs of this size to reduce bus loading at 60 ns timing. FCT technology should be used for buffers; ACT is specifically not supported" [1] §2.4.1 p. 7.

### 2.4 The L2 cache

The cache DIMM follows the **8500**, not the 9500 — easy to get backwards given that everything else here follows the 9500: "The cache DIMM connector is fit, form and function compatible with the PowerMac 8500 cache slot" [1] §6.2 p. 21. Sizes are 512 KB on the 500 and 1 MB on the 700 [3] Basics/Overview pp. 2–4.

The ROM behaviour keyed to the bus clock is the family-specific part: "Network Server ROM enables 'fast L2' mode for bus speeds of 44 Mhz or less" [1] §2.4.2 p. 8, §6.2 p. 21, which "decreases latency to the L2 by one cycle" [1] §2.4.2 p. 8. Fast L2 is therefore a function of the installed card's bus speed, not of the processor part: a 132 MHz card (44 MHz bus) gets it, a 150 MHz card (50 MHz bus) does not, and installing the 200 MHz upgrade in a 500 raises the bus from 44 to 50 MHz and *removes* fast L2 while raising the clock (§6). The SRAM rules are "Tag and cache memory speeds need to be 8ns and 11 ns or better, respectively" [1] §2.4.2 p. 8, with the sharper "Do not use slower than 8 ns tag ram, 7 ns preferred" [1] §6.2 p. 21, and one recorded electrical erratum: "early versions of burst SRAM from Motorola may cause signal integrity issues within the DIMM itself, resulting in clocking glitches that cause failed burst writes"; later-generation 5 V parts, or 3.3 V parts from Motorola, Micron or IBM, are preferred [1] §6.2 p. 21.

The L2 controller itself — sensing, the power-on test, the cache-control register file — is Hammerhead's and is not repeated here: [hammerhead.md](../tnt/hammerhead.md) §2.8–§2.9, §3.4, §4.4.

### 2.5 On-board video: the Cirrus 54M30

Where the TNT machines hang a Control/Chaos framebuffer subsystem off a dedicated video bus ([tnt.md](../tnt/tnt.md) §2.4), the Network Server has **no video bus at all**. Its on-board video is a single ordinary PCI device: "The Network Server implements a Cirrus Logic 54M30 video controller, which provides a bit-mapped 1Mbyte DRAM frame buffer" [1] §2.8 p. 9. The connector is "standard VGA with DDC-2 monitor sense. However the operating system may or may not interact with DDC" [1] §2.8 p. 9 — not Apple's DA-15.

The single most important fact about the part is its byte order:

> "This controller implements only a little-endian window into the packed-pixel frame buffer, hence Big Endian operating systems are limited to 8 bits per pixel unless low-level transformation routines are written. The buffer will support 1024x768 at 8 bits, however many monitor modes are supported." [1] §2.8 p. 9

The 8 bpp ceiling is not a memory limit (1 MB holds 1024x768x8 with room to spare) — it is the byte order. At 8 bpp each pixel is one byte and byte order does not matter; at 16 or 24 bpp it does, and a big-endian operating system would have to byte-swap every pixel in software. Bandit's register-swapping does not rescue this: framebuffer data crossing the bridge is data, not a register access ([tnt.md](../tnt/tnt.md) §2.5). The frame buffer DRAM is "Extended RAS... configured with adequate DRAM speed to accomplish 68 MHz clocking" [1] §2.8 p. 9. Apple is candid about the parts's purpose: "Hardware acceleration and cursors are available requiring software implementation. Pure bit-mapped mode will undoubtedly be visibly slow and require significant CPU utilization. Screen savers should be discouraged for maximum system performance" [1] §2.8 p. 9.

Identity and placement: PCI vendor $1013 (Cirrus Logic), device $00A0, Open Firmware node `54m30` with `compatible = "pci1013,a0"` and `model` of that form, unit address `@F` — IDSEL 15 on Bandit 1 [1] §4.6.2 p. 16; [7]. The "54M30" name appears in no other Apple product and matches no public Cirrus part number; the register map that applies is the Cirrus "Alpine" CL-GD543X/4X family's, and the 1 MB frame buffer and 1024x768 ceiling point at the GD5430/GD5440 end of that family rather than the 4 MB-capable GD5434 — the exact die behind the marking remains unconfirmed (§7.2). The controller **has no interrupt line**: "Note that the 54M30 video controller has no interrupt line" [1] §"Network Server External Interrupt Map" p. 16; its device-tree node carries no usable `interrupts` property routed to Grand Central.

Driver reality determines what an implementation has to be: AIX uses the part at 8 bpp as the console framebuffer, driven as a dumb linear frame buffer — no shipping software exercised the acceleration registers — and the production ROM contains no Mac OS driver for it; only the unreleased prototype ROM adds one ([8]; §1.3). A Mac OS boot on this hardware therefore goes through a PCI video card in a slot, not the 54M30.

### 2.6 The front panel: keyswitch, LCD, power and cooling

The front panel carries the power button (next to the floppy drive), a reset button, an interrupt button, the floppy and CD bays with an optional tape bay, the speaker, the seven drive bays behind a sliding security door, and the diagnostics LCD; the 700 adds a second power-supply bay [3] Basics/Front View p. 23, Basics/Setup and Operation pp. 19, 22–23.

**The keyswitch** is a hardware input with firmware semantics — the one Apple machine where the position of a physical key changes what the boot does. "Network Servers provide a keyswitch function which both Open Firmware and the Operating System interact with to set a boot path, as well as provide some lock-out of functionality. Open Firmware detects both the service and the locked positions of the key" [1] §2.6 p. 8, and the reset semantics differ per position:

| Key position | Cmd-Opt-P-R behaviour |
|---|---|
| Locked | Open Firmware "prevents all parameter and NVRAM resets" [1] §2.6 p. 8 |
| Service | Open Firmware "will erase all AIX-related booting parameters" [1] §2.6 p. 8 |
| Normal | erases "the Macintosh parameter RAM only" [1] §2.6 p. 8 |

The key position is readable from Board Register 1, bits 13 and 14 (§3.5.1), and the production ROM reads it early — its Forth vocabulary carries `get-keyswitch`, `(key=locked?`, `(key=service?` and `new-(key?)` words, with the locked position encoded as 1 and service as 2 [7] (*observed*). The service position additionally enables a first-boot path (§2.7). Note that the machine also has a second, unrelated key: the **rear drawer lock**, which must be in its locked position for the machine to power on at all (§2.8) — the service manual's "front key switch locks sliding security door and base cover" [3] Basics/Front View p. 23 is the panel key, and which physical switch drives which register bit is not stated by any document in evidence (§7.5).

**The LCD** is the machine's first and, during POST, only output device — a four-line character display driven through two write-only GBus registers, documented in full at §3.5.3. "It is the job of POST to initialize the hardware into a working state and establish a software path to the LCD. The LCD is then written with progress reports on the state of the discovered hardware: DRAM; SRAM cache; and various fan, temperature, and power supply fail states" [4] p. 5. The LCD comes up **before** memory is sized; it is a boot-critical output path, not a decoration.

**Power and cooling.** The 500 ships a 325 W modular supply; the 700 ships 425 W with "an optional redundant supply system... higher power and a power backplane", and "the installation of an optional redundant power supply does NOT increase these limits. The redundant supply system will current share to increase longevity. Power supplies are hot swappable" [1] §2.5 p. 8; [3] Basics/Overview of New Technologies p. 47. The supply budgets differ per rail:

| Model | +5 V max | +12 V max | +3.3 V max | total |
|---|---|---|---|---|
| 500, load #1 | 28 A | 11 A | 10 A | 325 W |
| 500, load #2 | 18 A | 11 A | 25 A | 325 W |
| 700, load #1 | 44 A | 13 A | 13 A | 425 W |
| 700, load #2 | 19 A | 13 A | 50 A | 425 W |

[1] §2.5 pp. 7–8. Minimum load is 3 A at +5 V (15 W) and the main logic board "should consume a minimum of 3 A @ 5 volts for start up conditions" [1] §2.4 p. 7. Internal SCSI drives "should perform delayed spin up", though the +12 V margin makes simultaneous spin-up survivable [1] §2.5 p. 8. The environmental sensors — two fans, two supply slots, processor-card thermal trips — are all polled, not signalled: they surface in Board Register 2 (§3.5.2), and Apple's programmatic note is explicit that "Network Server does not implement an interrupt for these functions. Software can implement a daemon that does background reads of this register (say every 30 seconds or so)" [1] §4.6.1 p. 15. The two thermal trip points are set "prior to hardware vulnerability, at a point that would normally correspond to either a blocked airflow or a slowing fan" (warn) and "slightly beyond worst case thermal ratings" (shut down) [1] §2.3.1 pp. 6–7.

### 2.7 Power-on, POST and the boot chain

The power-on chain, in the Theory of Operations' own order [4] pp. 1–4:

```
AC power path -> DC power path -> power controller path -> POST -> Open Firmware -> boot
```

The first two stages are a server's, not a Macintosh's: the AC interlock is closed "because the logic board is fully seated" (the sliding drawer is interlocked), and the supply provides "+5 volts trickle... to the power controller IC (Cuda)" — Cuda is alive before the machine is "on", exactly as on the TNT platform ([tnt.md](../tnt/tnt.md) §5.5). The power-controller stage carries the preconditions an implementation would never guess: "This means that the rear keyswitch is in the locked position, the processor card is fully seated, the Cuda chip is in the proper idle state, and the cables and connectors to the power switch are all in working order", plus short-circuit shutdown and a +5 V line that "is monitored by the power monitor IC on the logic board and if the voltage is below +4.7, the unit will also shut down" [4] p. 4. The processor-card precondition is enforced electrically: the card's `OffSenseOut` pins "are used to detect unseated processor cards and prevent the power supply from powering on" [1] §6.1 p. 21.

POST then runs — the same reset flow as the platform's ([tnt.md](../tnt/tnt.md) §2.7): the processor fetches $FFF00100 through Hammerhead, and "the first code executed will determine whether the processor is the primary processor... or if the processor is the secondary it will enter a spin-wait for an interprocessor interrupt" [1] §3 p. 10 — mapping and testing main memory "in conjunction with Hammerhead Registers", mapping, zeroing and enabling the L2 cache, building the device tree, finding a bootable device, detecting the keyswitch service position, and "detect[ing] and report[ing] system hardware failures to both the LCD panel and to the device tree" [1] §3 p. 10. Failures are recorded as device-tree properties, not only displayed. POST's results also persist in NVRAM (§3.5.5) — the production ROM's bootstrap reads them back and prints `POST results AOK.  Code is  00010000` on a clean machine [7] (*observed* on a full boot of the 1.1.22 ROM).

The LCD progress contract, which is the machine's only boot console until Open Firmware takes over the screen, is published message by message [3] Basics/Overview of New Technologies pp. 48–58:

Lines 1–2 hold the ROM version and copyright and then the parity DRAM size for the whole test phase; line 3 carries the progress messages in order — `DRAM test #1 Begins`, `ROM SIMM Test Begins`, `DRAM test #2 Begins`, `LONG DRAM test Begins`, `Turning on Caches`, `Jumping to RAM Prog`, `Testing Parity DIMMs`, `MainLBU Enet Setup`, `Sounding Boot Beep`, `Sizing RAM DIMMs`, `ROM SIMM Data Access`, `Allocating RAM DIMMs`, `MainLBUNVRAM Setup`, `CPU Card Info Setup`, `L2 Cache SIMM Setup`, `Testing L2 Cache SIMM`, `RAM/ROM/NVRAM: PASSED` — and line 4 accumulates consecutive dashes during the long memory test, one per DIMM slot tested [3] Basics/Overview of New Technologies pp. 48–52.

Failures print on line 1, one per module: `L2 Cache SIMM Failed`, `ROM SIMM Failed`, `MainLBUNVRAM Failed`, `RAM DIMM 1A failed at Address xxxxxxxx` (DIMMs identified 1A–4A/1B–4B), `MainLBUVideo ID Bad`, `MainLBU 825#1/825#2 Failed` (the two SCSI controllers), the fan, temperature and power-supply failures of Board Register 2 (`Drive Fan Failed!`, `Processor Fan Failed`, `Temperature Too Hot!`, `Temperature Warning!`, `Left/Right Power Fail!`, `Left/Right Power Hot!`), `CudaNotResponding!!!` and `ParityAddrAtAddrFail` [3] pp. 54–58 — the LCD messages and the register bits naming the same hardware. The completed display on a healthy 700/150 reads `ROM ver.1.1.20.1` / `0048 MB Parity DRAM` / `150 MHz 604, 50 MHz Bus` / `1024 KB Level 2 Cache` [3] p. 53. These messages are produced by POST on real hardware; an implementation that cannot accept the GBus LCD writes of §3.5.3 has no visibility into this phase at all.

Open Firmware then launches. Its primary job is "to find a bootable device (CD, floppy, or hard disk) based on the device or devices listed in the boot path **and the position of the front panel keyswitch**", and there is a first-boot path with two conditions: "If Open Firmware detects the key in the service position on a Network Server that has never been booted before, it will automatically attempt to find a diagnostic floppy or Install CD to boot from" [4] p. 6 — the documented route to an AIX install with a blank NVRAM. The companion behaviour is hard: with the key in service and a blank store, the firmware reconfigures and asks Cuda to pull the system reset line before booting the media — the ROM prints `RESETing to change Configuration!` and Cuda's reset-system pseudo-command must actually reset the machine [7] (*observed*; the command path and the failure mode — a Cuda that resets only itself leaves the firmware reporting `Can't reset-all` — are both witnessed).

The AIX boot itself: "If Open Firmware can find the boot blocks on the bootable device, 'Bootapple' messages are then written to the screen, and a compressed 'bosboot' image is loaded from disk into DRAM, expanded, and jumped to so that AIX execution can begin" [4] p. 7. The production ROM carries an `aix-boot` package alongside `xcoff-loader` and `iso-9660-files` [7] (*observed* in the firmware's package list), and the install media's block 0 is an IBM IPL record in the RS/6000 layout — four EBCDIC bytes `IBMA`, then a load image located at block $14A4, $124 blocks long, loaded and entered at $0011E124 [9] (*observed* on the 4.1.5 install CD). After `bootapple` hands over: kernel launch, configuration methods, file-server mode, "the system boot 'quack' is sounded, and the first system-wide interrupts are taken. SCSI buses are walked to discover attached devices, a File System Check (fsck) is performed" [4] pp. 7–8. The quack is a real diagnostic milestone — it sounds *before* the first system-wide interrupts, so hearing it proves the kernel and the audio path while proving nothing about the interrupt system.

The AIX side of the contract is the ODM device database, and the coupling makes node names load-bearing rather than cosmetic: "The properties guaranteed to be in the device tree for a device are the name property and the reg property. The name property identifies the device; the name must be stored in the Predefined Devices (PdDv) database of the ODM" [2] Ch. 6 p. 48; a device whose node name matches no PdDv entry is silently left unconfigured. The 4.1.5 media's ODM stanzas name the whole expected device set — the bridge `bandit` (`model` defaulting to `AAPL,343S1126`), `gc` and its expected Open Firmware children `53c94`, `mace`, `ch-a`, `ch-b`, `swim3`, `via-cuda`, the SCSI controllers `pci1000,3` (driver `pscsidd`) and the plug-in `pci1000,f`, the video part `pci1013,a0` (driver `cirrusdd`), the Mylex `dac960` RAID card and the DEC 21040/21140 Ethernet cards — and, notably, **no `awacs`, `lcd` or `adb` entry**: AIX never drove the Network Server's sound, and the LCD is POST/Open Firmware territory, not an AIX device [9]. The configuration methods read Open Firmware's `assigned-addresses` through a `resolve_pci_mem_space` routine — AIX consumes the firmware's BAR assignment rather than programming BARs itself, and the `busresolve` service is not supported [2] Ch. 8.

### 2.8 The storage backplane and the RAID interface

Both machines carry the same seven-bay hot-swap SCSI backplane — "The Network Server SCSI backplane consists of seven slots with hot swap. It is expected (but not required) that slot 0 will be a CD ROM" [1] §7.2 p. 23 — organized as "a pair of fast and wide buses with narrow compatibility", driven through the two 53C825As, leaving the logic board via the blind-mate mezzanine [1] §7.2.1 p. 23. The split matches the firmware's disk aliases (§1.3): four bays on bus 0 (drives 0–3) and three on bus 1 (drives 4–6), with the 700 adding "a rear drive bracket [that] will support two more drives which are cabled to the bottom bus" [1] §7.2.3 p. 25; [3] Basics/Overview pp. 2–4. "A stuffing option allows this backplane to appear as one bus... The terminators in the middle are automatically disabled by stealing a ground signal from the mating ribbon cable" [1] §7.2.3 pp. 25–26 — the low-end single-bus configuration.

The hot-swap mechanism is electrical, in the drive carrier: "a mechanically timed assertion, implemented through the use of long and short pins, of SCSI reset", plus advance power and ground on the long pins; insertion or removal "will cause a SCSI Reset for the time it takes to unmate or mate the long vs. short pins", asserted "a minimum of 5 ms" for typical pin lengths — and "operating systems may or may not support the use of SCSI reset to hot swap drives. Typically this function is reserved for RAID card use" [1] §7.2.4 p. 26. The carrier connector is a FutureBus 4x18 part with three pin lengths, and drives assume their SCSI ID from backplane wiring (`ScsiID0`–`ScsiID2` on pins D15–D17), not from jumpers; drive power is limited to 1.1 A at +5 and 1.3 A at +12 average, doubling for full-height drives [1] §7.2.4 p. 26, §7.3.1 p. 27.

Signal integrity is budgeted for implementers of the fast buses: about 12 inches of roughly 63 Ω trace on the logic board, standard 78 Ω single-ended ribbon under 24 inches on the mezzanine, and "including the backplane, the total extent of the SCSI bus should be under 6 feet" [1] §7.2.1 p. 23. Active terminators sit on the main logic board [1] §7.2.1 p. 23 — which is what the RAID interface's pin-19 trick defeats. The late software baseline adds "Support... for Ultra-SCSI drives" [5] p. 3, which the 825A's successor parts on cards satisfy; the on-board 825A is a Fast SCSI-2 part and does not.

The optional **RAID card** is a half-length PCI card in slot 1 — the logic-board diagram labels that slot "Slot 1 Bus 0 (Raid)" [1] §2.4 p. 7 — cabling back into the main logic board to take over the backplane: "Network Server implements the industry standard Wide SCSI pinout on the RAID access connectors, with the exception that pin 19 is stolen to disable the Main Logic Board termination, and TERMPOWER is not implemented. The RAID card will need to connect pin 19 to ground (which is normal)" [1] §7.2.2 p. 23. A separate **26-pin control interface** lets the card drive the per-drive failure LEDs on the front panel: `ledOE` "must be driven to ground by the mating interface to enable the LED function", `Bus0StrobeL` "latches the state of the drive LEDs 0-3" and `Bus1StrobeL` "latches the state of drive LED's 4-6", with `FailDrive0`–`FailDrive6` and `armO`/`armI` on the remaining pins [1] §7.2.2.1 p. 24 — so a RAID-equipped machine shows per-drive status with no main-logic-board involvement. The AIX side treats the RAID controller as "a single SCSI II controller... The RAID controller itself always takes ID 7", with logical IDs 8–15 for the system drives [10] Ch. 1.

The third SCSI bus must not be forgotten: the machine keeps the Curio 53C94 narrow external bus, "Mac Compatible SCSI-1" on a DB-25, on Grand Central device 0x0 and DBDMA channel 0 [1] §1.3 p. 2; [3] Basics/Overview pp. 2–4 — so the machine has **three** SCSI buses in all: two fast/wide internal, one narrow external.

## 3. Memory map & address decode shared by the family

### 3.1 The physical map

The Network Server "significantly leverages the MacRISC address Map... since the base I/O using Grand Central is very similar, much of the map is identical to the PowerMac 9500 series" [1] §4 p. 11. The deltas against the TNT map ([tnt.md](../tnt/tnt.md) §3.1) are all in the F segment, and they are enumerated by Apple's own address-map figure [1] §4 p. 11:

| Physical range | Contents | vs the TNT map |
|---|---|---|
| $00000000 – RAM top | main DRAM, ≤ 512 MB by ROM decode (§2.3) | same law, lower cap |
| $80000000 – $8FFFFFFF | PCI memory space behind Bandit 1 | unchanged ([tnt.md](../tnt/tnt.md) §3.3) |
| $90000000 – $9FFFFFFF | PCI memory space — the worked example's expansion-ROM BAR lands here (§3.2) | re-homed behind Bandit 2 (*inferred* from the worked example; the TNT map puts this range behind the absent Chaos) |
| $F0000000 – $F0FFFFFF | **Bandit 0 slice — not implemented** | TNT puts the Chaos display bridge here; the ANS has no Chaos |
| $F2000000 – $F2FFFFFF | Bandit 1 slice: on-board I/O plus slots 1–2 | unchanged base, different population |
| $F3000000 – $F301FFFF | Grand Central's 128 KB register window | unchanged (§3.4) |
| $F4000000 – $F4FFFFFF | Bandit 2 slice: slots 3–6 | unchanged base, different population |
| $F6000000 – $F6FFFFFF | **Bandit 3 slice — not implemented** | TNT also leaves this bridge unpopulated ([tnt.md](../tnt/tnt.md) §3.1) |
| $F8000000 | Hammerhead register window | unchanged |
| $FFC00000 – $FFFFFFFF | the 4 MB ROM, reset vector $FFF00100 | unchanged |

The structural consequence of the absent Bandit 0 is bigger than one table row: the TNT machines's whole $F1-segment display-bus device space — Control, RaDACal, the VRAM aperture ([tnt.md](../tnt/tnt.md) §3.1) — has no Network Server counterpart, because the machine has no display bus (§2.5). The remaining map law is the platform's own and is not restated here: the upper 7 bits of an F-segment address select the bridge number in the set $F0/$F2/$F4/$F6, the next 3 bits encode the PCI cycle type, and the whole decode is [bandit.md](../tnt/bandit.md) §3.4's.

### 3.2 Bridge cycles and the configuration ports

Bandit's cycle mechanics are the device page's ([bandit.md](../tnt/bandit.md) §3.1, §3.3–§3.5); what the Network Server adds is the instance list. The configuration ports — the indirect, mechanism-#1-style pair per bridge, "located in the bridges PCI I/O Space" [1] §4.4 pp. 12–13:

| ARBus address | Bridge | Register |
|---|---|---|
| $F0800000 / $F0C00000 | reserved (Bandit 0) | reserved |
| $F2800000 / $F2C00000 | Bandit 1 | Config Address / Config Data |
| $F4800000 / $F4C00000 | Bandit 2 | Config Address / Config Data |

[1] §4.4 p. 13. The Config Data "register" is a pseudo-register — "an access to this 'register' causes a Config (or, Special) Cycle to be made on the PCI side, using the address contained within the Config Address register", and the bridge "drives the contents of the Config Address register directly on the PCI AD lines, un-modified" [1] §4.4 p. 13. The two platform rules Apple restates for the server audience are the TNT ones: PCI I/O space must not be touched before a node is configured — "behavior is undefined if PCI I/O space is accessed BEFORE the PCI nodes are properly configured" [1] §4.3 p. 12 — and Grand Central lives in the pass-through region, "an 8 MB Pass-Through Memory region per ARBus-PCI bridge" defined by A[7] == 0b1 [1] §4.2 p. 12, which is how its registers land at $F3000000 inside Bandit 1's window ([tnt.md](../tnt/tnt.md) §3.2; [grand-central.md](../tnt/grand-central.md) §1.3).

**Discovery order is a contract**, not an implementation detail, because it fixes the order in which Open Firmware assigns BARs: "Open Firmware maps devices for their requested spaces in discovery order. Order of discovery is in slot order; on board input-output devices are configured prior to slots" [1] §4.4 p. 13 — an implementation that enumerates strictly by ascending IDSEL produces a device tree no published dump matches. Apple's worked example shows where the assignments land for a Symbios 53C875 card in the bottom slot: memory BARs at $F5100000 and $F5101000 (inside Bandit 2's pass-through window), I/O space at $400, and the expansion ROM at $90000000, down in the mappable PCI memory space below the F segment [2] Ch. 6.

### 3.3 IDSEL assignment and the slot map

The slot split is stated four times in Apple's documents and they agree: "The Network Server uses two separate PCI buses for on-board I/O (and two slots) and card expansion (four slots)" [1] §4.1 p. 11; "Slots 1 and 2 in the Network Server remain at IDSEL 13 and 14... For PCI Bus 2, PCI Slot 3 is moved to the second Bandit. Config cycles start at IDSEL(13) = Slot 3, (14) = Slot 4, (15) = Slot 5, (16) = Slot 6" [1] §4.6.2 p. 16, §7.1.1 p. 22. The RAID card manual's boot-path table independently confirms the same split — `pci1/dac960@d` and `@e` for slots 1–2, `pci2/dac960@d/@e/@f/@10` for slots 3–6 [10]. Beware Apple's two numbering schemes: the logic-board diagram labels the slots "Slot 1 Bus 0... Slot 6 Bus 1" [1] §2.4 p. 7, the address map says Bandit 1/Bandit 2 (or PCI1/PCI2), and §4.6.2's "PCI Bus 2" is the second Bandit. Bus 0 = Bandit 1 = $F2000000; Bus 1 = Bandit 2 = $F4000000.

The complete IDSEL map, with device-tree unit addresses (decimal IDSEL as Apple prints it, hex unit address as the tree encodes it):

| Bridge | IDSEL (dec) | Unit address | Occupant |
|---|---|---|---|
| Bandit 1 | 11 ($B) | `@B` | the bridge itself ([bandit.md](../tnt/bandit.md) §2.4) |
| Bandit 1 | 13 | `@D` | PCI slot 1 — the RAID card's slot (§2.8) |
| Bandit 1 | 14 | `@E` | PCI slot 2 |
| Bandit 1 | 15 | `@F` | the Cirrus 54M30 video controller |
| Bandit 1 | 16 | `@10` | Grand Central |
| Bandit 1 | 17 | `@11` | 53C825A fast/wide SCSI controller 0 |
| Bandit 1 | 18 | `@12` | 53C825A fast/wide SCSI controller 1 |
| Bandit 2 | 11 ($B) | `@B` | the bridge itself |
| Bandit 2 | 13–16 | `@D`–`@10` | PCI slots 3, 4, 5, 6 |

[1] §4.6.2 p. 16, §7.1.1 p. 22; [2] Ch. 6. Grand Central's `@10` is not printed as an IDSEL in the Hardware Developer Notes — it is read back from the published device tree (`/gc@10`) and is the only value consistent with slots 1–2 at 13–14 and video at 15 (*inferred* from agreeing sources; the result is asserted by the tree itself). Note the decimal/hex trap: IDSEL 17 is unit `@11`, because the unit address is the IDSEL *in hex*. Two structural facts fall out of the table: Bandit 1 carries **six** devices with no PCI-to-PCI bridge — the heaviest population any Bandit-1 bus in the platform family sees — and no IDSEL below 11 exists anywhere on the bus ([bandit.md](../tnt/bandit.md) §3.2).

The electrical layer is PCI revision 2.1-flavoured: "Network Server will use a 32-bit card to motherboard 5 Volt implementation for six slots of PCI expansion... Network Servers attempts to be PCI 2.1 compliant. Open issues surrounding this compliance are: Special Cycle support and discontinuous byte enables" [1] §7.1 p. 22 — an implementer need honour neither faithfully. The PRSNT signals are supported and surface in Board Register 1 (§3.5.1). For performance, "PCI cards in the Network Server need to implement the Cache Line Size register (32 bytes, 8 DWORDS), and Write with Invalidate for burst writes, and Read Multiple or Read Line for burst reads; otherwise the bridge will perform single beats to memory which decreases bandwidth significantly" [1] §7.1 p. 22. Slot power, all six cards in aggregate: "Not to exceed 50 watts in any combination" on the 500 and 90 W on the 700, with the per-card PCI 2.1 limit of 25 W always applying [1] §7.1.2 p. 22.

### 3.4 The Grand Central window: channels and devices

Grand Central "decodes 128 Kbytes of memory space and is configured by Open Firmware to live at xF300000" [1] §4.6.1 p. 13 — the same convention-only placement as on the TNT machines, where "Grand Central is fully PCI compliant and therefore can re-position its memory space response to anywhere in PCI memory space" [1] §4.2.1 p. 12; [grand-central.md](../tnt/grand-central.md) §1.3. The decode law inside the 128 KB — DMA controller registers, DMA channel registers, device registers on the `1_dddd` pattern — and the "writes to unmapped portions... will have no affect. Reads of unmapped locations will return zeros" rule are [grand-central.md](../tnt/grand-central.md) §2.1's and §2.5's, stated in [1] §4.6.1 p. 13.

The Network Server's own contribution to the tables is the channel and device roster. **Ten DMA channels** — and this is a real delta against the eleven of the TNT machines ([tnt.md](../tnt/tnt.md) §4.4): channel 10, MESH DMA, is gone with MESH:

| Channel | Name |
|---|---|
| 0x00 | SCSI (the 53C94) |
| 0x01 | Floppy |
| 0x02 | Ethernet Transmit |
| 0x03 | Ethernet Receive |
| 0x04 | SCC Channel A Transmit |
| 0x05 | SCC Channel A Receive |
| 0x06 | SCC Channel B Transmit |
| 0x07 | SCC Channel B Receive |
| 0x08 | Audio Out |
| 0x09 | Audio In |

[1] §4.6.1 p. 14. The device numbers are the TNT set with device 8 (MESH) absent and the ANS's GBus devices named:

| Device | Name |
|---|---|
| 0x0 | SCSI (the 53C94) |
| 0x1 | MACE |
| 0x2 | SCC "Compatibility" port |
| 0x3 | SCC "MacRISC" port |
| 0x4 | Audio |
| 0x5 | SWIM3 |
| 0x6–0x7 | VIA |
| 0x9 | Ethernet Address PROM |
| 0xA | GBus device 1 — Board Register 1 |
| 0xB | GBus device 2 — reserved |
| 0xC | GBus device 3 — LCD and Time Base Enable |
| 0xD | GBus device 4 — NVRAM high address |
| 0xE | GBus device 5 — Board Register 2 |
| 0xF | GBus device 6 — NVRAM data |

[1] §4.6.1 p. 14. "Grand Central implements a Generic Bus (GBUS) which provides six chip selects and write enable which the Network server uses for devices such as NVRAM, Ethernet PROM, board registers, and the LCD" [1] §2.2.2 p. 6 — the six chip selects are the six devices 0x9–0xF above, and their full treatment is §3.5's. On the TNT machines device 0xC is the Sixty6 convolver and 0xB the RaDACal colormap bank ([grand-central.md](../tnt/grand-central.md) §2.5); the ANS has neither part, and the same two chip selects carry the LCD and nothing, respectively.

### 3.5 The GBUS devices in full

The GBus is Grand Central's 16-bit generic bus ([tnt.md](../tnt/tnt.md) §4.3); its TNT-side apertures and decode are [grand-central.md](../tnt/grand-central.md) §2.5's. The Network Server's six devices are this page's to document in full — no other page carries them.

#### 3.5.1 Board Register 1 (device 0xA, offset $1A000)

"These are the same locations as in the 9500, x1A000 and x1E000", with "the PCIPRSNT bits unchanged" and `ETH10BT_Link` "not supported" [1] §4.6.1 p. 15. What the ANS adds is the top byte — the keyswitch and box identity. All signals active low unless noted; "bits not mentioned are the same as in 9500" [1] §4.6.1 p. 15:

| Bit | Function |
|---|---|
| b10 | not connected |
| b11 | `BoxId0` = 1 |
| b12 | `BoxId1` = 0 |
| b13 | `Keyswitch ServiceL` |
| b14 | `Keyswitch LockedL` |
| b15 | `TwoSuppliesH` (active high) |

Two readings fall out. The `BoxId0`/`BoxId1` pair is a hard-wired box identifier — 1/0 on every Network Server — a cheap machine-ID strap independent of the 53C825A-probe identity path (§5.1); whether any software consumes it is not attested (§7.4). `TwoSuppliesH` is the register-level 500/700 discriminator: it reads set only on the 700's dual-supply backplane. The register is read **byte-wise**, at offsets +0 and +1 only, and the observed values pin the whole top byte: with the key locked it reads $28 on a 500 and $A8 on a 700 [7] (*observed* on full boots) — that is `BoxId0` (b11, $08) set and `BoxId1` (b12) clear, `Keyswitch ServiceL` (b13, $20) set (deasserted — not in service), `Keyswitch LockedL` (b14, $40) clear (asserted — locked), and `TwoSuppliesH` (b15, $80) set on the 700 only. With both active-low keyswitch lines reading 0 — no keyswitch fitted — POST instead prints `Key Sw. Service Mode` on LCD line 3 and takes the service path (*observed*), which pins the decoding direction: the bit clear is the position active.

#### 3.5.2 Board Register 2 (device 0xE, offset $1E000)

"Board Register 2 is now a 16 bit register. The top byte of this 16 bit register is where the Network Server specific server monitoring status bits are located. These are all active Low signals" [1] §4.6.1 p. 15:

| Bit | Function |
|---|---|
| b8 | `FanFailDrive` |
| b9 | `FanFailProcessor` |
| b10 | `TempFailProcessor` |
| b11 | `TempWarnProcessor` |
| b12 | `FailPowSupplyLeft` |
| b13 | `FailPowSupplyRight` |
| b14 | `powSupplyHotLeft` |
| b15 | `powSupplyHotRight` |

The register is **polled, never signalled** — "Network Server does not implement an interrupt for these functions. Software can implement a daemon that does background reads of this register (say every 30 seconds or so)" [1] §4.6.1 p. 15 — which makes it the simplest part of the machine to satisfy: a static all-healthy value (every bit set, i.e. every active-low line deasserted) is what a working machine shows. The production ROM's own consumer is the `check-over-temp` word, which reads $F301E000 — Board Register 2 — and tests mask $0400, the `TempFailProcessor` bit, active low [7] (*observed* in the detokenized firmware, corroborating the printed bit table). The low byte is the 9500's board register, whose own bits no Apple document publishes ([grand-central.md](../tnt/grand-central.md) §6).

#### 3.5.3 The LCD interface (device 0xC, offsets $1C000/$1C010/$1C020)

The front-panel LCD — the POST output device of §2.7 — hangs off GBus device 3 as a two-register port:

> "Network Servers provides a WRITE ONLY LCD interface as two registers on GBUS device 3. Software will have to provide a 1 microsecond (or longer) timer between write accesses. Register 0 at address offset x1C000 is the R (Command) register for the device; register 1 at address offset x1C010 is the S (Data) register for the device." [1] §4.6.1 p. 15

| Offset | Register |
|---|---|
| $1C000 | R — command |
| $1C010 | S — data |
| $1C020 | register 2 — bit 15 = timebase enable (§5.3) |

The command set the production ROM drives — backspace $08, linefeed $0A, init $0C, line selects $80/$C0/$94/$D4 — is consistent with an HD44780-class character-controller behind the port (*inferred - unverified*; no Apple document names the controller, and the four-line x 20-character layout of the observed messages [3] pp. 48–58 fits the class). Three contracts pin the model: the interface is **write-only** — an implementation must never be asked to read LCD state back; writes need **at least 1 µs** between them (the production ROM in fact uses a 1 ms delay word, a thousand-fold margin [7] *observed*); and the same GBus device also hosts the timebase-enable register at +$1C020, so device 0xC is really three registers, not two (§5.3). The firmware's LCD driver words are `lcd-reg`, `lcd-cmd!`, `lcd-data!` — and `lcd-cmd@`, a *read* word whose existence sits oddly against the write-only hardware, and is either vestigial or tolerated by the real device [7] (*observed* in the vocabulary; the contradiction is §7.7's).

The LCD is also an Open Firmware node — `/bandit/gc/lcd`, aliased `lcd` [7] (*observed*) — which Apple's abridged published device tree omits (§4.1).

#### 3.5.4 The Ethernet address PROM (device 0x9, offset $19000)

Sixteen bytes on $10 strides, publishing the hardware address in both byte orders [1] §4.6.1 pp. 14–15:

| Offset | Contents |
|---|---|
| $00/$10/$20 | Group ID, high/middle/low byte |
| $30/$40/$50 | hardware (sequencing) address, high/middle/low byte |
| $60 | $AA — signifies normal bit ordering |
| $70 | inverted XOR checksum of the first seven bytes |
| $80–$D0 | the six address bytes, inverted |
| $E0 | $55 — signifies reverse bit ordering |
| $F0 | non-inverted XOR checksum |

Apple's table prints the base as `xF0319000`, which is inconsistent with Grand Central at $F3000000 — a documentation defect; the correct base is $F3019000 ($F3000000 + $19000), and the same document independently cites "offset x19_000" for the device in its interrupt-map section [1] §4.6.1 pp. 14–15, §"Network Server External Interrupt Map" p. 16.

This device is also the **multiprocessor doorbell**: "the programmatic way for a second processor to interrupt the first processor (seen as EXT10 on Grand Central) is through accesses to the Ethernet Prom Gbus Device address space. Hence SecToPri_Int is tied to Enet ROM Chip Select (at offset x19_000) on GBUS" [1] §"Network Server External Interrupt Map" p. 16. Any access — read or write — into this PROM's address space raises the secondary-to-primary interrupt. There is no dedicated IPI register anywhere on the machine (§5.3).

#### 3.5.5 The nonvolatile stores: NVRAM and parameter RAM

The Network Server has **two** nonvolatile stores with different reset semantics, and Apple is unusually explicit about the split [1] §2.7 p. 8:

- **NVRAM** (GBus devices 0xD address / 0xF data — the banked two-aperture form of [grand-central.md](../tnt/grand-central.md) §2.6) holds "Boot Paths, POST results, Open Firmware patches, and much other information". "Removal of a battery from the Main Logic Board will reset all parameter and NVRAM to default values."
- **Parameter RAM** is "a separate part of a Power Monitor IC, and this is where date, time, and boot beep volume are stored".

The reset tools differ accordingly: "The fail-safe red button on the logic board resets parameter RAM but not NVRAM; it will also clear all pending power messages and set the Power Monitor IC to idle" [1] §2.7 p. 8 — the red button is the PRAM-and-power-state reset, battery removal is the everything reset, and the keyswitch position gates the keyboard reset (§2.6). The Power Monitor IC is the same class of part that watches the +5 V rail (§2.7).

#### 3.5.6 One undocumented GBus register

POST writes $FFFF to GBus device 3 offset +$30 ($F301C030) exactly once per boot, immediately after the `Testing Parity DIMMs` LCD message and immediately before the sized memory is reported [7] (*observed* on full boots of the production ROM). No Apple document describes the register or its bits; its position in the POST sequence makes a parity-error latch the obvious reading (*inferred - unverified*). An implementation must at minimum accept and discard the write.

## 4. Device roster

### 4.1 The silicon set

| Device | Part | OF node / unit | Location | Device page |
|---|---|---|---|---|
| Hammerhead — memory/cache controller | 343S1142 (343S1190 in secondary ANS coverage) | `hammerhead` | $F8000000 | [hammerhead.md](../tnt/hammerhead.md) |
| Bandit 1 — PCI host bridge | 343S1126 | `bandit@F2000000` | $F2000000 | [bandit.md](../tnt/bandit.md) |
| Bandit 2 — PCI host bridge | 343S1126 | `bandit@F4000000` | $F4000000 | [bandit.md](../tnt/bandit.md) |
| Grand Central — I/O controller, interrupt collector, DBDMA host, GBus host | 343S1125 | `gc@10` | Bandit 1 IDSEL 16, registers at $F3000000 | [grand-central.md](../tnt/grand-central.md) |
| 53C825A fast/wide SCSI 0 | Symbios/NCR 53C825A | `53c825@11`, `model NCR,825A`, `compatible pci1000,3` | Bandit 1 IDSEL 17 | [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) |
| 53C825A fast/wide SCSI 1 | Symbios/NCR 53C825A | `53c825@12` | Bandit 1 IDSEL 18 | [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) |
| Cirrus 54M30 video | Cirrus Logic (Alpine-family) | `54m30@F`, `compatible pci1013,a0` | Bandit 1 IDSEL 15, 1 MB DRAM | this page §2.5 |
| Curio cells — 53C94 SCSI, MACE Ethernet, ESCC serial | — | `53c94@1000`, `mace@11000`, `escc@13000` (+`ch-a@13020`, `ch-b@13000`) | Grand Central devices 0x0/0x1/0x2–0x3 | [grand-central.md](../tnt/grand-central.md) §2.5 |
| AWACS audio codec | — | `awacs@14000` | Grand Central device 0x4 | [grand-central.md](../tnt/grand-central.md) §2.5 |
| SWIM III floppy | — | `swim3@15000` | Grand Central device 0x5 | [grand-central.md](../tnt/grand-central.md) §2.5 |
| Cuda microcontroller + ADB | — | `via-cuda@16000`, `adb@0,0` (+`keyboard`, `mouse`) | Grand Central devices 0x6–0x7 | [grand-central.md](../tnt/grand-central.md) §2.5; [adb.md](../../hardware/adb.md) |
| Front-panel LCD | HD44780-class (inferred) | `lcd` at `/bandit/gc/lcd` | GBus device 0xC | this page §3.5.3 |
| Board Register 1 / BoxID + keyswitch | — | — | GBus device 0xA, $F301A000 | this page §3.5.1 |
| Board Register 2 / environment | — | — | GBus device 0xE, $F301E000 | this page §3.5.2 |
| Ethernet address PROM | — | — | GBus device 0x9, $F3019000 | this page §3.5.4 |
| NVRAM | — | `nvram` | GBus devices 0xD/0xF | this page §3.5.5; [grand-central.md](../tnt/grand-central.md) §2.6 |

The SCSI backplane and the RAID interface (§2.8) are wiring, not devices; the 53C825A pair and their hot-swap backplane are [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md)'s to register-level (its §1.3 carries the ANS placement; §4.5 the backplane contract).

### 4.2 The device tree

Apple's published tree for the machine [2] Ch. 6 p. 49 — the acceptance target for any implementation; if the firmware's tree does not have this shape, the machine model is wrong. Corrected for the document's own transcription defects (it prints the bridge addresses with nine hex digits, prints `12-cache` for `l2-cache`, and mis-nests `gc` and `54m30` outside the Bandit they sit on; the production ROM's own aliases — `screen` at `/bandit/54m30@F`, `lcd` at `/bandit/gc/lcd` — prove the nesting [7]):

```
/PowerPC,604@0
    /l2-cache@0,0
/memory@0
/AAPL,ROM@FFC00000
/bandit@F2000000
    /53c825@11
    /53c825@12
    /gc@10
        /53c94@1000
        /mace@11000
        /escc@13000
            /ch-a@13020
            /ch-b@13000
        /awacs@14000
        /swim3@15000
        /via-cuda@16000
        /adb@0,0
            /keyboard@0,0
            /mouse@1,0
        /lcd
    /54m30@F
/bandit@F4000000
    /53c875@10            (a plug-in example card, not built-in hardware)
/hammerhead@F8000000
```

Three facts the tree settles. **There is no `mesh` node** — the independent, published confirmation that MESH is absent (§2.1), from a source that owes nothing to ROM behaviour. The `gc` subtree — 53c94, mace, escc, awacs, swim3, via-cuda, adb — is **exactly the 9500's**; everything the TNT set documents about those cells applies unchanged. And the root publishes a single CPU node in the shipping configuration, `/PowerPC,604@0` with its `l2-cache` child — the SMP machine's tree shape is not published (§7.3).

## 5. Interrupt, bus, and clock architecture

### 5.1 The external interrupt remap

The platform's single-collector interrupt architecture is unchanged — every device reaches the processor through Grand Central's thirty-two-source block, one external line into the PowerPC, the VIA1 cascade behind source 18 ([tnt.md](../tnt/tnt.md) §5.1; [grand-central.md](../tnt/grand-central.md) §2.2, §3.2–§3.5). What the Network Server changes is the **board wiring of the eleven external lines**, and the change is the single most boot-critical delta in the machine. Apple's own table [1] §"Network Server External Interrupt Map" p. 16:

| Grand Central EXT | Power Macintosh 9500 | Network Server |
|---|---|---|
| EXT0 | `Cuda_NMI` | `Cuda_NMI` |
| EXT1 | Reserved | **`Error_Int`** — both Bandits ganged |
| EXT2 | `Ban1_Int` | **`FW0_Int`** — 53C825A 0 |
| EXT3 | `SlotA_Int` | `Slot1_Int` |
| EXT4 | `SlotB_Int` | `Slot2_Int` |
| EXT5 | `SlotC_Int` | `Slot3_Int` |
| EXT6 | `Ban2_Int` | **`FW1_Int`** — 53C825A 1 |
| EXT7 | `SlotD_Int` | `Slot4_Int` |
| EXT8 | `SlotE_Int` | `Slot5_Int` |
| EXT9 | `SlotF_Int` | `Slot6_Int` |
| EXT10 | `SecToPri_Int` | `SecToPri_Int` — the Ethernet-PROM doorbell |

Only three lines move, and they are all consequences of one decision — freeing the two Bandit positions for the fast/wide SCSI controllers: "The Network Server keeps the critical positions of PowerMac 9500; however, F/W SCSI interrupts are moved to Bandit's positions. Bandit 1 and 2 have their interrupts wired together and moved to the Error_Int. Bandit only interrupts on bus timeouts (see the Bandit spec section 2.5); as this is an error response, this should not impact much of anything" [1] p. 16. The six slot lines keep their positions and are merely renamed — `Slot1_Int`…`Slot6_Int` — so slot-interrupt wiring is still the 9500's shape with new labels. "Note that the internal Grand Central interrupt mapping is unchanged" [1] p. 16: sources 0–19 — the DBDMA channels, the chip interrupts, the VIA1 cascade — carry over exactly ([grand-central.md](../tnt/grand-central.md) §3.2).

This table is the mechanism behind the observation that a 9500 ROM will not boot a Network Server: a 9500 ROM programs EXT2/EXT6 as Bandit interrupts and treats EXT1 as reserved, so on ANS hardware its Bandit-error enables arm the two SCSI controllers' lines and the real Bandit error interrupt arrives on a line the 9500 ROM never enables. The reverse holds too — the machine's own ROM will not behave correctly without the remap.

### 5.2 The AIX interrupt contract

The interrupt number an AIX driver receives is not the EXT line. The device tree publishes `AAPL,interrupts` — an internal interrupt number, of the same kind and role as on the TNT machines ([bandit.md](../tnt/bandit.md) §3.9) — and AIX's configuration methods read that property, not the Grand Central EXT number: the worked 53C875 card example in slot 6 carries `interrupts 00000001` (the PCI pin) but `AAPL,interrupts 0000001D` (29), and it is the latter the OS consumes [2] Ch. 6. The general mapping from EXT line to `AAPL,interrupts` value is not printed anywhere; the one data point is consistent with the TNT source numbering, in which the external lines occupy sources 20–30 ([grand-central.md](../tnt/grand-central.md) §3.2): slot 6 sits on EXT9, and 20 + 9 = 29 = $1D (*inferred - unverified*; a single data point cannot establish a rule). AIX's interrupt subsystem was itself modified for the machine — "The Network Server's interrupt hardware is considerably different from that of the IBM RS/6000. The interrupt interfaces are the same, although the AIX interrupt subsystem has been modified to account for the hardware differences" [2] Ch. 5 p. 44 — as was its DMA model: "With the Network Server, each device has an individual DMA engine" [2] Ch. 5 p. 45, which is DBDMA and the 825A's own bus master as seen from AIX.

### 5.3 The system bus and the multiprocessor doorbell

The AR bus is the platform's — "a multiple master, multiple caching agent shared bus running at up to 50 MHz... essentially identical to the 9500 system bus, with the addition of hooks for more than one caching agent", with 32-byte coherence granularity, L2-forced L1 inclusion and four agent classes [1] §5.1 p. 17; the coherency and timing law is [hammerhead.md](../tnt/hammerhead.md) §3.5–§3.6. What only this document supplies is the arbitration wiring: the point-to-point `BusReqL`/`BusGrantL` request-grant discipline, bus parking, the per-slave `Rddal` and the `DbgL`/`SsdL`/`sysDBWOL` data-bus grants [1] §5.1.1 pp. 17–18 — the only arbitration definition any document in the platform's evidence set gives — and the pin counts in its tables, seven `BusGrantL` and eleven `SsdL`, which imply **seven masters and eleven slaves** on the connector's arbiter side (*inferred* from the pin counts). Parity errors on the bus, like those in memory, "generate MCP to the 60x" [1] §5.2 p. 18.

The multiprocessor bring-up order — both processors fetching $FFF00100, `WhoAmI` deciding primary from secondary, the secondary spinning until signalled — is [hammerhead.md](../tnt/hammerhead.md) §4.5's. The two ANS-specific mechanisms are:

- **The doorbell is an address.** There is no interprocessor-interrupt register: the secondary signals the primary by accessing the Ethernet PROM's GBus chip-select space, which raises `SecToPri_Int` on EXT10 (§3.5.4). A multiprocessor model must make *any* access into device 0x9's space raise the line — behaviour no 9500-shaped model has.
- **The timebases can be synchronized from a board register.** "Synchronize_TimeBase allows software to lock-step the timebases of the two processors... Software would have to write a 0 to this register, implemented at bit 15 of Register 2 at offset x1C020. This disables the 604 timebase. It would then write a '0' value to each processor's timebase. It would then write a 1 to bit 15 of Register 2 at offset x1C020, re-enabling the timebases" [1] §4.6.1 p. 16 — clear bit 15, zero both TBs, set bit 15. Apple hedges its own feature: "This may or may not be useful in two processor configurations" [1] §4.6.1 p. 16.

### 5.4 Clocks

| Clock | Value | Authority |
|---|---|---|
| Processor (AR) bus | 40–50 MHz, sourced **from** the processor card; 44 MHz on the 132 MHz card, 50 MHz on the 150/200 MHz cards | [1] §2.10 p. 9; the one-third rule of the 9500 family ([tnt.md](../tnt/tnt.md) §6.3); `150 MHz 604, 50 MHz Bus` on a real LCD [3] p. 53 |
| `refClk` / `clkToProc` | card-supplied, min 1 V/ns transition; returned ~700 ps in advance of the ASICs; ≥1 ns inserted delay for 0-hold processors | [1] §2.10 p. 9, §6.1 p. 21 |
| PCI buses | 33 MHz, asynchronous to the processor bus | [tnt.md](../tnt/tnt.md) §2.1 |
| DRAM timing | 60 ns with parity detected, 70 ns without; ~20% bandwidth penalty at 50 MHz for 70 ns | [1] §2.4.1 p. 7 |
| L2 "fast" mode | enabled by the ROM at bus speeds ≤ 44 MHz; one cycle less L2 latency | [1] §2.4.2 p. 8 |
| 54M30 frame buffer | extended RAS at 68 MHz clocking | [1] §2.8 p. 9 |
| Fast/wide SCSI | up to 40 MB/s per the pair, 20 MB/s wide synchronous per channel | [1] §2.9 p. 9; [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) §1.5 |
| External 53C94 | 25 MHz, as the platform's device tree publishes | [grand-central.md](../tnt/grand-central.md) §2.5 |
| LCD writes | at least 1 µs apart | [1] §4.6.1 p. 15 |
| 604 timebases | stoppable/restartable in lock-step via GBus $1C020 bit 15 | [1] §4.6.1 p. 16 |

The one clock fact that is a real behavioural switch rather than a rate is the parity/timing coupling: DRAM timing is set from two detected factors, bus speed and parity, so the same DIMM population runs at different timing on different cards. Whether the SCLK feeding the 53C825As' SCSI cores is derived from the system clock or a separate crystal is not stated in the evidence ([sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) §6).

## 6. Per-machine index

The two machine pages carry each box's full delta set against this page. The machine pages are stubs in this tree at the time of writing; this index plus the machine stubs is their current specification.

### 6.1 Apple Network Server 500/132 — [ans500.md](ans500.md)

The entry model: a 132 MHz PowerPC 604 card on a 44 MHz bus, 512 KB of L2, 32 MB of parity DRAM minimum, a single 325 W supply with no redundant option, a 2 GB fast/wide drive, DAT2 tape and the AppleCD 600i as the standard configuration [3] Basics/Configurations pp. 5–6. Its defining deltas against §2 are the two the register set exposes: one power supply (`TwoSuppliesH` clear in Board Register 1, §3.5.1) and the 44 MHz bus that turns fast L2 on (§2.4) — the only shipping machine on which the ROM enables it. The 200 MHz processor-card upgrade converts a 500 into the performance profile of a 700/200 (raising the bus to 50 MHz and turning fast L2 off again) [6].

### 6.2 Apple Network Server 700/150, 700/200 and 700/200SMP — [ans700.md](ans700.md)

The full-configuration model: 1 MB of L2, 48 MB minimum, a 425 W supply with the hot-swappable redundant second supply and its power backplane, and the two rear drive bays cabled to the bottom SCSI bus [3] Basics/Overview pp. 3–4, Basics/Overview of New Technologies p. 47; [1] §2.5.2 p. 8. The 700/150 runs a 604 at 150 MHz on a 50 MHz bus; the 700/200 substitutes the 200 MHz 604e ("a 33% increase in clock speed over the 150-MHz microprocessor" [5] p. 2), standardizes the 8x CD-ROM, and is installable as an upgrade in an existing 700 [6]; the 700/200SMP puts two 200 MHz 604e parts on the one card [3] Basics/Overview p. 3 — the shipping expression of the dual-processor design of §2.2, whose second processor no AIX release in evidence is known to enable (§7.3). Register-level, every 700 reads `TwoSuppliesH` set (§3.5.1); the box otherwise shares the 500's identity path entirely, and the two models are not distinguished by any Open Firmware mechanism in evidence.

## 7. Open questions

1. **The `AAPL,interrupts` rule.** Only one value is published — $1D for a slot-6 card on EXT9 — and the 20+EXT arithmetic that fits it is a deduction from the TNT source numbering (§5.2). AIX reads this property to build its ODM interrupt entries, so a wrong rule breaks device configuration, and no Apple document in evidence gives the general mapping.
2. **The exact Cirrus die behind "54M30".** The name matches no public Cirrus part; the Alpine CL-GD543X/4X register map and the 1 MB / 1024x768 fit point at the GD5430/GD5440 end of the family, but no document or dump confirms which die carries the marking, and whether it is a customer-specific variant of a stock part (§2.5).
3. **Whether AIX ever used the second processor.** The 700/200SMP shipped with two 604e parts and "enabling of the second processor... will be operating system dependent" [1] §3 p. 10; no AIX release in evidence is known to enable it, and the SMP machine's device-tree shape is unpublished (§4.2, §6.2).
4. **What consumes `BoxId0`/`BoxId1`.** Board Register 1's hard-wired box identifier (§3.5.1) is documented but no software in evidence reads it; the machine identity the ROM actually uses is the 53C825A probe. Whether the strap has a consumer at all is unknown.
5. **Which physical key drives Board Register 1.** The machine has a front panel keyswitch and a rear drawer lock; the Theory of Operations makes the *rear* lock's closed position a power-on precondition, the front keyswitch locks the security door, and the ROM's keyswitch words read Board Register 1 — no document states which physical switch sets bits 13–14 (§2.6, §3.5.1).
6. **The undocumented GBus register at +$1C030.** POST writes $FFFF there once per boot between the parity test and the memory report (§3.5.6); neither the register nor its bits appear in any Apple document.
7. **The `lcd-cmd@` contradiction.** The production ROM's LCD vocabulary contains a read word for a register Apple documents as write-only (§3.5.3); either the word is vestigial or the real device tolerates reads, and no document says which.
8. **The front-panel LCD controller's identity.** The command set is HD44780-class by inference only (§3.5.3); no document names the part, its geometry beyond the observed four lines, or its initialization requirements beyond the port contract.
9. **The 53C825A's SCLK.** The chip needs at least a 40 MHz SCSI core clock for fast transfers; whether the ANS board derives it from the system clock or a separate source is not stated (§5.4; [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) §6).
10. **The unchecksummed fourth megabyte.** The prototype ROM's Apple checksum verifies over its first 3 MiB only, exactly as the 9500 family's ROMs do, leaving a fourth megabyte of real content outside the checksum; what occupies it — and whether the production ROM checksums the same span — is unestablished (§1.3).
11. **The "(TBD) August 1996" decode date.** Apple's own 512 MB sentence carries an unresolved date for when production ROMs provide the decoding (§2.3); whether any ROM revision decodes more than 512 MB is unknown.
12. **Board Register 1's low bytes on the ANS.** "Bits not mentioned are the same as in 9500" — but the 9500's own bits (beyond PCIPRSNT and the factory-test strap) are themselves unpublished ([grand-central.md](../tnt/grand-central.md) §6), so the ANS register's low bytes are doubly undocumented.
13. **The Grand Central, Hammerhead and Bandit ERS documents.** Apple's Network Server notes name the "Grand Central ERS" as the register-level reference [1] §4.6.1 p. 13; none of the three was ever published ([tnt.md](../tnt/tnt.md) §7, item 2). They would close the register gaps this page inherits from the platform.
14. **The Network Server 300 and the later ROM family.** An unshipped rack-mount model and at least one further ROM revision exist by name in secondary material and inside the prototype ROM's build string (§1.2); no hardware description or ROM image for either is in evidence.

## References

1. Apple Computer, Inc., *Network Server Hardware Developer Notes* (Apple Network Server 500/700), c. 1996 — Chapter 1: §1 p. 2 (server positioning; the "not one-stop shopping" framing; product family), §1.1.1 p. 2 (not 64-bit ready), §1.1.2 p. 2 (price-point differentiation), §1.2.1 p. 2 (target OS), §1.3 p. 2 (FCS feature list); Chapter 2: §2.1 p. 4 (block diagram, "CPU CARD — Supports 2 CPU's"), §2.2.2 p. 6 (GBUS, six chip selects), §2.3–§2.3.3 pp. 6–7 (Safe Server thermal trips), §2.4 p. 7 (logic board; bank labels; 3 A startup), §2.4.1 p. 7 (parity memory; 60/70 ns timing; the 512 MB decode sentence; device rules), §2.4.2 p. 8 (cache memory; fast L2), §2.5 pp. 7–8 (power supply budgets), §2.6 p. 8 (keyswitch), §2.7 p. 8 (NVRAM and PRAM), §2.8 p. 9 (on-board video), §2.9 p. 9 (fast/wide SCSI), §2.10 p. 9 (low-skew clocking), §2.11 p. 9 (processor cards); Chapter 3: §3 p. 10 (initialization; primary/secondary; the OS-dependent second processor); Chapter 4: §4 p. 11 (address map; "two separate PCI buses"), §4.1 p. 11 (bridge number encoding), §4.2 p. 12 (pass-through cycles), §4.2.1 p. 12 (Grand Central device registers; re-positionable response), §4.3 p. 12 (I/O cycles; the configure-before-I/O rule), §4.4 pp. 12–13 (configuration cycles; the config ports; discovery order), §4.6.1 pp. 13–16 (Grand Central window; DMA channel and device tables; GBUS devices; Board Registers 1 and 2; LCD interface; Synchronize_TimeBase; Ethernet PROM layout), §4.6.2 p. 16 (other PCI devices; IDSEL), §"Network Server External Interrupt Map" p. 16 (the EXT table; the 54M30's missing line; the SecToPri/Enet-ROM tie); Chapter 5: §5.1 pp. 17–18 (system bus; agent types; arbitration signals and pin counts), §5.2 p. 18 (parity handling); Chapter 6: §6.1 pp. 19–21 (processor slot pinout and notes; current limits; OffSenseOut; reset timing; ParityErrL), §6.2 p. 21 (cache DIMM; burst-SRAM erratum), §6.3 p. 21 (power connector), §6.4 p. 21 (mezzanine); Chapter 7: §7.1 p. 22 (PCI definition; 2.1-compliance caveats; Cache Line Size guidance), §7.1.2 p. 22 (slot power), §7.2 pp. 23–27 (SCSI backplane, hot swap, RAID interface, drive requirements), §7.3.1 p. 27 (drive power); Chapter 8: §8.1 p. 30 (the Hendy enclosure).
2. Apple Computer, Inc., *Developer's Reference Guide for the Apple Network Server*, 1996 — Chapter 5 pp. 44–46 (the AIX port: PCI and Open Firmware; "considerably different" interrupt hardware; per-device DMA engines; the m_nextpkt convention; what is unchanged from IBM AIX); Chapter 6 pp. 48–49 (the Open Firmware device tree, IEEE 1275 conformance, Listing 6-1, the name/reg guarantee and the ODM PdDv requirement) and the worked 53C875 `.properties` example (vendor/device IDs, `AAPL,interrupts 0000001D`, `AAPL,slot-name SLOT6_PCI1`, the `reg`/`assigned-addresses` values); Chapter 7 (device hierarchy and ODM databases); Chapter 8 (`busresolve` not supported; address translation and programmed I/O).
3. Apple Computer, Inc. (Service Source), *Network Server 500/700 Series* service manual — Basics/Overview pp. 1–4 (the three models and their features, including the 700/200SMP's two 604e parts, cache sizes, RAM minimums, drive bays, AAUI Ethernet, the three SCSI buses, the redundant supplies); Basics/Configurations pp. 5–7 (standard configurations); Basics/Setup and Operation pp. 19–23 (turning the server on; the front view); Basics/Overview of New Technologies pp. 47–58 (hot-swappable supplies; the LCD progress messages of Table 1, the completed display, the problem messages of Tables 2 and the DIMM-slot dash table).
4. Apple Computer, Inc. (Service Source), *Network Server Theory of Operations* — pp. 1–4 (the power-on chain: AC path, DC path and the trickle +5 V to Cuda, the power-controller preconditions including the rear keyswitch and +4.7 V monitor), pp. 5–8 (POST and the LCD; Open Firmware's boot-device search and the service-position first-boot rule; the bootapple/bosboot chain; the system boot quack and the interrupt milestone).
5. Apple Computer, Inc., *What's New With the Network Server* — p. 2 (the 700/200 and its 604e; AIX 4.1.4.1), p. 3 (512 MB RAM support; Ultra-SCSI drive support; the new-hardware list).
6. Apple Computer, Inc., *Installing the Network Server 200-MHz Processor Card Upgrade* — the card-swap procedure (the rear drawer lock, the card guides, seating), establishing the 200 MHz card as a field upgrade for the 500 and 700.
7. Apple Network Server 500/700 boot ROM, production release (Open Firmware 1.1.22) — 4 MB image, header checksum $962F6C13, version field $077D.28F2 (identical to the Power Macintosh 9500 v2 ROM's; checksums differ), verified byte-for-byte between a serial capture from hardware and a packaged dump. Evidence used: the detokenized Open Firmware source — the `?esb`-gated root `compatible` (`AAPL,ShinerESB` + `MacRISC`), the `825a?` revision-ID probe and `?esb-evt2`, the `install-825s`/`install-54m30` device installation, the `screen` (`/bandit/54m30@F`) and `lcd` (`/bandit/gc/lcd`) aliases, the `disk0`–`disk6` aliases over `/bandit/53c825@11` and `@12`, the keyswitch words (`get-keyswitch`, `(key=locked?`, `(key=service?` — locked 1, service 2), `check-over-temp` reading $F301E000 mask $0400, `setenv-aix`/`setenv-netware`/`setenv-monitor`, the `1ms` LCD delay and the `lcd-cmd@`/`lcd-cmd!`/`lcd-data!` words, the `aix-boot`/`xcoff-loader`/`iso-9660-files` packages, the `MacOS is not supported` string; and boot-time observation of the 1.1.22 ROM: the byte-wise Board Register 1 reads ($28 on a 500, $A8 on a 700, key locked; `Key Sw. Service Mode` with both keyswitch lines low), the $FFFF write to GBus $F301C030 after `Testing Parity DIMMs`, `?esb .` printing true after the 53C825A probe, the service-position/blank-NVRAM install-CD path and its Cuda system-reset requirement (`RESETing to change Configuration!`, `Can't reset-all` on failure), and the `bootapple` output (`POST results AOK. Code is 00010000`, `AAPL,cpu-id 39002089`, the ShinerESB model line, the `/bandit/53c825@11/sd@0:aix` boot device).
8. Apple Network Server boot ROM, unreleased prototype ("2.0", image dated 1998-01-14) — 4 MB image, header checksum $49B2BE8F, version field $077D.7DD0, reassembled from four byte-swapped 16-bit chip lanes of the 64-bit ROM DIMM (the Apple checksum verifies over the first 3 MiB, the family's own convention). Evidence used: the Mac OS driver names (`.Display_Video_Cirrus_54M30`, `SimNCR53c8xx`), the Open Firmware 2.0 version string, the `AAPL,7500`/`8500`/`9500`/`7300` model table with no `ShinerESB` string, the retained `aix-boot`/`aix-block0` words, and the build string naming the unshipped "ANS 300" alongside the 500 and 700.
9. AIX 4.1.5 for the Apple Network Server, BOS installation CD (1997) — evidence used: the ISO9660 volume's IBM IPL record at block 0 (EBCDIC `IBMA`, image at block $14A4, $124 blocks, load and entry at $0011E124); the ODM `PdDv`/`PdAt` stanzas (the `bandit`, `gc`, `pci1000,3`, `pci1000,f`, `pci1013,a0`, `dac960` and PNP0A03 filesets; `gc`'s `children` attribute naming `53c94`, `mace`, `ch-a`, `ch-b`, `swim3`, `via-cuda`; the absence of `awacs`, `lcd` and `adb` entries); the driver set (`pscsidd`, `pscsiddpin`, `cirrusdd`, `hscsidd`, `rsdd`) and the `/usr/lib/methods/` configuration methods (`cfggc`, `cfgpscsi`, `cfgcirrus`, `cfgsys`, and `cfggc`'s `resolve_pci_mem_space`); and observed guest behaviour of the installed system (the SCSI bus walk of the empty bays and its per-ID cost, as recorded on the [sym53c8xx.md](../../hardware/scsi/sym53c8xx.md) page §4.3).
10. Apple Computer, Inc., *Using PCI RAID Card: For the Apple Network Server* — Chapter 1 (the AIX view of the RAID controller: ID 7, logical drive IDs 8–15, the `:aix` boot-path suffix) and the boot-path table (slot 1 `pci1/dac960@d` through slot 6 `pci2/dac960@10`), independently confirming the slot-to-bridge split of §3.3.
