# The Symbios/NCR 53C8xx PCI SCSI controllers (53C825A-class) and the SCRIPTS processor

**Contents:**

1. [Overview](#1-overview) — what the part is, the three cores, the 53C8xx family and its names, which machines carry it, endianness and clocking
2. [Register file](#2-register-file) — the four PCI address spaces, configuration registers, the full operating-register map, per-register detail
3. [Behaviour](#3-behaviour) — the SCRIPTS instruction set, DMA engine and FIFOs, the SCSI core, selection/reselection, timers, the interrupt model, selection time-out
4. [Programming model](#4-programming-model) — discovery, register initialization, the SCRIPTS program pattern, how the Network Server's AIX driver drives the chip, resets
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **53C8xx family** — designed by NCR Microelectronics, sold as **Symbios Logic** after the 1996 spin-off, and rebranded **LSI Logic** from 1998 — are *PCI-SCSI I/O Processors*: single-chip PCI host adapters that merge a SCSI protocol controller, a PCI bus-master DMA engine and a small execution engine into one part [1] Ch. 1. This page covers the **53C825A**, the part that the Apple Network Server carries two of, and its close siblings; where the family differs (the 53C875 Ultra part) the differences are called out explicitly.

The defining property of the family is the **SCRIPTS processor**. Where the earlier Macintosh SCSI controllers run one bus sequence per register write (see [ncr-53c96.md](ncr-53c96.md)) or leave the protocol to the CPU entirely ([ncr-5380.md](ncr-5380.md)), the 53C8xx fetches a *program* — eight- or twelve-byte instructions, in system memory or in 4 KB of on-chip RAM — and executes SCSI algorithms autonomously. Symbios's own framing: "The SCRIPTS processor can begin a SCSI I/O operation in approximately 500 ns. This compares with 2–8 ms required for traditional intelligent host adapters" [1] § 2.2.3. Apple's is terser: the Network Server's controllers "use SCRIPTS based DMA for high performance with low overhead" [5] § 2.9.

The chip is built of three blocks [1] § 2.2:

| Block | Function | Key parameters |
|---|---|---|
| SCSI core | 8- or 16-bit SCSI-2 bus, SE or differential | 20 MB/s synchronous, 10 MB/s asynchronous (16-bit bus); sync offset to 16; TolerANT drivers/receivers |
| DMA core | 32-bit PCI bus master | 536-byte DMA FIFO; bursts of 2/4/8/16/32/64/128 dwords; >110 MB/s zero-wait-state bursts at 33 MHz; >47 MB/s memory-to-memory |
| SCRIPTS processor | Fetches and executes SCRIPTS from memory or internal RAM | 4 KB internal RAM; 8-dword instruction prefetch; interrupt-only interaction with the host |

Physical facts: 160-pin plastic quad flat pack; 5 V supply (4.75–5.25 V), 130 mA dynamic, 0–70 °C free air [1] § 6.1 Tables 6.1–6.2. The "825AJ" variant trades four pins for JTAG boundary scan; the "825AE" adds PCI power management [1] § 1.2.

### 1.2 Three vendors, four part names

The same silicon carries several names, and Apple's own documents use all of them. The Network Server Hardware Developer Notes say "Symbios Logi 53C825A" in § 2.9, "Symbios Logic 53C825A" in § 4.6.2, "Symbios Logic 53C825" in § 7.1.1, and "NCR 825A" in § 7.2.1 — and, in one sentence of § 7.2.1, even "the 820 device" (evidently a typo for the 825A; see § 5). The device's Open Firmware node is `53c825` and its `model` property is `NCR,825A` [7].

The 53C825A is a *die revision* of the 53C825, "a pin-for-pin replacement" with software enhancements [1] § 1.1 — and critically, it keeps the 53C825's PCI device ID:

> "The SYM53C825A device ID is 0003h. This value is the same as in the SYM53C825, since the 53C825A is a drop-in replacement. The devices are uniquely identified in the upper nibble of the Revision ID register." [2] Ch. 3, Register 02h

So software cannot tell the parts apart from the device ID; it must read configuration offset `0x08`, the **Revision ID**. The later LSI-branded manual prints the value per part: the LSI53C825A's Revision ID is **0x14** and the LSI53C825AE's is **0x26** [1] § 4.1, Register 0x08. The byte splits cleanly: upper nibble = part identity (1 = 825A, so bit 4 is set), lower nibble = silicon revision level, and the operating register `CTEST3[7:4]` "should have the same value as the lower nibble of the PCI Revision ID register" [1] § 4.2, Register 0x1B. The ANS production ROM relies on exactly this: its identity word for the part masks the Revision ID with `0x10` and arms a later code path only when the bit is set (*observed* in the ROM's 825A probe; a 53C825 with a Revision ID below 0x10 would not take that path) [7].

A second identity register exists in the operating file: `MACNTL[7:4]`, "Chip Type", is fixed at **0x06** for the devices covered by the LSI53C825A manual [1] § 4.2, Register 0x46.

### 1.3 Which machines carry it

| Machine | Controllers | PCI position | Notes |
|---|---|---|---|
| Apple Network Server 500/700 (codename Shiner) | **two 53C825A**, single-ended | Bandit 1, IDSEL 17 and 18 — OF units `53c825@11`, `53c825@12` [5] §§ 4.6.2, 7.1.1; [6] | the machine's fast/wide internal buses; MESH is absent, and a third *narrow* bus (the 53C94-class Curio device, Grand Central DMA channel 0) serves the external DB-25 [5] § 4.6.1 device table |
| PCI Power Macintosh (Bandit platforms, e.g. Power Macintosh 7500/8500/9500) | **53C875** as an add-in card | any PCI slot; Apple's worked example is `53c875@10` (IDSEL 16) on Bandit 2 [6] | the production ROM natively recognizes `pci1000,f` and names the node `53c875`, `model` `NCR,875` (*observed* in the ROM's model check) [7] |

No PCI Power Macintosh carries a 53C8xx controller on the main logic board — the built-in fast bus of that generation is MESH or Curio, and the 53C8xx arrives only as a card. The Network Server is the one Apple machine that builds the family in, twice, and its boot behavior depends on them existing (§ 4.1).

### 1.4 The family, and what differs

| Part | What it is | Distinguishing facts |
|---|---|---|
| 53C825 | the original fast/wide part | device ID 0003h [2] |
| **53C825A** | die revision, "drop-in" | device ID 0003h, Revision ID **0x14** [1] § 4.1; the ANS part |
| 53C825AJ | JTAG variant | JTAG signals *replace* TESTIN, MAC/_TESTOUT, BIG_LIT/ and SDIRP1; **little-endian only** [1] §§ 1.2, 2.4.4–2.4.5 |
| 53C825AE / 825AJE | PC 97 power-management variant | Revision ID 0x26; PCI capability at configuration 0x34 pointing to power-management registers at 0x40; D0/D3 states only [1] §§ 1.1, 2.5 |
| **53C875** | pin-compatible Ultra (Fast-20) part | device ID **000Fh** [3] Ch. 3; "pin-for-pin replacement for the SYM53C825 … with added support for the SCSI-3 Ultra standard"; 20 MB/s narrow / 40 MB/s wide synchronous when Ultra is enabled; Fast-20 Enable bit and an internal SCSI clock doubler [3] Ch. 1 |

Because the 875 is socket- and largely software-compatible with the 825A [3] Ch. 1, everything in § 2 and § 3 applies to it except the Ultra-specific bits (Fast-20 Enable in SCNTL3 bit 7, the clock doubler in STEST1/STEST3), which the 825A lacks [4] Table 6-8.

### 1.5 Performance claims, reconciled

Apple states the Network Server's "two fast/wide SCSI channels (up to 40 Mbytes/sec)" [5] § 2.9. The vendor manual's per-chip ceiling is "SCSI synchronous transfer rates up to 20 Mbytes/s, and asynchronous transfer rates up to 10 Mbytes/s on a 16-bit wide SCSI bus" [1] § 2.2.1 — 20 MB/s is also the 825A benefit-list figure for fast/wide synchronous [1] § 1.4.1. The two statements are consistent only if Apple's 40 MB/s is the *pair's aggregate* — two independent 20 MB/s channels (*inferred*; Apple's sentence is ambiguous, and no Apple document breaks the number out per channel).

### 1.6 Endianness — a pin, not a register

The chip supports both byte orders, selected by the **BIG_LIT/ pin**: in big-endian mode "the first byte of an aligned SCSI to PCI transfer is routed to lane three and succeeding transfers are routed to descending lanes"; in little-endian mode, lane zero ascending; the same rule applies to the external ROM interface [1] § 2.4.5; [2] BIG_LIT/ signal. The Network Server is a big-endian (PowerPC, AIX) host, so its 825As are strapped big-endian; a Macintosh-world 53C875 card may be strapped either way. The 53C825AJ is little-endian only because the JTAG pins consume BIG_LIT/ [1] § 1.2.

The pin moves *addresses*, not registers: "The registers always appear on the same byte lane, but the address of the register is repositioned" [1] § 2.4.5. Concretely, each operating register has two addresses — the big-endian offset (SCNTL0 = 0x00) and the little-endian offset (SCNTL0 = 0x80) — and Symbios's guidance is to write drivers against logical names with an equate that flips between the two [1] § 2.4.5. This page uses the **big-endian offsets** throughout, matching a big-endian host; the little-endian value is given in parentheses in the register map. From inside a SCRIPTS instruction, register addressing is endian-independent — "Internally, the LSI53C825A always operates in little endian mode" [1] § 2.4.5.

### 1.7 Clocking

Two clocks feed the part. **CLK** is the PCI bus clock (33 MHz class) and optionally the SCSI core clock too — though "the LSI53C825A is not able to achieve Fast SCSI transfer rates" from CLK alone [1] § 3.1.1. **SCLK** (pin 56) "is used to derive all SCSI-related timings" [1] § 3.1.6; for 10 MB/s (20 MB/s wide) Fast SCSI transfers "at least a 40 MHz external SCLK must be provided", or STEST1 bit 7 can be set to disable the external pin and run the SCSI clock from CLK [1] § 4.2, Register 0x4D. SCLK is then divided for the SCSI core by the CCF field (asynchronous core) and again by the SCF field (synchronous core), both in SCNTL3 [1] § 4.2, Register 0x03. The Network Server platform supports 40–50 MHz system clocks [5] § 2.10; the exact SCLK fitted to the ANS logic board is not stated in the evidence (§ 6).

## 2. Register file

### 2.1 Address spaces

The chip answers in four places [1] § 2.1.1:

| Space | Size | Assigned by | Contents |
|---|---|---|---|
| PCI configuration space | 256 bytes | IDSEL chip select | lower 128 bytes: standard PCI configuration registers; **upper 128 bytes: a second image of the operating registers** |
| I/O space | 256-byte block | Base Address Zero (BAR0, config 0x10) | operating registers |
| Memory space | 256-byte block | Base Address One (BAR1, config 0x14) | operating registers (same file, memory decode) |
| SCRIPTS RAM | 4 KB | RAM Base Address register (BAR2, config 0x18) | the internal 4 KB RAM, byte-accessible from PCI; CPU accesses take 3 wait states instead of the 5 of a register access [1] § 2.2.4 |

The operating registers are "available in both the upper and lower 128-byte portions of the 256-byte space selected" [1] § 2.1.1 — i.e. within any of the three windows, offsets 0x00–0x7F and 0x80–0xFF are two endian views of the same file (§ 1.6). There is also an external memory interface for an expansion ROM, up to 1 MB in binary steps from 16 KB, sized by MAD[3:1] strapping sensed at reset [1] § 2.3.

One load-bearing rule governs the whole file:

> "The only register that the host CPU can access while the LSI53C825A is executing SCRIPTS is the Interrupt Status (ISTAT) register; attempts to access other registers will interfere with the operation of the chip. However, all operating registers are accessible with SCRIPTS." [1] § 4.2

And its mirror: "The LSI53C825A cannot fetch SCRIPTS instructions from the operating register space. Instructions must be fetched from system memory or the internal SCRIPTS RAM" [1] § 4.2.

### 2.2 Configuration registers (offsets in configuration space)

| Offset | Register | Value / behavior |
|---|---|---|
| 0x00 | Vendor ID | 0x1000 (Symbios Logic / NCR / LSI) [2] Ch. 3 |
| 0x02 | Device ID | **0x0003** for 53C825 and 53C825A alike [2] Ch. 3; 0x000F for the 53C875 [3] Ch. 3 |
| 0x04 | Command | PCI command control; bit 4 (Write and Invalidate enable) participates in W&I issue conditions [1] § 2.1.3.5 |
| 0x06 | Status | standard [1] § 4.1 |
| 0x08 | **Revision ID** | 0x14 (825A), 0x26 (825AE) — upper nibble part identity, lower nibble revision [1] § 4.1; [2] Ch. 3 |
| 0x09 | Class Code | 0x010000 — SCSI controller [1] § 4.1 |
| 0x0C | Cache Line Size | cache-line logic enable with DCNTL.CLSE [1] § 4.1 |
| 0x0D | Latency Timer | standard [1] § 4.1 |
| 0x0E | Header Type | 0 [1] § 4.1 |
| 0x10 | Base Address Zero (I/O) | 256-byte I/O window [1] § 4.1 |
| 0x14 | Base Address One (Memory) | 256-byte memory window [1] § 4.1 |
| 0x18 | RAM Base Address (SCRIPTS RAM) | the 4 KB internal RAM; enabled by the MAD5 pin (a 4.7 kΩ resistor from MAD5 to VSS *disables* it) [1] §§ 2.2.4, 4.1 |
| 0x2C / 0x2E | Subsystem Vendor ID / ID | on 825A Revision G, selected by MAD(4)/MAD(6) strapping among four configurations (normal: 0x0000/0x0000); "chip revisions before Revision G … are hard wired to zero values" [2] Ch. 4 Tables 4-11/4-12 |
| 0x30 | Expansion ROM Base Address | external ROM window [1] § 4.1 |
| 0x34 | Capability Pointer | 825AE only; points at the power-management capability at 0x40 [1] § 4.1 |
| 0x3C | Interrupt Line | writable, for the OS to record routing [1] § 4.1 |
| 0x3D | **Interrupt Pin** | fixed 0x01 = INTA/ [1] § 4.1 |
| 0x3E / 0x3F | Min_Gnt / Max_Lat | 0x11 and 0x40 (units of 0.25 µs) [1] § 4.1 — matching the `min-grant`/`max-latency` properties of Apple's 53C875 example, 0x08/0x40 [6] |
| 0x40–0x47 | Power Management registers | 825AE only: next-item pointer, capabilities (PME support, D1/D2 unsupported), and the two power-state bits at 0x44 (00 = D0, 11 = D3) [1] §§ 2.5, 4.1 |

Configuration cycles are only recognized with IDSEL asserted, and AD[10:8] must be zero [1] § 2.1.1.

### 2.3 Operating register map

Byte offsets below are the big-endian addresses; the little-endian address is in parentheses. Power-on/reset defaults are shown where the manual prints them [1] § 4.2, Table 4.2 and per-register diagrams.

| Offset (LE) | Registers |
|---|---|
| 0x00–0x03 (0x80–0x83) | SCNTL3 · SCNTL2 · SCNTL1 · SCNTL0 |
| 0x04–0x07 (0x84–0x87) | GPREG · SDID · SXFER · SCID |
| 0x08–0x0B (0x88–0x8B) | SBCL · SSID · SOCL · SFBR |
| 0x0C–0x0F (0x8C–0x8F) | SSTAT2 · SSTAT1 · SSTAT0 · DSTAT |
| 0x10–0x13 (0x90–0x93) | DSA (32-bit Data Structure Address) |
| 0x14 (0x94) | **ISTAT** (Interrupt Status) |
| 0x15–0x17 (0x95–0x97) | Reserved |
| 0x18–0x1B (0x98–0x9B) | CTEST3 · CTEST2 · CTEST1 · CTEST0 |
| 0x1C–0x1F (0x9C–0x9F) | TEMP (32-bit Temporary) |
| 0x20 (0xA0) | DFIFO (DMA FIFO byte-offset counter) |
| 0x21–0x23 (0xA1–0xA3) | CTEST6 · CTEST5 · CTEST4 |
| 0x24–0x26 (0xA4–0xA6) | DBC (24-bit DMA Byte Counter) |
| 0x27 (0xA7) | DCMD (DMA Command) |
| 0x28–0x2B (0xA8–0xAB) | DNAD (32-bit DMA Next Address) |
| 0x2C–0x2F (0xAC–0xAF) | **DSP** (32-bit DMA SCRIPTS Pointer) |
| 0x30–0x33 (0xB0–0xB3) | DSPS (32-bit DMA SCRIPTS Pointer Save) |
| 0x34–0x37 (0xB4–0xB7) | SCRATCHA |
| 0x38 (0xB8) | DMODE (DMA Mode) |
| 0x39 (0xB9) | DIEN (DMA Interrupt Enable) |
| 0x3A (0xBA) | SBR (Scratch Byte Register) |
| 0x3B (0xBB) | DCNTL (DMA Control) |
| 0x3C–0x3F (0xBC–0xBF) | ADDER (Adder Sum Output, read-only) |
| 0x40–0x41 (0xC0–0xC1) | SIEN1 · SIEN0 |
| 0x42–0x43 (0xC2–0xC3) | SIST1 · SIST0 (read-to-clear) |
| 0x44 (0xC4) | SLPAR (SCSI Longitudinal Parity) |
| 0x45 (0xC5) | SWIDE (SCSI Wide Residue) |
| 0x46 (0xC6) | MACNTL (Memory Access Control) |
| 0x47 (0xC7) | GPCNTL (General Purpose Pin Control) |
| 0x48–0x49 (0xC8–0xC9) | STIME1 · STIME0 |
| 0x4A–0x4B (0xCA–0xCB) | RESPID1 · RESPID0 |
| 0x4C–0x4F (0xCC–0xCF) | STEST3 · STEST2 · STEST1 · STEST0 |
| 0x50–0x51 (0xD0–0xD1) | SIDL (SCSI Input Data Latch, 16-bit) |
| 0x52–0x53 (0xD2–0xD3) | Reserved |
| 0x54–0x55 (0xD4–0xD5) | SODL (SCSI Output Data Latch, 16-bit) |
| 0x56–0x57 (0xD6–0xD7) | Reserved |
| 0x58–0x59 (0xD8–0xD9) | SBDL (SCSI Bus Data Lines, 16-bit, read-only) |
| 0x5A–0x5B (0xDA–0xDB) | Reserved |
| 0x5C–0x5F (0xDC–0xDF) | SCRATCHB |
| 0x60–0x7F (0xE0–0xFF) | SCRATCHC through SCRATCHJ (eight 32-bit scratch registers) |

The rest of this section gives the per-register detail. Bits marked reserved "should always be written to zero; mask all information read from them" [1] § 4.2.

### 2.4 SCSI control registers

**SCNTL0 (0x00) — SCSI Control Zero**, reset default 0xC0. Bits: ARB[1:0] (7:6), START (5), WATN (4), EPC (3), reserved (2), AAP (1), TRG (0).

- **ARB[1:0]** selects the arbitration mode: 00 = *simple* arbitration, 11 = *full arbitration with selection/reselection*, other encodings reserved. Simple arbitration asserts BSY and the chip ID after Bus Free and expects the CPU to read SBDL and check for a higher-priority ID itself; on SSEL/ from another device it deasserts and sets SSTAT0.LOA. Full arbitration repeats "until it wins control of the SCSI bus", sets SSTAT0.WOA, performs selection (asserting SSEL/, the target ID from SDID and its own ID from SCID), sets SIST0.CMP on completion — and sets SIST1.STO "if a selection time-out occurs" [1] § 4.2, Register 0x00.
- **START** (5) kicks the arbitration sequence; "during SCSI SCRIPTS operations, this bit is controlled by the SCRIPTS processor". It self-clears when the sequence completes, and "should not be started" while SCNTL1.CON indicates a connection [1] ibid.
- **WATN** (4) makes the chip assert SATN/ during selection — the initiator's "I have a message" flag; SATN/ deasserts together with SSEL/ if the selection times out [1] ibid.
- **EPC** (3) enables odd-parity checking of SCSI receives (and of SCSI-FIFO-to-DMA-FIFO moves); parity errors set SIST0.PAR. In initiator mode a parity error may optionally raise SATN/ "but the transfer continues until the target changes phase" [1] ibid.
- **AAP** (1) auto-asserts SATN/ on a parity error, asserted "before deasserting SACK/ during the byte transfer with the parity error"; requires EPC [1] ibid.
- **TRG** (0) sets the default role; in SCRIPTS operation it is driven by SET TARGET / CLEAR TARGET, and the manual cautions that writing it while disconnected "may cause the loss of a selection or reselection" [1] ibid.

**SCNTL1 (0x01) — SCSI Control One**, default 0x00. Bits: EXC (7), ADB (6), DHP (5), CON (4), RST (3), AESP (2), IARB (1), SST (0).

- **EXC** (7) adds one clock of data setup on SCSI *sends* — design margin at a rate cost.
- **ADB** (6) forces the SODL contents onto the SCSI data bus (role-gated by SCSI I/O); diagnostic/low-level only, "clear this bit when executing SCSI SCRIPTS".
- **DHP** (5, target only) stops the transfer halting on SATN/ or parity error; when clear, the chip "may transfer up to three additional bytes before halting to synchronize between internal core cells", and in synchronous mode it drains outstanding offset first.
- **CON** (4) is set by hardware on any connection (after successful selection/reselection or won arbitration) and is writable "primarily during loopback mode".
- **RST** (3) asserts the SCSI RST/ line *until cleared* — the 25 µs SCSI minimum "must be timed out by the controlling microprocessor or a SCRIPTS loop" (§ 4.4).
- **AESP** (2) forces even parity, i.e. deliberately bad parity on every byte sent, for error-handling tests.
- **IARB** (1) begins arbitration immediately on Bus Free *following an expected disconnect*, for multithreaded initiator stacks; it self-clears when the subsequent selection/reselection completes or times out, and an unexpected disconnect clears it without arbitrating. The manual spells out the abort dance: set ISTAT.ABRT, then either WOA appears (reset IARB to finish the abort and disconnect) or the chip loses arbitration (detected by IARB clearing — not by SSTAT0.LOA) [1] ibid.
- **SST** (0) starts a SCSI transfer with REQ/ACK handshaking; "automatically set during SCRIPTS execution and should not be used".

**SCNTL2 (0x02) — SCSI Control Two**, default 0x00. Bits: SDU (7), CHM (6), SLPMD (5), SLPHBEN (4), WSS (3), VUE0 (2), VUE1 (1), WSR (0).

- **SDU** (7, initiator only) declares disconnects unexpected; "set automatically whenever the SCSI core is reselected, or successfully selects another SCSI device", and the driver/script should clear it before an *expected* disconnect (before Abort/BDR/Clear Queue/Release Recovery, before ACK-ing a Disconnect or Command Complete) [1] ibid.
- **CHM** (6) is set by CHMOV and cleared by MOVE; with it, an odd-boundary end-of-transfer parks the last byte in SWIDE (receive) or SODL (send) for "marrying" with the next transfer's first byte (§ 3.3).
- **SLPMD/SLPHBEN** (5/4) select whether the SLPAR register shows the XOR of both longitudinal-parity bytes, one raw byte, and which half (§ 2.6).
- **WSS** (3) and **WSR** (0) are the wide-send/wide-receive residue flags; each reads the flag and self-clears on a write of 1 [1] ibid.
- **VUE0/VUE1** (2/1) are the vendor-unique enhancements: VUE0 reflects whether the last command-phase group code was vendor-unique; VUE1 **disables the automatic byte-count reload in command phase** — with VUE1 clear, a Group 0/1/2/5 command byte makes the chip overwrite DBC with the 6/10/12-byte CDB length (§ 3.1) [1] ibid.

**SCNTL3 (0x03) — SCSI Control Three**, default 0x00. Bits: reserved (7), SCF[2:0] (6:4), EWS (3), CCF[2:0] (2:0).

- **SCF** divides SCLK for the *synchronous* SCSI logic; **CCF** divides SCLK for the asynchronous core. The two are programmed together from the SCLK frequency [1] § 4.2, Table 4.3:

| SCF/CCF encoding | Factor | SCLK range the encoding is valid for |
|---|---|---|
| 001 | SCLK/1 | 16.67–25.0 MHz |
| 010 | SCLK/1.5 | 25.01–37.5 MHz |
| 011 | SCLK/2 | 37.51–50.0 MHz |
| 000, 100 | SCLK/3 | 50.01–75.0 MHz |

  "It is important that these bits are set to the proper values to guarantee that the LSI53C825A meets the SCSI timings as defined by the ANSI specification" [1] ibid.
- **EWS** (3) enables wide (16-bit) information transfer phases; command, status and message phases are unaffected. Clearing EWS also clears the WSR residue flag. (The 53C875 adds bit 7 here: the Ultra/Fast-20 enable [4] Table 6-8.)

### 2.5 SCSI identity and timing registers

**SCID (0x04) — SCSI Chip ID**. Bits: reserved (7), RRE (6), SRE (5), reserved (4), ENC[3:0] (3:0). ENC is the encoded ID the chip *asserts when arbitrating*; RRE and SRE enable response to *reselection* and *selection* respectively, at the IDs enabled in RESPID0/1 — note the deliberate split: arbitration uses one ID (SCID), response can cover many (RESPID) [1] § 4.2, Register 0x04. SCSI ID priority is 7 highest, then 6…0, then 15…8 lowest.

**SXFER (0x05) — SCSI Transfer**. Bits: TP[2:0] (7:5), MO[4:0] (4:0). TP selects the synchronous transfer period as a divisor of the SCF output: XFERP = 4 + TP, so 4 through 11 clocks of the synchronous core clock [1] § 4.2, Register 0x05. The worked example in the manual: for a 10 MB/s target with a 40 MHz SCLK, Period = 100 ns, SSCP = 25 ns, so SXFERP = Period/SSCP + ExtCC = 4 (+1 if SCNTL1.EXC is set and the chip is sending). MO is the maximum synchronous offset, which the 825A extends from the family's earlier 8 to **16** levels [1] § 1.4.1. Bits [7:0] of SXFER are loaded automatically from the table-indirect I/O structure on table-indirect instructions [1] ibid.

**SDID (0x06) — SCSI Destination ID**. ENC[3:0] holds the binary-encoded ID of the initiator or target being selected/reselected; during SCRIPTS the SCRIPTS processor writes it from the Select/Reselect instruction's ID field [1] § 4.2, Register 0x06.

**GPREG (0x07) — General Purpose**. GPIO[4:0], direction-programmed by GPCNTL; GPIO4 defaults to output (used for the external-flash VPP enable), GPIO[3:0] default to inputs with pull-downs. Vendor software uses GPIO3 as a differential-board sense (pulled low = differential board) and GPIO0 as a drive LED strobe [1] § 4.2, Register 0x07.

### 2.6 SCSI status and data-path registers

**SFBR (0x08) — SCSI First Byte Received**. Holds "the first byte received in any asynchronous information transfer phase" — Message-In, Status, Data-In first bytes for an initiator — refreshed on every Block Move for that phase. It doubles as the SCRIPTS ALU accumulator: read-modify-write instructions target it, and comparisons in Transfer Control instructions test it. It is *not* writable by the CPU or by a Memory Move; to load it from memory a script must stage the byte through another register. With DCNTL.COM clear it also captures the SCSI data lines during the selection phase [1] § 4.2, Register 0x08.

**SOCL (0x09) — SCSI Output Control Latch**. Write bits REQ, ACK, BSY, SEL, ATN, MSG, C/D, I/O to drive the named SCSI control line directly; "controlled by the SCRIPTS processor when executing SCSI SCRIPTS … Do not write to the register once the LSI53C825A starts executing normal SCSI SCRIPTS" [1] § 4.2, Register 0x09.

**SSID (0x0A) — SCSI Selector ID** (read-only). VAL (7) says two IDs were on the bus during a bus-initiated selection/reselection; ENID[3:0] then holds the binary-encoded *other* party's ID. With VAL clear "only one ID is present and the contents of the encoded destination ID are meaningless" — the SCSI-1 single-initiator case [1] § 4.2, Register 0x0A.

**SBCL (0x0B) — SCSI Bus Control Lines** (read-only). Unlatched, live image of the nine control lines, synchronized before presentation to PCI; diagnostics and low-level mode only [1] § 4.2, Register 0x0B.

**DSTAT (0x0C) — DMA Status** (read-only, **read-to-clear**; default 0x80). Bits: DFE (7), MDPE (6), BF (5), ABRT (4), SSI (3), SIR (2), reserved (1), IID (0).

- **DFE** — DMA FIFO empty; pure status, never interrupts, never clears on read.
- **MDPE** — master data parity error on the PCI side (gated by CTEST4.MPEE).
- **BF** — bus fault: a master cycle "that ends with a Bad Address or Target Abort Condition".
- **ABRT** — an abort happened (ISTAT.ABRT was set).
- **SSI** — single-step interrupt (DCNTL.SSM).
- **SIR** — a SCRIPTS INT instruction evaluated true.
- **IID** — illegal instruction detected. Beyond reserved opcodes, the manual enumerates the conditions that raise it: a Wait Disconnect with REQ asserted and no disconnect; a Block Move with DBC = 0 (outside target command phase); Transfer Control with CD and CP both set in target mode; Carry Test combined with CD or CP; reserved bit 22 or WVP-in-target-mode in a Transfer Control; Load/Store whose memory address maps back to the operating registers, or is misaligned with the register, or has DCMD bits wrong, or a byte count outside 1–4, or crosses a dword boundary; Memory Move with reserved DCMD bits or unaligned source/destination [1] § 4.2, Register 0x0C.

Reading DSTAT clears its set bits "but does not necessarily clear the register in case additional interrupts are pending (the LSI53C825A stack interrupts)" — and an 8-bit read of DSTAT, SIST0 and SIST1 back-to-back needs a 12-CLK spacing between reads [1] ibid (§ 3.4).

**SSTAT0 (0x0D) — SCSI Status Zero** (read-only). ILF (7) SIDL low byte full; ORF (6) the hidden synchronous send buffer SODR full; OLF (5) SODL low byte full; AIP (4) arbitration in progress (Bus Free seen, BSY and ID asserted); LOA (3) arbitration lost to another device's SSEL/; WOA (2) arbitration won; RST/ (1) live SCSI reset level; SDP0/ (0) live parity line. The ILF/ORF/OLF trio is how a driver counts bytes stranded in the SCSI-side data path after an error (§ 3.2) [1] § 4.2, Register 0x0D.

**SSTAT1 (0x0E) — SCSI Status One** (read-only). FF[3:0] (7:4) are the low four bits of the SCSI synchronous FIFO fill count, with SSTAT2.FF4 as the fifth bit — a 0–16 byte/word count that "is not latched and they will change as data moves through the FIFO". SDP0L (3) is the parity bit latched with SIDL; MSG/C/D/I/O (2:0) are the phase lines "latched on the asserting edge of SREQ/" in either role — the value a Block Move or phase compare tests against [1] § 4.2, Register 0x0E.

**SSTAT2 (0x0F) — SCSI Status Two** (read-only). The wide mirrors ILF1/ORF1/OLF1 (7:5), FF4 (4), SPL1 (3, latched parity of SD[15:8]), **DIFF** (2, set when an SE cable is detected on the DIFFSENS pin of a differential system), **LDSC** (1, "last disconnect": set when CON drops, cleared by a Block Move executed while CON is on — the pair CON+LDSC detects a disconnect followed by a select/reselect), SDP1/ (0, live upper parity line) [1] § 4.2, Register 0x0F.

**SIDL (0x50) / SODL (0x54) / SBDL (0x58)** — the SCSI data-path registers: SIDL is the latched input (reading it *checks* parity and can raise a parity interrupt), SODL the output latch (assert onto the bus with SCNTL1.ADB), SBDL the live unlatched bus image, all 16 bits wide. Diagnostic, low-level and error-recovery use [1] § 4.2, Registers 0x50/0x54/0x58.

**SWIDE (0x45) — SCSI Wide Residue**. Holds the leftover high-order byte of a partial wide receive: "either the first data byte of a subsequent data transfer, or … a residue byte which should be cleared when an Ignore Wide Residue message is received. It may also be an overrun data byte" [1] § 4.2, Register 0x45.

**SLPAR (0x44) — SCSI Longitudinal Parity**. A bytewise XOR across all SCSI data moved through the core, in two multiplexed bytes per SCNTL2.SLPMD/SLPHBEN; seed it to zero and it holds the correct even check byte at the end of a block. Optional, "does not latch SCSI selection/reselection IDs under any circumstances", and any write clears it to zero [1] § 4.2, Register 0x44.

### 2.7 DMA and SCRIPTS execution registers

**DSA (0x10–0x13) — Data Structure Address**. The 32-bit base for all table-indirect addressing, "usually loaded prior to starting an I/O, but it is possible for a SCRIPTS Memory Move to load the DSA during the I/O". Preserved across Memory-to-Memory Moves via a shadow copy (CTEST4.SRTM exposes the shadows) [1] § 4.2, Register 0x10.

**ISTAT (0x14) — Interrupt Status** (read/write; the *only* CPU-accessible register during SCRIPTS). Bits: ABRT (7), SRST (6), SIGP (5), SEM (4), CON (3), INTF (2), SIP (1), DIP (0).

- **ABRT** — write 1 to abort the current operation; the manual's abort sequence is: set ABRT, wait for interrupt, read ISTAT; if SIP is set read SIST0/SIST1 and loop; when only DIP remains, "write 0x00 value to this register" then read DSTAT to confirm the abort [1] § 4.2, Register 0x14.
- **SRST** — software reset: "All operating registers are cleared to their respective default values and all SCSI signals are deasserted". Not self-clearing, does not assert SCSI RST/, and does not touch the PCI configuration registers (the DCNTL.COM bit also survives it) [1] §§ 4.2 Register 0x14, 4.2 Register 0x3B.
- **SIGP** — the doorbell between CPU and a running script: "writable at any time, and polled and reset using Chip Test Two (CTEST2)". The only instructions that consume it are Wait Select and Wait Reselect, which jump to their alternate address immediately when it is set [1] ibid; [4] Ch. 10.
- **SEM** — a one-bit semaphore settable from both sides.
- **CON** — the connected mirror of SCNTL1.CON.
- **INTF** — set by an INTFLY instruction; the script does *not* halt; cleared by writing 1 (not by read). "If the INTF bit is set but SIP or DIP is not set, do not attempt to read the other chip status registers" [1] ibid.
- **SIP / DIP** — SCSI-type / DMA-type interrupt pending summaries; the causes are enumerated in § 3.4.

**CTEST0 (0x18)** — read/write but "LSI Logic reserves the right to use these bits for future" enhancements; prints the DMA-FIFO byte-empty flags. **CTEST1 (0x19)** — read-only FMT[3:0] (bottom-of-FIFO empty flags per byte lane; all set = FIFO empty) and FFL[3:0] (top-of-FIFO full flags; all set = FIFO full) [1] § 4.2, Registers 0x18/0x19.

**CTEST2 (0x1A)** — DDIR (7, data direction, set = SCSI→host), SIGP (6, a copy of ISTAT.SIGP; **reading CTEST2 clears ISTAT.SIGP**), CIO/CM (5/4, "currently enabled as I/O / memory space", both set if dual-mapped), SRTCH (3, the only writable bit: SCRATCHA shows the operating-register memory-mapped base and SCRATCHB shows the SCRIPTS RAM base while it is set), TEOP (2), DREQ (1), DACK (0). "Do not execute a Read-Modify-Write to this register" [1] § 4.2, Register 0x1A.

**CTEST3 (0x1B)** — V[3:0] (7:4, chip revision level, mirrors the Revision ID's lower nibble), FLF (3, flush the DMA FIFO to memory at DNAD), CLF (2, clear the DMA FIFO pointers — data lost, self-clears), FM (1, fetch-pin mode), WRIE (0, enable PCI Write and Invalidate, with the config Command bit 4 and DCNTL.CLSE) [1] § 4.2, Register 0x1B.

**TEMP (0x1C–0x1F)** — the single CALL return-address slot (and the Memory Move destination address); "not a stack and can only hold one Dword" [1] §§ 4.2 Register 0x1C, 5.6.1.

**DFIFO (0x20)** — the DMA FIFO byte-offset counter, BO[7:0]; with CTEST5.BO[9:8] it forms a 10-bit count of bytes moved between the SCSI and DMA cores, against DBC's count of bytes moved on PCI — "The difference between these two counters represents the number of bytes remaining in the DMA FIFO" [1] § 4.2, Register 0x20.

**CTEST4 (0x21)** — BDIS (7, back-to-back cycles instead of bursts), ZMOD (6, all pins high-impedance), ZSD (5, SCSI data pins high-impedance), SRTM (4, expose the TEMP/DSA shadows), MPEE (3, master parity error enable), FBL[2:0] (FIFO byte-lane select for CTEST6 access; "For normal operation, FBL3 must equal zero") [1] § 4.2, Register 0x21.

**CTEST5 (0x22)** — ADCK (7, increment DNAD), BBCK (6, decrement DBC), DFS (5, **DMA FIFO size: 88 bytes when clear, 536 when set**; the 88-byte mode exists so "software written for other LSI53C8XX family chips" computes residual counts correctly), MASR (4) + DDIR (3, drive the internal DMA direction signal), BL2 (2, the third burst-length bit), BO[9:8] [1] § 4.2, Register 0x22.

**CTEST6 (0x23)** — the DMA FIFO data port for CTEST4-gated FIFO testing; "should not be accessed before starting or restarting SCRIPTS operation" [1] § 4.2, Register 0x23.

**DBC (0x24–0x26)** — the 24-bit DMA Byte Counter: bytes per Block Move, decremented as data crosses the *PCI* bus; maximum 16,777,215 bytes per instruction; a Block Move with 0x000000 loaded is an illegal instruction (outside target command phase). It also holds the low 24 bits of every fetched instruction's first dword and table-indirect offsets [1] § 4.2, Register 0x24.

**DCMD (0x27)** — the opcode byte of the current instruction [1] § 4.2, Register 0x27.

**DNAD (0x28–0x2B)** — the general-purpose DMA address pointer (data address for Block Moves, alternate address for I/O instructions). The manual warns: "should not be used to determine data addresses during a Phase Mismatch interrupt, as its value is not always correct for this use" — use DBC, DFIFO and DSPS for residual arithmetic instead [1] § 4.2, Register 0x28.

**DSP (0x2C–0x2F)** — the SCRIPTS program counter. "To execute SCSI SCRIPTS, the address of the first SCRIPTS instruction must be written to this register" and execution then runs "until an interrupt condition occurs" — with the fine print that when written eight bits at a time, "writing the upper eight bits begins execution" [1] § 4.2, Register 0x2C. A DSP write also flushes the prefetch unit [1] § 2.2.6.

**DSPS (0x30–0x33)** — the second dword of the current instruction: data address, jump/alternate address, or interrupt vector [1] § 4.2, Register 0x30.

**SCRATCHA (0x34), SCRATCHB (0x5C), SCRATCHC–J (0x60–0x7F)** — ten general-purpose 32-bit scratch registers, writable by CPU, SCRIPTS read/write instructions, Memory Moves and Load/Store. While CTEST2.SRTCH is set, A displays the register-file memory base and B displays the SCRIPTS RAM base (writes still pass through to the real registers) [1] §§ 2.2.4, 4.2. SCRIPTS cannot execute out of them [1] § 4.2.

**DMODE (0x38)** — BL[1:0] (7:6, burst length with CTEST5.BL2: 2/4/8/16/32/64/128-transfer bursts), SIOM (5), DIOM (4, memory vs I/O space for a Memory/Block Move's source and destination), ERL (3, Read Line), ERMP (2, Read Multiple), BOF (1, burst opcode fetch), MAN (0, manual start: "prevents the LSI53C825A from automatically fetching and executing SCSI SCRIPTS when the DSP register is written" — then DCNTL.STD starts it). The chip asserts REQ/ when the FIFO can take at least one burst and inserts "a 'fairness delay' of four CLKs between burst transfers … not … during PCI retry cycles" [1] § 4.2, Register 0x38.

**DIEN (0x39)** — the DMA interrupt enables: MDPE, BF, ABRT, SSI, SIR, IID. "All DMA interrupts are considered fatal, therefore SCRIPTS stops running when this condition occurs, whether or not the interrupt is masked" — masking suppresses only the IRQ/ pin assertion, never the halt [1] § 4.2, Register 0x39.

**SBR (0x3A)** — the Scratch Byte Register, general purpose; "called the DMA Watchdog Timer on previous LSI53C8XX family products" [1] § 4.2, Register 0x3A.

**DCNTL (0x3B)** — CLSE (7, cache-line-size enable), PFF (6, prefetch flush, self-clearing), PFEN (5, prefetch enable, needs bursts >= 4 dwords), SSM (4, single-step mode), IRQM (3, totem-pole IRQ driver; "should remain cleared to retain full PCI compliance"), STD (2, start DMA — required in manual-start and single-step modes), IRQD (1, mask the IRQ/ pin; the interrupt is "not lost … merely masked at the pin", and clearing the bit with an interrupt pending asserts IRQ/ immediately), COM (0, LSI53C700-family compatibility: with COM clear, selection/reselection IDs go to both SSID and SFBR; with COM set only to SDID, "protecting the SFBR from being overwritten … during a DMA register-to-register operation"; **not affected by a software reset**) [1] § 4.2, Register 0x3B.

**ADDER (0x3C–0x3F)** — the internal adder's sum output, test-only [1] § 4.2, Register 0x3C.

### 2.8 SCSI interrupt registers

**SIEN0 (0x40) / SIST0 (0x42)** — mask and (read-to-clear) status of the SCSI-level interrupts, in initiator-mode meaning:

| Bit | Name | Meaning |
|---|---|---|
| 7 | M/A | phase mismatch (initiator) / SATN/ active (target) |
| 6 | CMP | function complete: "an arbitration only or full arbitration sequence is completed" |
| 5 | SEL | the chip was selected (needs SCID.SRE and the ID in RESPID) |
| 4 | RSL | the chip was reselected (needs SCID.RRE and the ID in RESPID) |
| 3 | SGE | SCSI gross error: FIFO underflow/overflow, offset underflow/overflow, phase change with outstanding synchronous offset, residual data in the sync FIFO |
| 2 | UDC | unexpected disconnect (§ 3.6) |
| 1 | RST | SCSI RST/ received — **edge-sensitive**, "so multiple interrupts cannot occur because of a single SRST/ pulse" |
| 0 | PAR | parity error (with SCNTL0.EPC) |

Reading SIST0 "returns the status of the various interrupt conditions, whether they are enabled … or not" and clears them [1] § 4.2, Registers 0x40/0x42.

**SIEN1 (0x41) / SIST1 (0x43)** — mask and status of: **STO** (2, selection/reselection time-out), **GEN** (1, general-purpose timer expired), **HTH** (0, handshake-to-handshake timer expired). Bits 7:3 reserved. Reading SIST1 clears it [1] § 4.2, Registers 0x41/0x43.

### 2.9 Memory-access, GPIO and timer registers

**MACNTL (0x46)** — TYP[3:0] (7:4) is the chip type, **0x06** for the 825A/AE; DWR/DRD/PSCPT/SCPTS (3:0) define which master access types count as "local" for the MAC/_TESTOUT pin [1] § 4.2, Register 0x46.

**GPCNTL (0x47)** — GPIO direction bits for GPREG's five pins (inputs get an internal pull-up), plus ME/FE (7/6) to export the internal bus-master and opcode-fetch signals on GPIO1/GPIO0 [1] § 4.2, Register 0x47.

**STIME0 (0x48)** — HTH[3:0] (7:4) and SEL[3:0] (3:0) program the handshake-to-handshake and selection/reselection timers. The selection timer counts "this timing (plus the 200 µs selection abort time)"; on expiry SIST1.STO sets. Both nibbles share one period table [1] § 4.2, Register 0x48, Table 4.7:

| Value | Minimum time-out (40/160 MHz column) | Minimum time-out (50 MHz column) |
|---|---|---|
| 0000 | disabled | disabled |
| 0001 | 125 µs | 100 µs |
| 0010 | 250 µs | 200 µs |
| 0011 | 500 µs | 400 µs |
| 0100 | 1 ms | 800 µs |
| 0101 | 2 ms | 1.6 ms |
| 0110 | 4 ms | 3.2 ms |
| 0111 | 8 ms | 6.4 ms |
| 1000 | 16 ms | 12.8 ms |
| 1001 | 32 ms | 25.6 ms |
| 1010 | 64 ms | 51.2 ms |
| 1011 | 128 ms | 102.4 ms |
| 1100 | 256 ms | 204.8 ms |
| 1101 | 512 ms | 409.6 ms |
| 1110 | 1.024 s | 819.2 ms |
| 1111 | 2.048 s | 1.6384 s |

**STIME1 (0x49)** — GEN[3:0] (3:0) programs the general-purpose timer over the same table; GENSF (5) and HTHSF (4) scale their timers by 16; HTHBA (6) starts handshake monitoring "as soon as SBSY/ is asserted". "To reset a timer before it expires and obtain repeatable delays, the time value must be written to zero first, and then written back to the desired value" [1] § 4.2, Register 0x49, Table 4.8.

**RESPID0/1 (0x4A/0x4B)** — the two-byte selection/reselection response mask: "each bit represents one possible ID" from 0 (RESPID0 bit 0) to 15 (RESPID1 bit 7). "The chip can respond to more than one ID … However, the chip can arbitrate with only one ID value in the SCID register" [1] § 4.2, Registers 0x4A/0x4B.

### 2.10 Test registers

**STEST0 (0x4C, read-only)** — SSAID[3:0] (the encoded ID the chip was selected/reselected *as*), SLT (3, selection-response logic ready), ART (2, arbitration priority-encoder test), SOZ (1, sync offset zero), SOM (0, sync offset at maximum) [1] § 4.2, Register 0x4C.

**STEST1 (0x4D)** — SCLK (7, disable the external SCLK pin and clock the SCSI core from CLK; must be *clear* for 10 MB/s transfers with a >= 40 MHz SCLK), SISO (6, low-power input isolation). The 53C875 adds the clock-doubler bits (3:2) here [1] § 4.2, Register 0x4D; [4] Table 6-8.

**STEST2 (0x4E)** — SCE (7, assert any SCSI line via SOCL/SODL regardless of role — "could cause contention on the SCSI bus"), ROF (6, reset an outstanding synchronous offset — "If a SCSI gross error occurs, set this bit"; self-clearing), DIF (5, differential mode: 3-states the BSY/, SEL/, RST/ pads for use as inputs), SLB (4, loopback: the SCSI core plays both roles), SZM (3, high-impedance the open-drain 48 mA SCSI drivers for internal loopback), AWS (2, force wide on *all* phases — normally off since 16-bit message/command/status "are not supported by the SCSI specifications"), EXT (1, extend the TolerANT REQ/ACK deasserted-edge glitch filter from 30 ns to 60 ns — "Never set this bit during Fast SCSI"), LOW (0, low-level mode: "no DMA operations occur, and no SCRIPTS execute"; bus control by hand) [1] § 4.2, Register 0x4E.

**STEST3 (0x4F)** — TE (7, TolerANT active negation: actively drives REQ, ACK, data and parity high instead of relying on terminators; "should be enabled" in differential or Fast SCSI), STR (6, stop the divided SCSI clock glitchlessly), HSC (5, halt the SCSI clock), DSI (4, ignore SCSI-1 single-initiator selections — "Assert this bit in SCSI-2 systems so that a single bit error … is not interpreted as a single initiator response"), S16 (3, assume a 16-bit system and check parity of IDs 15–8 during selection/reselection), TTM (2, timer test mode — divides all three timer periods massively; manufacturing only), CSF (1, clear the SCSI FIFO: clears the full flags and the SIDL/SODL/SODR full bits in SSTAT0/SSTAT2; self-clearing), STW (0, SCSI FIFO test write via SODL) [1] § 4.2, Register 0x4F.

## 3. Behaviour

### 3.1 The SCRIPTS processor

After initialization the chip runs in one of two modes [1] §§ 5.1–5.2. **Low-level register interface mode** (STEST2.LOW) exposes the DMA and SCSI bus control logic directly, for board-level tests, unusual timings and loopback. **High-level SCRIPTS mode** needs only a start address:

> "To operate in the SCSI SCRIPTS mode, the LSI53C825A requires only a SCRIPTS start address. The start address must be at a Dword (four byte) boundary … Instructions are fetched until an interrupt instruction is encountered, or until an unexpected event (such as a hardware error) causes an interrupt … Once an interrupt is generated, the LSI53C825A halts all operations until the interrupt is serviced. Then, the start address of the next SCRIPTS instruction may be written to the DMA SCRIPTS Pointer (DSP) register to restart the automatic fetching and execution of instructions." [1] § 5.2

The manual is emphatic that this interface is sufficient for everything: "Switching to low level mode for error recovery should never be required" [1] § 5.2.

Every instruction is two or three dwords: the first loads DCMD and DBC, the second loads DSPS, the (Memory Move only) third loads the TEMP shadow [1] § 5.2. Indirect and table-indirect forms fetch one or two more dwords. The five instruction classes [1] § 5.2, Table 5.1:

| Class | IT bits [31:30] | Purpose |
|---|---|---|
| Block Move (MOVE, CHMOV) | 00 | move data between SCSI and memory |
| I/O | 01 | trigger SCSI hardware sequences (Select, Wait Disconnect, Wait Reselect, Set, Clear, and the target-role mirrors) |
| Read/Write | 01 (opcodes 101–111) | register moves and read-modify-write ALU ops |
| Transfer Control | 10 | Jump / Call / Return / Interrupt / INTFLY, conditionally |
| Memory Move | 00 (opcode field) | memory-to-memory block copy, up to 16 MB |
| Load and Store | 111 (IT[2:0]) | 1–4 bytes between an internal register and memory, optionally DSA-relative |

**Block Move.** The phase field SCSIP[2:0] names the phase (MSG/C/D/I/O → Data-Out, Data-In, Command, Status, Reserved-Out/In, Message-Out, Message-In). In target mode the chip asserts that phase; in initiator mode it compares against the SSTAT1 latches and — on mismatch — "generates a phase mismatch interrupt and the instruction is not executed" [1] § 5.3.1. In target command phase the first command byte's group code auto-reloads DBC with the CDB length (6/10/12) unless SCNTL2.VUE1 is set [1] § 5.3.1. An initiator Message-In Block Move deliberately withholds the final ACK/: "Clear the SACK/ signal using the Clear SACK I/O instruction", giving the initiator room to assert ATN and reject the message [1] § 5.3.1. The opcode bit selects MOVE vs CHMOV and *swaps meaning with the role* (target: 0 = MOVE; initiator: 0 = CHMOV), which is what makes chained wide transfers correct in both directions [1] § 5.3.1. Addressing: direct (the address dword is the data address), indirect (IA: the address dword points at the real address), or table-indirect (TIA: the 24-bit signed offset from DSA fetches an 8-byte count+address pair) — "Do not use indirect and table indirect addressing simultaneously" [1] § 5.3.1.

**I/O instructions.** The opcode table is role-dependent [1] § 5.4:

| OPC | Initiator mode | Target mode |
|---|---|---|
| 000 | Select | Reselect |
| 001 | Wait Disconnect | Disconnect |
| 010 | Wait Reselect | Wait Select |
| 011 | Set | Set |
| 100 | Clear | Clear |

- **Select** arbitrates (retrying silently on loss), then selects the ID in the instruction's ENDID field; once arbitration is won "it fetches the next instruction … This way the SCRIPTS can move on to the next instruction before the selection completes", and execution continues "until a SCRIPT that requires a response from the Target is encountered". The instruction's second dword is the alternate address: "specifies the memory address to fetch the next instruction if the chip is selected or reselected during the selection" [1] § 5.4.1; [4] Ch. 3, SELECT. The Sel bit asserts ATN during the selection phase (SELECT ATN); setting it on any other I/O instruction is an illegal instruction [1] § 5.4.1. Four addressing combinations exist via the RA and TI bits — direct, table-indirect (TI fetches a DSA-relative quad of *config, ID, offset/period* and loads SCNTL3/SXFER/the ID), relative, and table-relative [1] § 5.4.1.
- **Wait Disconnect** waits for a "legal" disconnect: BSY and SEL inactive for a minimum Bus Free delay of 400 ns after a Disconnect or Command Complete message [1] § 5.4.1.
- **Wait Reselect** waits to be reselected; the alternate address is taken "if the chip is selected … or if the SIGP bit … is set by the host processor" — the host's way to abort the wait and schedule other work [1] § 5.4.1; [4] Ch. 3, WAIT RESELECT.
- **Set / Clear** drive the ACK, ATN, target and carry bits — e.g. CLEAR ACK after a message byte, SET ATN to reject the next one [1] § 5.4.1.

**Read/Write instructions.** Three opcode forms (read-modify-write 111, move-to-SFBR 110, move-from-SFBR 101) over eight operators: move, shift-left, OR, XOR, AND, shift-right, add, add-with-carry — shifts run through the ALU carry bit, and the forms with SFBR give register-to-register moves and comparisons [1] § 5.5.4, Table 5.2. There is no subtract: "Subtraction is not available when SCSI First Byte Received (SFBR) is used instead of data8 … first XOR the value to subtract with 0xFF, and add 1" [1] § 5.5.4.

**Transfer Control.** Jump, Call, Return, Interrupt, INTFLY, each conditionally on: the ALU carry (CT), the latched phase (CP), the SFBR byte against an 8-bit compare value with an 8-bit mask (CD + DCM/DCV), and the true/false polarity bit (JMP). WVP makes the chip wait for an unserviced phase before comparing [1] § 5.6.1. Call stores the return address in TEMP — "nested call instructions are not allowed"; Return loads DSP from TEMP and "does not check to see whether the Call instruction has already been executed. It does not generate an interrupt if a Return instruction is executed without previously executing a Call" [1] § 5.6.1. Interrupt halts the processor, with DSPS as a caller-supplied vector so "the Interrupt Service Routine [can] quickly identify the point at which the interrupt occurred"; INTFLY sets ISTAT.INTF without halting [1] § 5.6.1. The RA bit makes the second dword a signed 24-bit offset from DSP — "a relative transfer can be to any address within a 16 Mbyte segment", forward or backward, and a wholly-relative program "does not require any run time alteration of physical addresses, and can be stored in and executed from a PROM" [1] § 5.6.1.

**Memory Move.** Copies up to 16 MB in one instruction from the DSPS address to the TEMP address, with the same alignment rule for both ends; misalignment is an illegal instruction [1] § 5.7. Because the chip decodes its own BARs, a Memory Move whose source or destination falls inside the register window touches the register named by the low address bits — "register values are saved to system memory and later restored, and SCRIPTS can make decisions based on data values in system memory" [1] § 5.7.2.

**Load and Store.** Move 1–4 bytes between one internal register (RA[6:0]) and memory, optionally at a DSA-relative address; the byte count must respect the register/memory alignment and not cross a dword boundary; SFBR cannot be a Load target; a memory address that maps back to the chip's registers raises an immediate IID [1] § 5.8. The NF (no flush) bit spares the prefetch unit when the store is not self-modifying code [1] § 5.8.1.

**Prefetch.** With DCNTL.PFEN and bursts >= 4 dwords, the prefetch unit fetches 8 dwords of instructions ahead, and disables itself if it cannot. It flushes itself: on every Memory Move, on every Store (unless NF), on every write to DSP, on every taken Transfer Control branch, and on PFF [1] § 2.2.6. While prefetching, the PCI cache-line commands are not used [1] § 2.2.6.

### 3.2 DMA engine and the FIFOs

The DMA FIFO is "4 bytes wide by 134 transfers deep" — 536 bytes — and can be shrunk to the family-standard 88 bytes with CTEST5.DFS [1] § 2.4.8. The engine is a bus master that bursts 2–128 dwords per ownership, with programmable burst length (DMODE.BL + CTEST5.BL2), a four-CLK fairness delay between bursts, and support for misaligned transfers [1] §§ 1.4.2, 2.2.2, 4.2 Register 0x38. With the PCI Cache Line Size register programmed and DCNTL.CLSE set, the chip aligns to cache-line boundaries with a stepping scheme — single dwords to a 4-dword boundary, then the largest legal burst up to the line — and issues Memory Read Line, Memory Read Multiple and Memory Write and Invalidate in place of plain reads/writes when their enable bits and alignment conditions are met [1] § 2.1.3.

On the SCSI side, the asynchronous data path is SIDL → DMA FIFO → PCI (receive) and PCI → DMA FIFO → SODL → bus (send); synchronous sends add the hidden SODR buffer between SODL and the bus, and synchronous receives go through the 16-deep SCSI FIFO counted by SSTAT1.FF + SSTAT2.FF4 [1] §§ 2.4.8.1, 4.2 Registers 0x0D–0x0F.

After an interrupt, the stranded-byte calculation is a fixed recipe [1] § 2.4.8.1: (1) bytes in the DMA FIFO = DFIFO (with CTEST5.BO[9:8] if 536-byte mode) minus DBC's low bits, masked to 0x7F or 0x3FF; (2) check SSTAT0/SSTAT2 bits 5–7 for bytes in SODL/SIDL; (3) for synchronous paths also the SSTAT1 FIFO flags and, after chained wide moves, the WSR/SWIDE residue.

### 3.3 The SCSI core: rates, wide transfers and residue

Synchronous timing is a two-stage divide: SCLK → SCF (SCNTL3[6:4], synchronous core) → TP (SXFER[7:5], XFERP = 4–11 core clocks per REQ period), with SCNTL1.EXC adding one clock on sends. The receive side runs at a quarter of the SCF output: "if SCLK is 40 MHz and the SCF value is set to divide by one, then the maximum rate at which data can be received is 10 MHz" [1] § 2.4.11.2. For the tightest send timings the manual's guidance is to divide as much as possible in SCF and as little as possible in TP [1] § 2.4.12.

Wide operation (SCNTL3.EWS) makes *data* phases 16-bit; command, status and message stay 8-bit [1] § 4.2, Register 0x03. The hard part is an odd byte count, and the family's answer is the chained Block Move plus two flags:

- On a *receive* ending mid-word, the high byte parks in SWIDE, WSR sets, and "The contents of the SCSI Wide Residue (SWIDE) register should be the first byte transferred to memory at the start of the chained block move data stream" [1] § 2.4.14.5.
- On a *send* ending mid-word, the low byte parks in SODL, WSS sets, and the next transfer's first byte is "married" with it regardless of whether that next instruction is a MOVE or a CHMOV [1] § 2.4.14.1.

The manual's worked example: an initiator `CHMOV 5, 3 when Data_Out` moves bytes at 0x03–0x06 and leaves 0x07 in SODL; the following `MOVE 5, 9 when Data_Out` sends it married with the byte at 0x09 [1] § 2.4.14.5. For N consecutive wide receives, instructions 2..N should be CHMOVs; for N consecutive wide sends, instructions 1..N−1 should be [1] § 2.4.14.5.

Parity is layered: SCSI-side odd checking under SCNTL0.EPC with optional SATN/ on error (AAP) and optional no-halt (SCNTL1.DHP, target); a bad-parity injector (AESP); the longitudinal SLPAR accumulator; and PCI-side master parity under CTEST4.MPEE. The four-way table of halt/interrupt behavior against DHP and the PAR enable is Table 2.5 [1] § 2.4.7.

Electrical behavior is TolerANT: active negation of REQ, ACK, data and parity when STEST3.TE is set ("actively deasserted, instead of relying on external pull-ups"), and a digital input filter on REQ/ACK deasserting edges, 30 ns or 60 ns with STEST2.EXT — the feature that "helps eliminate double clocking of data, the single biggest reliability issue with SCSI operations" [1] § 1.3. The Network Server's backplane budget is sized around it: ~12" of ~63 Ω trace on the logic board, <24" of 78 Ω single-ended cable on the mezzanine, "including the backplane, the total extent of the SCSI bus should be under 6 feet" [5] § 7.2.1.

### 3.4 Interrupts

The chip summarizes in ISTAT first: INTF (clear it first, by writing 1), then SIP (read SIST0 and SIST1) and DIP (read DSTAT); "SCSI-type and DMA-type interrupts may occur simultaneously, so in some cases both SIP and DIP may be set" [1] § 2.4.13.2. The service order when both are set: DMA first, "because a serious DMA interrupt condition could influence how the SCSI interrupt is acted upon" [1] § 2.4.13.5.

**Fatal vs nonfatal.** All DMA interrupts are fatal — SCRIPTS stops whether they are masked or not. SCSI interrupts are fatal *except* a short list that exists precisely so ordinary bus events never stop the engine: in initiator mode CMP, SEL, RSL, GEN and HTH; in target mode CMP, SEL, RSL, M/A, GEN and HTH; plus INTF. "All nonfatal interrupts become fatal when they are enabled by setting the appropriate interrupt enable bit" [1] § 2.4.13.3 — that is, SIEN0/SIEN1 bits are really "make it interrupt the CPU" bits, not "make it stop" bits.

**Masking.** A masked nonfatal condition still sets its SIST bit but does not set SIP or assert IRQ/. A masked fatal condition still halts the engine and still sets SIP/DIP — only the pin stays quiet, so "the SCRIPTS halts and the system will never know it unless it times out and checks the ISTAT after a certain period of inactivity" [1] § 2.4.13.4. The IRQ/ output is latched: "once asserted, it remains asserted until the interrupt is cleared … Masking an interrupt after the IRQ/ output is asserted does not cause IRQ/ to be deasserted" [1] § 4.2, Register 0x39.

**Stacking.** The chip holds a *second level* of interrupt registers behind SIST0/SIST1/DSTAT:

> "If the SIP or DIP bits … are set (first level), then there is already at least one pending interrupt, and any future interrupts are stacked in extra registers behind … (second level). When the first level of interrupts are cleared, all the interrupts that came in afterward will move into … After the first interrupt is cleared by reading the appropriate register, the IRQ/ pin is deasserted for a minimum of three CLKs; the stacked interrupts move into the SIST0, SIST1, or DSTAT and the IRQ/ pin is asserted once again." [1] § 2.4.13.5

Because stacking only begins once SIP/DIP is set, "there is a small timing window in which multiple interrupts can occur but are not stacked" — two conditions landing inside that window present together in one read [1] § 2.4.13.5. Since a masked nonfatal interrupt never sets SIP/DIP, it never stacks either: a later interrupt presents directly with the earlier masked bit still set [1] § 2.4.13.5.

**Orderly halt.** On an interrupt the chip halts in a defined order: an in-flight instruction fetch completes (except on bus fault) with DSP already advanced to the next instruction; a memory-write direction is flushed to memory before halting when the interrupt is SCSI-side; begun REQ/ACK handshakes complete; outstanding synchronous offset is cleaned up; a Transfer Control instruction that has started runs to completion — and for a `JUMP/CALL WHEN/IF <phase>`, "the DSP will be updated to the transfer address before halting" [4] Ch. 9, "Halting in an Orderly Fashion". After any fatal interrupt the driver checks DSTAT.DFE and clears CLF and CSF (or flushes FLF) if data remains [1] § 2.4.13.2.

**Register-read discipline.** Consecutive 8-bit reads of DSTAT, SIST0 and SIST1 "in any order" need "a delay equivalent to 12 CLK periods between the reads to ensure that the interrupts clear properly"; and when SIP/DIP may not both be set, read the SIST pair *before* DSTAT "to avoid missing a SCSI interrupt" [1] §§ 4.2 Register 0x0C, 4.2 Register 0x42.

**The pin.** IRQ/ is open-drain with an internal weak pull-up by default; DCNTL.IRM selects a totem-pole driver ("should remain cleared to retain full PCI compliance") [1] §§ 3.1.7, 4.2 Register 0x3B. The Interrupt Pin register is hard-wired to INTA/ [1] § 4.1, Register 0x3D.

### 3.5 Selection, reselection and the multithreaded pattern

Being selected or reselected *while trying to select/reselect* is the normal hazard of a multithreaded initiator, and the Select/Reselect instructions' alternate address exists for exactly that: "In multithreaded SCSI I/O environments, it is not uncommon to be selected or reselected while trying to perform selection/reselection … The Select SCRIPTS instruction has an alternate address to which the SCRIPTS jumps when this situation occurs" [1] § 2.4.10. The programming guide's operand description is equally specific about *when* that address is used — "if the chip is selected or reselected by another device", or if arbitration "terminates because of a bus initiated selection or reselection" [4] Ch. 3, SELECT. Nothing in either manual routes a *time-out* to the alternate address; a time-out is an interrupt condition (§ 3.6), and the chip halts.

After a role change of that kind, the manual directs that the receiving script begin with SET TARGET or SET INITIATOR, and that both SCID response-enable bits be set so the chip can be selected and reselected [1] § 2.4.10.

### 3.6 Selection time-out — the part's most consequential behavior

Selecting an absent SCSI ID is the *common* case during discovery — a wide bus has sixteen IDs and most are empty — and the 825A's answer to it is unusual enough that IBM's driver comments on it. The facts, from the manual:

1. The selection timer is programmed by STIME0.SEL; when "this timing (plus the 200 µs selection abort time)" expires, SIST1.STO sets [1] § 4.2, Register 0x48.
2. The unexpected-disconnect bit accompanies it. SIST0.UDC's description carries the load-bearing parenthetical:

> "This bit is also set if a selection time-out occurs (it may occur before, at the same time, or stacked after the STO interrupt, since this is not considered an expected disconnect)." [1] § 4.2, Register 0x42

3. STO is fatal (it is not in the nonfatal list [1] § 2.4.13.3), so the SCRIPTS processor halts, and the three possible UDC orderings interact with the interrupt-stacking rules of § 3.4: UDC "at the same time" arrives in one 16-bit SIST read with STO; UDC "stacked after" appears in a second interrupt once the first is serviced; UDC "before" arrives first and is serviced first.

The manual permits all three orderings and does not say which is typical, or what selects between them (§ 6). The Network Server's AIX driver is written against the stacked case and is intolerant of the simultaneous case: its interrupt handler classifies a SCSI interrupt by *exact equality* of the 16-bit SIST word against a small set of single-cause values, and a word with two causes set goes to a generic "unknown status" error path (*observed* in the disassembled `pscsiddpin` interrupt handler) [8]. The same driver's script clears SCNTL0's START bit in its selection-time-out recovery with the comment "manually clear Start bit to compensate for chip problem" — a chip problem the vendor manuals in the evidence set do not describe (§ 6) [8].

For the reverse direction — what the *script* does on a time-out — the driver evidence is convergent: its SELECT instructions carry unrelocated offsets as alternate addresses, so a time-out that jumped there would derail execution into unmapped memory; the machine boots, therefore the chip cannot take the alternate address on a time-out and must halt instead (*observed*, by this consistency argument, across a full AIX install boot) [8].

### 3.7 PCI power management (53C825AE only)

The AE parts implement the PCI Bus Power Management Interface with D0 and D3 only. In D3hot "the SCSI clock and the SCSI clock doubler Phase Lock Loop (PLL) are disabled", the function's soft reset is held asserted (clearing pending interrupts, 3-stating the SCSI bus), and the PCI Command register is cleared; D3cold is power removal, and returns to D0 only through power-on reset [1] § 2.5.2.

## 4. Programming model

### 4.1 Discovery and configuration on the Network Server

Both 53C825As are on-board PCI devices of Bandit 1, at IDSEL 17 and 18 — Open Firmware unit addresses `53c825@11` and `53c825@12` [5] §§ 4.6.2, 7.1.1. They are configured *before* any slot, because "Open Firmware maps devices for their requested spaces in discovery order … on board input-output devices are configured prior to slots" [5] § 4.4 — so their BARs are assigned before the RAID card's, and a device-tree dump from a real machine fixes the order. The device tree names them with `model` `NCR,825A` and `compatible` `pci1000,3` [7].

Their interrupt lines land on Grand Central positions that the Network Server deliberately moved from the Power Macintosh 9500's: on the 9500, EXT2/EXT6 carry the two Bandit interrupt lines; on the Network Server those positions carry **FW0_Int** and **FW1_Int** — the two 53C825A controllers' interrupts — with the Bandit lines moved together onto EXT1 as `Error_Int` [5] § 4.2. So each controller's INTA/ output reaches the OS through a dedicated Grand Central external interrupt.

The ROM distinguishes the 825A from a plain 825 by reading configuration offset 0x08 and testing bit 4 of the Revision ID (§ 1.2); on a match it arms the machine-specific path that also drives the over-temperature poll (*observed* in the ROM) [7]. The two controllers are the machine-identity signal: the production ROM detects them to select the Network Server personality — without them as PCI devices, nothing about the machine's SCSI (or its model string) behaves correctly [7].

For an add-in 53C875 in a PCI Power Macintosh, Apple's own worked example prints the full property set of `53c875@10` on Bandit 2: `vendor-id 00001000`, `device-id 0000000F`, `revision-id 00000001`, `class-code 00010000`, `interrupts 00000001`, `min-grant 00000008`, `max-latency 00000040`, `devsel-speed 00000001`, `AAPL,interrupts 0000001D`, `AAPL,slot-name SLOT6_PCI1`, `name 53c875`, `model NCR,875`, `compatible pci1000,f`, `device_type scsi`, a `reg` package with four address spaces (config, I/O, memory, RAM), and `power-consumption` [6]. Note the revision ID 0x01 there: an 875, not an 825A-class part — the two must not be conflated.

### 4.2 Register initialization

Symbios's programming guide enumerates the "startup bits" a driver must consider before writing DSP, per family — for the 825A/875 class [4] Table 6-8:

| Register | Bits to decide | Decision |
|---|---|---|
| SCNTL0 | 7:6, 3, 1:0 | arbitration mode, parity checking, assert-ATN-on-parity, default role |
| SCNTL1 | 7, 5 | extra setup clock, DHP (target) |
| SCNTL3 | all | SCF/CCF from the fitted SCLK (Ultra enable on 875) |
| SCID | 6:5, 3:0 | response enables, chip ID (wide enable on this part) |
| SXFER | all | leave asynchronous until synchronous parameters are negotiated |
| DSA | all | required if table-indirect mode is used |
| CTEST3 | 1:0 | fetch-pin mode, Write and Invalidate enable |
| CTEST4 | 7, 3 | burst disable, master parity enable |
| CTEST2 | 3 | SCRATCHA/B RAM-base alias while SCRIPTS RAM is being located |
| DSP | all | **last**: "write the address of the first SCRIPTS instruction to this register to begin SCRIPTS execution" |
| DMODE | 7:2 | burst length, SIOM/DIOM, Read Line / Read Multiple |
| DIEN | 4:2, 0 | abort, single-step, SCRIPTS-interrupt, illegal instruction |
| DCNTL | 7, 5:3, 0 | cache-line enable, prefetch, single-step, IRQ mode, 700 compatibility |
| SIEN0 | all | the SCSI interrupt enables |
| SIEN1 | 2:0 | STO, GEN, HTH |
| MACNTL | 3:0 | only if the MAC pin is used |
| STIME0 | all | handshake and selection timers |
| STIME1 | 3:0 | general-purpose timer |
| RESPID0/1 | all | the response ID mask |
| STEST1 | 7 | SCLK source |
| STEST2 | 5, 1 | differential mode, REQ/ACK filter width |
| STEST3 | 7 | TolerANT enable |

The guide's advice is that "the hardware default values … are suitable for most applications" and that reserved bits "should be left cleared" [4] Ch. 6. The Guide's sample SCRIPTS program shows the shape the whole industry uses — a phase-dispatch loop entered after a table-indirect SELECT:

```
start_up:
  SELECT ATN FROM device, REL(resel)
switch:
  JUMP REL(msgin),    WHEN MSG_IN
  JUMP REL(msgout),   IF MSG_OUT
  JUMP REL(command_phase), IF CMD
  JUMP REL(dataout),  IF DATA_OUT
  JUMP REL(datain),   IF DATA_IN
  JUMP REL(end),      IF STATUS
  INT err1
```

with the expected-disconnect discipline visible in the code itself: `MOVE SCNTL2 & 0x7f to SCNTL2` ("expect disconnect") before `CLEAR ACK` and `WAIT DISCONNECT`, then `WAIT RESELECT REL(select_adr)` [4] Ch. 7, Figure 7-1.

The 4 KB SCRIPTS RAM is loaded either by CPU copy or by a MOVE MEMORY from host memory into the RAM's PCI address — the RAM base being read by setting CTEST2 bit 3 and reading SCRATCHB, then clearing the bit [1] § 2.2.4; [4] Ch. 9. Once the program (or the hot parts of it) sits in the RAM, instruction fetches and table-indirect fetches from it "remain internal to the chip and do not use the PCI bus" [1] § 2.2.4.

### 4.3 How the Network Server's AIX driver drives it

The installed AIX 4.1.5's SCSI driver for these controllers is IBM's RS/6000 `pscsidd` reworked for the 825A, split into an unpinned half (configuration, ioctl, command build) and a pinned half containing the interrupt handler and the register accessors [8]. Its SCRIPTS program, reconstructed from the chip's RAM after the driver loads it, shows the pattern the manuals teach, used hard:

- a dispatcher that polls a command **ring/mailbox** in host memory by LOAD/STORE against a DSA-relative nexus block, keeping a one-byte **state code** for the current command in the nexus (values include 1 = "selecting the target", 7 = "selecting to send ABORT/BUS DEVICE RESET", 9 = idle in WAIT RESELECT, and a set of per-phase codes);
- **self-patching** — the target ID byte is stored into the SELECT instruction before each selection;
- two SELECT ATNs (one for commands, one for abort/BDR with a RAM message buffer);
- `WAIT RESELECT` as the idle state, with SIGP as the host's kick.

The interrupt handler reads ISTAT, loops while an interrupt shows, reads the 16-bit SIST pair on SIP, and classifies by *exact equality* against single causes — STO, UDC, SGE, PAR, MA (phase mismatch), RST — with any multi-cause word treated as unknown status (§ 3.6). On STO it inspects the script's state byte: state 7 (abort/BDR select failed) clears the mailbox quietly; a normal probe that failed leads to a command failure. On UDC with a state byte indicating an expected disconnect it proceeds; with any other state it runs a full cleanup — ISTAT.ABRT, software reset, register re-init, a SCSI bus reset — and rebuilds the queues [8].

The observable cost of this design on a machine with empty bays is large and is *the hardware's* behavior as much as the driver's: each empty ID costs a selection time-out (~205 ms per probe as measured on a real install boot, a figure matching the 50 MHz STIME0 table's 204.8 ms entry — § 6), a chip reset, a SCSI bus reset and a driver-level settle, repeated for every absent ID on two wide buses [8].

### 4.4 Resets

Four distinct resets touch the part, and they are not interchangeable:

| Reset | Effect | Source |
|---|---|---|
| PCI RST/ (hardware) | full reset; all PCI logic to known state; SCSI pins 3-stated (even on the JTAG variant — see § 5) | [1] § 3.1.1 |
| ISTAT.SRST (software) | all *operating* registers to defaults, SCSI signals deasserted; **no SCSI RST/ pulse**; PCI configuration untouched; COM bit survives | [1] § 4.2, Register 0x14 |
| SCNTL1.RST (SCSI reset) | asserts SCSI RST/ *until cleared*; the 25 µs SCSI minimum must be timed by the driver or a script loop; the receiving side is edge-detected into SIST0.RST | [1] §§ 4.2 Register 0x01, 4.2 Register 0x42 |
| Power state D3 (AE) | SCSI clock + PLL off, soft reset held, PCI Command cleared | [1] § 2.5.2 |

The Network Server adds a fifth, mechanical one: hot swap "is accomplished through: 1) a mechanically timed assertion, implemented through the use of long and short pins, of SCSI reset; and 2) advance power and ground, also through long pins" — insertion or removal of a drive carrier asserts SCSI RST/ "for the time it takes to unmate or mate the long vs. short pins", measured at a minimum of 5 ms for typical pin lengths, detected by XOR logic on the backplane [5] § 7.2.4.

### 4.5 The backplane contract

The seven-slot hot-swap backplane is "organized as a pair of fast and wide buses with narrow compatibility", driven through the two 825As, leaving the logic board through the blind-mate mezzanine; "Active terminators are used on the Main Logic Board for signal integrity" [5] § 7.2.1. Drives take their SCSI IDs from the backplane wiring of the carrier connectors [5] § 7.2.3. A stuffing option can join the two buses into one, with the mid-backplane terminators disabled by "stealing a ground signal from the mating ribbon cable" [5] § 7.2.3.

The optional RAID card "sits in PCI BUS 0 Slot 1" and cables back into the logic board over the RAID access connectors — industry-standard wide SCSI pinout except that "pin 19 is stolen to disable the Main Logic Board termination, and TERMPOWER is not implemented" [5] §§ 2.4, 7.2.2 — plus a 26-pin control interface for the front-panel drive-fail LEDs (FailDrive0–6, Bus0StrobeL latching LEDs 0–3, Bus1StrobeL latching LEDs 4–6, `ledOE` which "must be driven to ground by the mating interface to enable the LED function") [5] § 7.2.2.1. This is why a RAID-equipped Network Server shows per-drive status with no main-logic-board software involvement.

Apple's device requirements for the fast/wide drives are explicit: "Remote start, single-ended, full SCSI-2 with tag-queuing optimization" [5] § 7.3 — remote start because spinning up seven drives at once would overwhelm the supply, and single-ended because the 825As are strapped SE on this board.

## 5. Quirks & errata

- **The device ID does not identify the part.** 53C825 and 53C825A both answer device ID 0x0003 [2] Ch. 3; only the Revision ID's upper nibble separates them (0x14 vs a pre-A value), and the Network Server ROM's probe tests exactly that bit [7]. Software that keys on the device ID alone sees a 53C825.
- **ISTAT is the only register the CPU may touch while SCRIPTS runs.** Every other register access during execution "will interfere with the operation of the chip" [1] § 4.2 — including reads that look harmless.
- **A selection time-out brings an unexpected disconnect with it.** UDC "may occur before, at the same time, or stacked after the STO interrupt" [1] § 4.2, Register 0x42 — a driver must tolerate all three orderings, and one that decomposes SIST words by exact single-cause equality treats the simultaneous case as an unspecified error [8].
- **The SELECT alternate address is not the time-out exit.** It is specified for "selected or reselected during the selection" [1] § 5.4.2; [4] Ch. 3 — the manuals never route a time-out there, and the chip halts on the interrupt instead. Code that leaves an unrelocated offset in the alternate-address field only works *because* the chip never takes it on a time-out (*observed* on the ANS driver's own program) [8].
- **STO is fatal, masked or not.** Like all SCSI fatal interrupts, masking it suppresses only IRQ/; the engine still halts and "the system will never know it unless it times out and checks the ISTAT" [1] §§ 2.4.13.3–2.4.13.4.
- **Interrupt reads need spacing.** Consecutive 8-bit reads of DSTAT/SIST0/SIST1 need 12 CLK between them to clear properly, and the SIST pair should be read before DSTAT when in doubt [1] § 4.2, Register 0x42.
- **INTF clears only by writing 1**, and must be serviced before any SIP/DIP condition; reading the other status registers while only INTF is set is explicitly forbidden [1] § 4.2, Register 0x14.
- **IRQ/ is latched.** Masking an enabled interrupt after IRQ/ asserted does not deassert the pin, and between stacked levels the pin drops for only three CLKs [1] §§ 2.4.13.5, 4.2 Register 0x39.
- **One CALL level.** TEMP is not a stack; nested calls overwrite the return address, and a RETURN without a CALL silently "returns" to whatever TEMP holds [1] § 5.6.1.
- **A zero-length Block Move is an illegal instruction** (outside target command phase) [1] § 4.2, Register 0x0C.
- **DSP writes start the engine.** Unless DMODE.MAN or DCNTL.SSM is set, any write of a start address to DSP begins fetching — and when written a byte at a time, it is the *upper* byte that releases the engine [1] § 4.2, Register 0x2C.
- **Endian mode moves register addresses by 0x80.** The same file appears at 0x00–0x7F (big-endian) or 0x80–0xFF (little-endian) of any window [1] § 2.4.5; SCRIPTS-internal register addressing is endian-independent.
- **The 53C825AJ is little-endian only** — the JTAG pins consume BIG_LIT/ — and its RST/ pin 3-states the SCSI pins even during boundary scan, which "is not compliant with the specification" [1] §§ 1.2, 2.4.4.
- **The 88-byte FIFO is a compatibility mode.** The physical FIFO is 536 bytes; CTEST5.DFS shrinks the *accounting* to 88 so older-family residual arithmetic still works — and the DFIFO/DBC subtraction formula differs between the two [1] §§ 2.4.8, 4.2 Register 0x20.
- **SCNTL0.START may need manual clearing.** The ANS driver's script clears it "to compensate for chip problem" [8]; no vendor manual in the evidence set documents the problem (§ 6).
- **SCNTL1.RST is level, not pulse.** The line stays asserted until the bit is cleared, and the 25 µs SCSI minimum is the software's job [1] § 4.2, Register 0x01.
- **Timers need a zero-then-value write** to restart cleanly or change periods repeatably [1] § 4.2, Register 0x49.
- **Never set STEST2.EXT during Fast SCSI** — the widened glitch filter can swallow a valid assertion [1] § 4.2, Register 0x4E.
- **VUE1 disables the CDB-length auto-reload.** With it clear, the chip silently rewrites DBC after the first command byte of a standard group code — a surprise if the script budgeted a different count [1] § 5.3.1.
- **RESPID is not SCID.** Arbitration uses one ID; *response* to selection/reselection can be a different, wider set [1] § 4.2, Register 0x4A.
- **Apple cannot decide what to call the chip.** One Apple document uses "53C825A", "53C825" and "NCR 825A" in three sections [5] §§ 2.9, 4.6.2, 7.1.1, 7.2.1 — and § 7.2.1 once says "the 820 device", evidently meaning the 825A.
- **MESH is absent from the Network Server.** The machine's SCSI complement is two 53C825As plus the Curio 53C94 narrow bus; no `mesh` node exists in its device tree [5] § 4.6.1; [7] (see also [ncr-53c96.md](ncr-53c96.md)).
- **The 4 KB RAM exists only if MAD5 says so.** The RAM is pin-enabled; a single 4.7 kΩ resistor from MAD5 to VSS removes it [1] § 2.2.4.

## 6. Open questions

1. **The STO/UDC ordering rule.** The manual permits UDC "before, at the same time, or stacked after" STO [1] § 4.2, Register 0x42 but never says which ordering a plain selection time-out on an empty ID actually produces, nor what selects it (SCLK frequency, CCF, whether SIEN0.UDC is enabled). Drivers observe all three as distinct cases [8]; the physical determinants are undocumented.
2. **The SCNTL0.START "chip problem."** The ANS driver's script comment implies a known erratum — the START bit not self-clearing in some selection-time-out path — but no errata sheet for the 53C825A exists in the evidence set, and the vendor manuals describe START as automatically cleared [1] § 4.2, Register 0x00.
3. **Published errata generally.** None of the four vendor documents in the evidence set carries an errata appendix for arbitration, selection or SCRIPTS defects.
4. **The ANS SCLK frequency and STIME0 value.** The observed ~205 ms probe time-out matches the 50 MHz column's 204.8 ms (SEL = 1100) rather than any 40 MHz-table entry (*inferred — unverified*); the ANS board's fitted SCLK and the driver's actual STIME0 programming are not stated in the evidence.
5. **Apple's "40 Mbytes/sec".** Whether the Network Server figure is per channel or the two channels' aggregate (§ 1.5) — the per-chip synchronous ceiling in the vendor manual is 20 MB/s wide.
6. **The initiator SCSI ID of the two controllers.** The backplane assigns *drive* IDs [5] § 7.2.3; the value programmed into the two chips' SCID (and whether the two channels use different IDs) is not in the evidence.
7. **The 825A revision on the ANS.** The Revision ID 0x14 is revision-level 4; the ANS documents do not state which silicon stepping the machines shipped with, and pre-G revisions have hardwired-zero subsystem IDs [2] Ch. 4.
8. **What the ROM's evt2 path does with the controller** beyond identity probing — the over-temperature poll it arms is documented only from ROM behavior, not from any Apple text [7].
9. **GPIO usage on the ANS.** Whether Apple's board wires any of the five GPIO pins (drive LEDs, terminator control, differential sense as vendor software uses them [1] § 4.2, Register 0x07) is not stated; the RAID LED path is a separate 26-pin interface, suggesting GPIO is unused there.
10. **The `AAPL,interrupts` value 0x1D (29)** of the 53C875 card example [6] — its mapping into the TNT-platform Grand Central interrupt space is not traced here.

## References

1. LSI Logic Corporation, *LSI53C825A/LSI53C825AE PCI to SCSI I/O Processor Technical Manual*, Version 3.1, January 2001, document DB14-000159-00 (Fourth Edition; revision 3.0, 12/97, was the Symbios-branded edition) — Ch. 1 "Introduction" (§§ 1.1–1.4); Ch. 2 "Functional Description" (§ 2.1.1 Configuration Space, § 2.1.3 PCI Cache Mode pp. 2-4 ff., § 2.2.1 SCSI Core, § 2.2.2 DMA Core, § 2.2.3 SCRIPTS Processor, § 2.2.4 Internal SCRIPTS RAM p. 2-11, § 2.2.6 Prefetching SCRIPTS Instructions p. 2-13, § 2.2.7 Opcode Fetch Burst, § 2.3 External Memory Interface, § 2.4.4 JTAG, § 2.4.5 Big and Little Endian Support p. 2-19, § 2.4.7 Parity Options, § 2.4.8 DMA FIFO p. 2-23 and § 2.4.8.1 Data Paths, § 2.4.9 SCSI Bus Interface, § 2.4.10 Select/Reselect During Selection/Reselection p. 2-33, § 2.4.11 Synchronous Operation p. 2-33, § 2.4.12 Achieving Optimal SCSI Send Rates p. 2-34, § 2.4.13 Interrupt Handling p. 2-35, § 2.4.14 Chained Block Moves p. 2-42, § 2.5 Power Management); Ch. 3 "Signal Descriptions" (§§ 3.1.1–3.1.8); Ch. 4 "Registers" (§ 4.1 Configuration Registers p. 4-1, § 4.2 Operating Registers p. 4-18 and Table 4.2 Register Map, every per-register description cited by register name); Ch. 5 "SCSI SCRIPTS Instruction Set" (§ 5.2 High Level SCSI SCRIPTS Mode, § 5.3 Block Move Instructions p. 5-6, § 5.4 I/O Instruction p. 5-14, § 5.5 Read/Write Instructions p. 5-24, § 5.6 Transfer Control Instructions p. 5-29, § 5.7 Memory Move Instructions p. 5-36, § 5.8 Load and Store Instructions p. 5-40); Ch. 6 "Specifications" (Tables 6.1–6.5).
2. Symbios Logic Inc., *SYM53C825A/825AE PCI-SCSI I/O Processor Data Manual*, Version 3.0, © 1995 — Ch. 3 "PCI Configuration Registers" (Register 02h Device ID, Register 08h Revision ID); Ch. 4 external-memory and subsystem-data MAD strapping (Tables 4-11, 4-12, 4-13); signal descriptions (BIG_LIT/).
3. Symbios Logic Inc., *SYM53C875/875E PCI-Ultra SCSI I/O Processor Data Manual*, Version 4.0 (Rev 2.0, 3/96, "Fast-20 changed to Ultra SCSI throughout … added SCSI clock doubler") — Ch. 1 "Introduction" (Benefits of Ultra SCSI, features); Ch. 2 (Clock Doubler); Ch. 3 (Device ID 000Fh); feature list "wide, Ultra SCSI synchronous transfers as fast as 40 MB/s".
4. Symbios Logic Inc., *PCI-SCSI I/O Processors Programming Guide*, Version 2.1, © 1995–1997 — Ch. 3 "The SYM53C8XX Instruction Set" (SELECT, WAIT RESELECT, SET/CLEAR, and the other instruction entries); Ch. 6 "Using the Registers to Control Chip Operations" (Tables 6-7 and 6-8 startup bits); Ch. 7 "Integrating SCRIPTS Programs Into 'C' Language Drivers" (Running a SCRIPTS Program, Figure 7-1 sample program, patching); Ch. 9 "SCRIPTS Programming Topics" (Using the SCRIPTS RAM, Loading SCRIPTS RAM, Interrupt Handling, Halting in an Orderly Fashion, Sample Interrupt Service Routine); Ch. 10 "Multi-Threaded I/O" (Using the SIGP bit to Abort an Instruction).
5. Apple Computer, Inc., *Apple Network Server 500/700 Hardware Developer Notes* (internal, Network Server / "Shiner") — § 2.9 Fast/Wide SCSI; § 2.10 Low-Skew Clocking; § 4.2 Network Server External Interrupt Map; § 4.4 PCI discovery order; § 4.6.1 Grand Central DMA channel and device assignments; § 4.6.2 Other PCI Devices; § 7.1.1 PCI Device Configuration; § 7.2 Network Server SCSI Expansion (§ 7.2.1 Theory of Operation, § 7.2.2 Main Logic Board to RAID Card Interface, § 7.2.2.1 26-Pin RAID Control Interface, § 7.2.3 SCSI ID / backplane bus pinouts, § 7.2.4 Hot Swap, § 7.2.5 Narrow Device Compatibility); § 7.3 SCSI Device Requirements.
6. Apple Computer, Inc., *Apple Network Server 500/700 Software Developer Notes* — the Open Firmware device-tree walkthrough of a Symbios 53C875 SCSI card "installed in the bottom PCI slot": the `.properties` dump of `/bandit@F4000000/53c875@10`.
7. Apple Computer, Inc., Apple Network Server 500/700 boot ROM (the machine's Open Firmware ROM image) — *observed* behavior only: the `53c825@11`/`53c825@12` node naming and properties, the Revision-ID-bit-4 825A probe and its consequences, and the absence of a `mesh` node.
8. International Business Machines Corporation, AIX 4.1.5 for the Apple Network Server — the kernel extensions `/usr/lib/drivers/pscsidd` and `/usr/lib/drivers/pscsiddpin` (the RS/6000 `pscsidd` driver, `bos/kernext/scsi/pscsiddb.c` in the published AIX 4.1.3 source, reworked for the 53C825A) — cited for *observed driver behavior* only: the SCRIPTS program structure (mailbox, nexus state codes, self-patching SELECT, WAIT RESELECT/SIGP), the interrupt handler's exact-equality classification of SIST, the STO and UDC recovery paths, the "manually clear Start bit to compensate for chip problem" script comment, and the measured per-probe cost of selecting an absent ID.
