# Macintosh IIcx System Architecture

The Macintosh IIcx (Gestalt ID 6, introduced March 1989) is a compact modular desktop Macintosh built around a Motorola 68030 CPU at 15.6672 MHz and a Motorola 68882 FPU. It shares its 256 KB system ROM (ROM ID `$0178`, checksum `$97221136`) with the Macintosh IIx and SE/30, and uses the same GLUE ASIC (344S0602-A) for address decoding and interrupt routing. The IIcx distinguishes itself from its siblings through two hardware ID bits: **VIA1 PA6 = 1** and **VIA2 PB3 = 1**.

This document describes the IIcx's physical memory map, I/O wiring, VIA configuration, interrupt architecture, NuBus expansion, video subsystem, and the specific hardware differences that set it apart from the SE/30 and IIx.

---

## Processor and Bus

| Parameter | Value |
|-----------|-------|
| CPU | Motorola MC68030, 15.6672 MHz |
| FPU | Motorola MC68882 (coprocessor interface, line-F traps) |
| Address bus | 32-bit physical |
| Data bus | 32-bit external |
| Instruction cache | 256 bytes (16 lines × 4 longwords) |
| Data cache | 256 bytes (16 lines × 4 longwords) |
| Burst mode | **Disabled** — the GLUE does not generate burst-acknowledge signals; all cache fills are single-entry |
| Bus clock (C7M) | 7.8336 MHz (master crystal / 2) |
| VIA clock | 783.36 kHz (C7M / 10) |

The 68030's integrated PMMU is used by the operating system to translate 24-bit logical addresses to 32-bit physical addresses when running in 24-bit mode. The ROM boots in 24-bit mode by default; MODE32 or 32-bit-clean ROMs switch to full 32-bit addressing.

The 68882 FPU is memory-mapped through the 68030's coprocessor bus protocol. The CPU communicates with the FPU using line-F instructions, which the 68030 translates into coprocessor interface bus cycles. No explicit I/O addresses are involved.

---

## Physical Memory Map

The IIcx's 4 GB address space is divided into fixed regions by the GLUE ASIC:

| Physical Address Range | Size | Region | Description |
|------------------------|------|--------|-------------|
| `$00000000`–`$3FFFFFFF` | 1 GB | RAM | System DRAM. Eight 30-pin SIMM slots in two banks (A and B). Officially up to 32 MB; theoretically 128 MB with MODE32. |
| `$40000000`–`$4FFFFFFF` | 256 MB | ROM | 256 KB ROM mirrored throughout this range (incomplete decoding). |
| `$50000000`–`$5FFFFFFF` | 256 MB | I/O | Memory-mapped peripherals. Mirrored every `$20000` (128 KB). |
| `$60000000`–`$8FFFFFFF` | 768 MB | NuBus super slots | Reserved for super slots `$6`–`$8`. Not physically connected. |
| `$90000000`–`$9FFFFFFF` | 256 MB | NuBus slot `$9` super | 256 MB super-slot space for slot `$9`. |
| `$A0000000`–`$AFFFFFFF` | 256 MB | NuBus slot `$A` super | 256 MB super-slot space for slot `$A`. |
| `$B0000000`–`$BFFFFFFF` | 256 MB | NuBus slot `$B` super | 256 MB super-slot space for slot `$B`. |
| `$C0000000`–`$EFFFFFFF` | 768 MB | NuBus reserved | Reserved for super slots `$C`–`$E`. Not physically connected on IIcx. |
| `$F0000000`–`$F0FFFFFF` | 16 MB | NuBus→MLB translation | NuBus cards access MLB I/O (`$F0000000`→`$50000000`) and ROM (`$F0800000`→`$40800000`) through this window. |
| `$F1000000`–`$F8FFFFFF` | 128 MB | Standard slots (unused) | Standard slot space for slots `$1`–`$8`. Not connected. |
| `$F9000000`–`$F9FFFFFF` | 16 MB | Slot `$9` standard | Standard slot space for slot `$9` (declaration ROM, small framebuffers). |
| `$FA000000`–`$FAFFFFFF` | 16 MB | Slot `$A` standard | Standard slot space for slot `$A`. |
| `$FB000000`–`$FBFFFFFF` | 16 MB | Slot `$B` standard | Standard slot space for slot `$B`. |
| `$FC000000`–`$FFFFFFFF` | 64 MB | Standard slots (unused) | Standard slot space for slots `$C`–`$F`. Not physically connected on IIcx. |

### ROM Overlay

On reset, the GLUE maps the system ROM at address `$00000000` so the 68030 can fetch valid boot vectors (initial SSP and PC). VIA1 Port A bit 4 (`vOverlay`) is forced high by hardware at reset. The ROM's early boot code clears this bit, which disables the overlay and exposes RAM at `$00000000`.

### RAM Banking

The GLUE's memory controller maps eight SIMM slots into Bank A (4 SIMMs) and Bank B (4 SIMMs). A hardware restriction requires Bank A ≥ Bank B in capacity. The `$0178` ROM limits recognized RAM to 8 MB at boot. MODE32 reprograms the GLUE's memory controller registers to expose more RAM and reconfigures the 68030 PMMU to remap physically fragmented banks into a contiguous logical block.

The IIcx supports 256 Kbit, 1 Mbit, and 16 Mbit DRAM ICs. 4 Mbit ICs are **not supported** due to a GLUE-related refresh timing issue shared with the SE/30. SIMMs must be 120 ns or faster.

VIA2 Port A bits 7:6 (`v2RAM1:v2RAM0`) encode the IC density of Bank A, written by the ROM during RAM sizing:

| v2RAM1 | v2RAM0 | IC Density |
|--------|--------|------------|
| 0 | 0 | 256 Kbit |
| 0 | 1 | 1 Mbit |
| 1 | 0 | 4 Mbit |
| 1 | 1 | 16 Mbit |

### 24-Bit Addressing Mode

The `$0178` ROM is "32-bit dirty" — the operating system stores flags in the upper 8 bits of master pointers. The 68030 PMMU is configured to mask the upper byte during 24-bit mode, keeping the effective address space to 16 MB. In 24-bit mode, the logical memory map is:

| Logical Address | Maps To |
|----------------|---------|
| `$000000`–`$7FFFFF` | RAM |
| `$800000`–`$8FFFFF` | ROM |
| `$900000`–`$EFFFFF` | NuBus standard slot space (slots `$9`–`$E`) |
| `$F00000`–`$FFFFFF` | I/O space |

In 24-bit mode, access to NuBus super-slot space (starting at `$90000000`) is not possible; all slot transactions use the standard slot space. The PMMU maps logical `$9x xxxx` to physical `$F90x xxxx` (and similarly for slots `$A` and `$B`), allowing legacy 24-bit software to reach the declaration ROMs and small framebuffers of installed NuBus cards.

---

## I/O Device Map

The I/O region begins at `$50000000`. Each device occupies an 8 KB (`$2000`) window. The entire device block mirrors every `$20000` bytes (128 KB) due to the GLUE's incomplete address decoding. The ROM and system software rely on this mirroring.

| Offset | Physical Address | Device | Description |
|--------|-----------------|--------|-------------|
| `$00000` | `$50000000` | **VIA1** | ADB transceiver interface, RTC serial, floppy head select, ROM overlay, sound volume |
| `$02000` | `$50002000` | **VIA2** | Slot interrupt aggregation, SCSI/ASC interrupt routing, machine ID, power control |
| `$04000` | `$50004000` | **SCC** | Zilog Z8530 serial controller — modem (Port A) and printer (Port B) |
| `$06000` | `$50006000` | **SCSI pseudo-DMA (handshake)** | NCR 5380 pseudo-DMA with DRQ hardware handshaking |
| `$10000` | `$50010000` | **SCSI registers** | NCR 5380 direct register access (polling mode) |
| `$12000` | `$50012000` | **SCSI pseudo-DMA (blind)** | NCR 5380 pseudo-DMA bypassing DRQ handshake |
| `$14000` | `$50014000` | **ASC** | Apple Sound Chip (344S0063) — FIFO and wavetable synthesis |
| `$16000` | `$50016000` | **SWIM** | Floppy disk controller (344S0062) — IWM/ISM modes |

### NuBus-to-Motherboard I/O Translation

NuBus cards cannot directly access the `$50000000` I/O space. The GLUE provides a translation window at `$F0000000`:

| NuBus Address | MLB Address | Target |
|---------------|-------------|--------|
| `$F0000000`–`$F07FFFFF` | `$50000000`–`$507FFFFF` | Motherboard I/O |
| `$F0800000`–`$F0FFFFFF` | `$40800000`–`$40FFFFFF` | System ROM |

NuBus cards also have direct access to main RAM at `$00000000`–`$3FFFFFFF`.

### VIA Register Addressing

Within each VIA's 8 KB window, the 16 registers are spaced at 512-byte (`$200`) intervals because only address lines A9–A12 are decoded:

| Offset | Register | Name |
|--------|----------|------|
| `$0000` | ORB/IRB | Data Register B |
| `$0200` | ORA/IRA | Data Register A (with handshake) |
| `$0400` | DDRB | Data Direction Register B |
| `$0600` | DDRA | Data Direction Register A |
| `$0800` | T1CL | Timer 1 Counter Low |
| `$0A00` | T1CH | Timer 1 Counter High |
| `$0C00` | T1LL | Timer 1 Latch Low |
| `$0E00` | T1LH | Timer 1 Latch High |
| `$1000` | T2CL | Timer 2 Counter Low |
| `$1200` | T2CH | Timer 2 Counter High |
| `$1400` | SR | Shift Register |
| `$1600` | ACR | Auxiliary Control Register |
| `$1800` | PCR | Peripheral Control Register |
| `$1A00` | IFR | Interrupt Flag Register |
| `$1C00` | IER | Interrupt Enable Register |
| `$1E00` | ORA (no HS) | Data Register A (no handshake) |

VIA1 registers: `$50000000 + offset`. VIA2 registers: `$50002000 + offset`.

---

## VIA1 — ADB, RTC, System Control

VIA1 (Rockwell 65C22, board position UK12) handles the machine's timing-sensitive serial peripherals: the ADB transceiver, the real-time clock, floppy head selection, and the ROM overlay toggle.

### VIA1 Port A

| Bit | Mask | Name | Dir | Function |
|-----|------|------|-----|----------|
| 7 | `$80` | vSccWrReq | In | SCC write/request (active-low, wire-ORed channels A+B) |
| 6 | `$40` | CPUID1 | In | **Machine ID bit 1** — hardwired to **1** on the IIcx (SE/30: also 1; IIx: 0) |
| 5 | `$20` | vHeadSel | Out | Floppy disk head select (SEL line to SWIM) |
| 4 | `$10` | vOverlay | Out | ROM overlay — 1 = ROM at `$00000000` (set at reset, cleared during boot) |
| 3 | `$08` | vSync | Out | Synchronous modem enable for SCC Port A |
| 2–0 | | | Out | Reserved (manufacturing burn-in test); bit 0 must read **1** to avoid forced diagnostic boot |

### VIA1 Port B

| Bit | Mask | Name | Dir | Function |
|-----|------|------|-----|----------|
| 7 | `$80` | vSndEnb | Out | Sound enable (0 = enabled, active-low); legacy control, superseded by ASC |
| 6 | `$40` | — | — | **Not connected** on IIcx (SE/30: vSyncEnA video VBL enable; IIci: parity enable) |
| 5 | `$20` | vADBS2 (ST1) | Out | ADB state bit 1 — to ADB transceiver MCU |
| 4 | `$10` | vADBS1 (ST0) | Out | ADB state bit 0 — to ADB transceiver MCU |
| 3 | `$08` | vADBInt | In | ADB interrupt from transceiver (0 = active, active-low) |
| 2 | `$04` | vRTCEnb | Out | RTC chip select (0 = enabled, active-low) |
| 1 | `$02` | vRTCClk | Out | RTC serial clock |
| 0 | `$01` | vRTCData | I/O | RTC serial data (direction set by DDRB bit 0) |

### VIA1 CA1/CA2 and CB1/CB2

| Pin | Signal | IFR Bit | Description |
|-----|--------|---------|-------------|
| CA1 | VBlank | 1 (`$02`) | 60.15 Hz vertical blanking pulse from VIA2 Timer 1 PB7 output (negative-edge triggered) |
| CA2 | One-second tick | 0 (`$01`) | 1 Hz pulse from the RTC chip's CKO pin |
| CB1 | ADB clock | 4 (`$10`) | External clock from ADB transceiver MCU (342S0440) |
| CB2 | ADB data | 3 (`$08`) | Bidirectional ADB data line |
| SR | ADB byte complete | 2 (`$04`) | Fires when a complete 8-bit ADB byte has shifted in or out |

### VIA1 Timers

- **Timer 1**: Free-running mode (ACR bits 7:6 = `01`). Generates the system heartbeat tick (~60.15 Hz).
- **Timer 2**: One-shot mode. Used for ADB communication timing.

---

## VIA2 — Slot Interrupts, SCSI/ASC Routing, Machine ID, Power Control

VIA2 (Rockwell 65C22, board position UK11) aggregates interrupt sources from the NuBus expansion slots, SCSI controller, and sound chip. Unlike the IIci's RBV substitute, the IIcx's VIA2 is a real 6522 with fully functional Timer 1 and Timer 2.

### VIA2 Port A

| Bit | Mask | Name | Dir | Function |
|-----|------|------|-----|----------|
| 7 | `$80` | v2RAM1 | Out | RAM size bit 1 (set by ROM at startup) |
| 6 | `$40` | v2RAM0 | Out | RAM size bit 0 (set by ROM at startup) |
| 5 | `$20` | vIRQE | In | Slot `$E` interrupt (active-low) — **not physically connected** on IIcx |
| 4 | `$10` | vIRQD | In | Slot `$D` interrupt (active-low) — **not physically connected** on IIcx |
| 3 | `$08` | vIRQC | In | Slot `$C` interrupt (active-low) — **not physically connected** on IIcx |
| 2 | `$04` | vIRQB | In | **Slot `$B` interrupt** (active-low) — physical NuBus slot |
| 1 | `$02` | vIRQA | In | **Slot `$A` interrupt** (active-low) — physical NuBus slot |
| 0 | `$01` | vIRQ9 | In | **Slot `$9` interrupt** (active-low) — physical NuBus slot |

Unconnected slot interrupt lines (bits 5–3) float high (inactive) on the IIcx.

### VIA2 Port B

| Bit | Mask | Name | Dir | Function |
|-----|------|------|-----|----------|
| 7 | `$80` | v2VBL | Out | VBL output (driven by VIA2 Timer 1 at 60.15 Hz); wired to VIA1 CA1 |
| 6 | `$40` | v2SndJck | In | **Sound jack detect** (0 = headphone jack inserted). Functional on IIcx; SE/30 always reads 0. |
| 5 | `$20` | v2TM0A | In | NuBus transfer acknowledge bit 0 |
| 4 | `$10` | v2TM1A | In | NuBus transfer acknowledge bit 1 |
| 3 | `$08` | CPUID0 | In | **Machine ID bit 0** — hardwired **high (1)** on the IIcx (SE/30: low; IIx: low) |
| 2 | `$04` | v2PowerOff | Out | Power-off (0 = shut down power supply); soft power control |
| 1 | `$02` | v2BusLk | Out | NuBus bus lock (0 = NuBus transactions locked out) |
| 0 | `$01` | v2CDis | Out | Cache disable (0 = disable 68030 instruction and data caches) |

### VIA2 CA1/CA2 and CB1/CB2

| Pin | Signal | IFR Bit | Description |
|-----|--------|---------|-------------|
| CA1 | Slot /NMRQ | 1 (`$02`) | OR of all slot interrupt lines (via GLUE); read Port A to identify source |
| CA2 | SCSI DRQ | 0 (`$01`) | Data request from NCR 5380 for pseudo-DMA |
| CB1 | ASC IRQ | 4 (`$10`) | Interrupt from Apple Sound Chip |
| CB2 | SCSI IRQ | 3 (`$08`) | Interrupt from NCR 5380 SCSI controller |

### VIA2 Timers

- **Timer 1**: Generates the 60.15 Hz VBL timing pulse on PB7 (`v2VBL`), which is wired to VIA1 CA1.
- **Timer 2**: Available for general system use.

### NuBus Interrupt Dispatch

When VIA2 CA1 fires, the CPU reads VIA2 Port A to determine which slot asserted its interrupt. Individual slot interrupts cannot be masked at the VIA level — only the CA1 umbrella interrupt can be enabled/disabled.

The dispatch path: NuBus card asserts `/NMRQ` → GLUE ORs all slot lines → asserts VIA2 CA1 → VIA2 fires `/IRQ` → GLUE encodes IPL 2 → CPU reads VIA2 Port A to identify source slot → Slot Manager dispatches to card's driver.

---

## Machine Identification

The `$0178` ROM identifies the hardware by reading two bits early in the boot sequence:

| Model | VIA1 PA6 | VIA2 PB3 | Combined |
|-------|----------|----------|----------|
| Macintosh IIx | 0 | 0 | `$00` |
| Macintosh II | 0 | 1 | `$01` |
| Macintosh SE/30 | 1 | 0 | `$02` |
| **Macintosh IIcx** | **1** | **1** | **`$03`** |

On the IIcx, VIA2 PB3 is hardwired high through a pull-up. VIA1 PA6 reads 1 through the GLUE logic. The ROM uses these bits to branch into model-specific initialization for expansion slot configuration, RAM sizing, and video card detection.

---

## Interrupt Architecture

Both VIA `/IRQ` outputs connect independently to the GLUE ASIC, which encodes them onto the 68030's three IPL lines as autovector interrupt levels. **VIA2 does not cascade through VIA1.**

| IPL Level | Source | Autovector | Description |
|-----------|--------|------------|-------------|
| 1 | VIA1 | `$64` | ADB, VBlank (CA1), one-second clock (CA2), Timer 1/2, Shift Register |
| 2 | VIA2 | `$68` | Slot interrupts (CA1), SCSI DRQ (CA2), SCSI IRQ (CB2), ASC IRQ (CB1), Timer 1/2 |
| 4 | SCC | `$70` | Zilog Z8530 serial controller (direct to GLUE, bypasses VIAs) |
| 6 | — | — | Power switch (active on IIcx soft-power hardware) |
| 7 | NMI | `$7C` | Programmer's switch (non-maskable) |

Levels 3 and 5 are unused.

### VBL Generation

Unlike the SE/30 which has a built-in video circuit that generates a hardware VSync signal, the IIcx has **no built-in video**. The 60.15 Hz VBL timing pulse is generated entirely by VIA2 Timer 1 running in free-run mode with PB7 output. This pulse is wired to VIA1 CA1, which fires the IPL level 1 VBL interrupt used by the OS for task scheduling.

NuBus video cards may independently generate their own VSync interrupts, which appear as slot-level interrupts in VIA2 Port A (bits 0–2 for slots `$9`–`$B`). These fire through VIA2 CA1 as IPL level 2 interrupts. The Slot Manager uses the `SlotQDT` (Slot Queue Dispatch Table) at low-memory global `$0D04` to route these to the appropriate video driver.

---

## ADB Subsystem Wiring

The IIcx uses the Apple 342S0440 ADB transceiver (a PIC microcontroller derivative) connected through VIA1:

| Signal | VIA1 Pin | Description |
|--------|----------|-------------|
| State bit 1 (ST1) | PB5 | Transaction state output to transceiver |
| State bit 0 (ST0) | PB4 | Transaction state output to transceiver |
| Interrupt (vADBInt) | PB3 | Active-low interrupt from transceiver (data ready) |
| Shift register data | SR | Byte-serial transfers (command and data) |
| External clock | CB1 | Clock from transceiver (~330 µs per bit) |
| Data line | CB2 | Bidirectional ADB data |

### ADB Transaction States (ST1:ST0)

| State | Bits | Meaning |
|-------|------|---------|
| 0 | 00 | Command phase — CPU writes command byte to SR |
| 1 | 01 | Even data byte transfer |
| 2 | 10 | Odd data byte transfer |
| 3 | 11 | Idle — transceiver auto-polls devices every ~11 ms |

The ADB shift register uses an externally-clocked mode: ACR bits 4:2 = `111` for shift-out (CPU → transceiver), or `011` for shift-in (transceiver → CPU). The transceiver's MCU provides the clock on CB1. When a complete byte shifts, IFR bit 2 (SR complete) fires.

---

## RTC Wiring

The Real-Time Clock (Apple 343-0042) connects through VIA1 Port B using a 3-wire bit-banged serial protocol:

| Signal | VIA1 Pin | Description |
|--------|----------|-------------|
| Chip select | PB2 (vRTCEnb) | Active-low: 0 = RTC selected |
| Clock | PB1 (vRTCClk) | Serial clock, toggled by CPU |
| Data | PB0 (vRTCData) | Bidirectional serial data (direction via DDRB bit 0) |

The CPU asserts chip select low, then clocks an 8-bit command followed by data bytes. On each rising clock edge, data is latched. The RTC's CKO pin provides a 1 Hz pulse to VIA1 CA2.

---

## SCC Wiring

The Zilog Z8530 SCC is mapped at `$50004000` and provides two RS-422 serial ports:

| Port | Function | Clock Source |
|------|----------|-------------|
| Port A | Modem | 3.672 MHz (C3.7M) or external GPi via vSync bit |
| Port B | Printer | 3.672 MHz (C3.7M), fixed |

- The SCC runs on a dedicated 3.672 MHz peripheral clock (PCLK), not the 15.6672 MHz system clock.
- Port A has higher interrupt priority than Port B within the SCC.
- The SCC's interrupt output connects directly to the GLUE for IPL level 4, bypassing both VIAs entirely.
- VIA1 PA7 (`vSccWrReq`) reflects the SCC's write/request line.

---

## SCSI Wiring

The NCR 5380 (or compatible Zilog 53C80) SCSI controller is accessed through three address windows:

| Address | Mode | Description |
|---------|------|-------------|
| `$50010000` | Direct register | Polling access to 5380 internal registers |
| `$50006000` | Pseudo-DMA (handshake) | DRQ-mediated pseudo-DMA; GLUE suspends CPU via `/DSACK` until DRQ asserts |
| `$50012000` | Pseudo-DMA (blind) | No DRQ check; hardware handshake without waiting for data-ready |

### Pseudo-DMA Mechanism

When the CPU performs a 32-bit longword read at `$50006000`:

1. The GLUE fragments the request into four sequential 8-bit transfers to the 5380.
2. The GLUE holds `/DSACK` high, stalling the 68030 mid-instruction.
3. When the 5380 asserts DRQ (byte ready), the GLUE pulses `/DSACK` to latch each byte.
4. After four bytes, the full longword is returned and the instruction completes.

The SCSI controller's interrupt (IRQ) connects to **VIA2 CB2** and its data request (DRQ) connects to **VIA2 CA2**. The internal SCSI bus ID is 7 (initiator).

---

## ASC Wiring

The Apple Sound Chip (344S0063) is mapped at `$50014000` and provides 8-bit stereo audio output through dual Sony DAC chips:

| Signal | Connection |
|--------|------------|
| ASC IRQ | VIA2 CB1 — fires IPL level 2 when FIFO reaches half-empty |
| Audio output | Two Sony sound chips (DACs) + analog amplifier |
| Headphone jack | Rear panel — stereo mini-jack with hardware detect |
| Internal speaker | Driven by ASC; **disconnects when headphone jack is inserted** |

VIA2 PB6 (`v2SndJck`) reflects the physical state of the headphone jack: 0 when a plug is inserted, 1 when unplugged. This is a functional detection circuit on the IIcx (unlike the SE/30 where this bit always reads 0).

VIA1 PB7 (`vSndEnb`) is a legacy sound-enable control (0 = enabled). On the IIcx, this controls the analog audio output stage. The ASC itself manages digital playback independently.

---

## SWIM Wiring

The SWIM floppy controller (344S0062) is mapped at `$50016000` and drives a single internal 1.44 MB FDHD (SuperDrive). A DB-19 external floppy connector on the rear panel supports a second drive. VIA1 PA5 (`vHeadSel`) provides the floppy disk head select signal.

The SWIM supports IWM mode (400 KB/800 KB GCR disks) and ISM mode (1.44 MB MFM disks). Mode switching requires a specific 4-write sequence toggling data bit 6 in the pattern 1, 0, 1, 1.

---

## Video Subsystem

The Macintosh IIcx has **no built-in video hardware**. It requires a NuBus video card installed in one of its three expansion slots to produce display output. This is the primary architectural difference between the IIcx and the SE/30 (which has integrated 512×342 monochrome video emulating NuBus slot `$E`).

### NuBus Video Card Operation

A NuBus video card occupies one of the three physical slots (`$9`, `$A`, or `$B`). Its framebuffer resides in the super-slot address space (256 MB per slot), and its declaration ROM resides in the standard slot space (16 MB per slot):

| Component | Slot `$9` | Slot `$A` | Slot `$B` |
|-----------|-----------|-----------|-----------|
| Framebuffer (super slot) | `$90000000`–`$9FFFFFFF` | `$A0000000`–`$AFFFFFFF` | `$B0000000`–`$BFFFFFFF` |
| Declaration ROM (standard) | `$F9000000`–`$F9FFFFFF` | `$FA000000`–`$FAFFFFFF` | `$FB000000`–`$FBFFFFFF` |

At boot, the Slot Manager probes each slot's standard slot space for a valid declaration ROM. The declaration ROM follows the NuBus `FHeaderRec` format (byte-lane test pattern `$5A932BC7`, sResource directory with board and video resources). The Slot Manager reads the card's `PrimaryInit` code and executes it to initialize the video hardware.

### Video Interrupts

NuBus video cards typically generate their own VSync interrupts, which assert the slot's `/NMRQ` line. This appears in VIA2 Port A at the corresponding bit (bit 0 for slot `$9`, bit 1 for slot `$A`, bit 2 for slot `$B`), causing a VIA2 CA1 interrupt at IPL level 2.

The OS routes these slot-level VBL interrupts to the video driver via the Slot Manager's `SlotQDT` dispatch table. This is separate from the system-wide VBL interrupt generated by VIA2 Timer 1 at IPL level 1.

### No Slot `$E` Video Hardware

Unlike the SE/30, the IIcx has no VRAM at `$FEE00000` and no VROM at `$FEFF0000`. Accesses to slot `$E` address space will bus-error unless a card is physically present (which is not possible on the IIcx — slots `$C`–`$E` have no physical connectors).

---

## NuBus Expansion

The IIcx provides three 96-pin NuBus expansion slots. The NuBus controller is the Apple NuChip ASIC (344S0606), the same chip used in the Mac II and IIx.

### Slot Geographic IDs

Each slot has a hardcoded 4-bit ID established by grounding or floating the identification pins (`/ID0`–`/ID3`) at the physical connector:

| Physical Slot | Slot ID | Super-Slot Space | Standard Slot Space |
|---------------|---------|------------------|---------------------|
| Slot 1 (leftmost) | `$9` | `$90000000`–`$9FFFFFFF` | `$F9000000`–`$F9FFFFFF` |
| Slot 2 (center) | `$A` | `$A0000000`–`$AFFFFFFF` | `$FA000000`–`$FAFFFFFF` |
| Slot 3 (rightmost) | `$B` | `$B0000000`–`$BFFFFFFF` | `$FB000000`–`$FBFFFFFF` |

### Super Slot vs. Standard Slot Space

- **Super-slot space** (256 MB per slot): Used for large framebuffers, memory expansion, or DSP cards. Available only in 32-bit addressing mode.
- **Standard slot space** (16 MB per slot): Used for declaration ROMs and small resources. Accessible in both 24-bit and 32-bit modes (via PMMU translation in 24-bit mode).

### NuBus Bus Characteristics

| Parameter | Value |
|-----------|-------|
| Bus width | 32 bits (multiplexed address/data) |
| Bus clock | 10 MHz |
| Transfer modes | Block, single |
| Arbitration | Fair — NuChip implements round-robin with backoff |
| Bus lock | VIA2 PB1 (`v2BusLk`) — 0 = NuBus transactions locked out |
| Connector pins A2/C2 | **Grounded** (IIci/IIfx leave these open) |

### NuBus Interrupt Routing

Each slot's `/NMRQ` line is routed through the GLUE ASIC, which performs a logical OR of all slot interrupts and drives VIA2 CA1. The CPU reads VIA2 Port A to identify the source slot:

| Slot | VIA2 Port A Bit | Active |
|------|-----------------|--------|
| `$9` | Bit 0 (`vIRQ9`) | Low = asserted |
| `$A` | Bit 1 (`vIRQA`) | Low = asserted |
| `$B` | Bit 2 (`vIRQB`) | Low = asserted |
| `$C` | Bit 3 | Not connected (reads high) |
| `$D` | Bit 4 | Not connected (reads high) |
| `$E` | Bit 5 | Not connected (reads high) |

---

## Power Supply

The IIcx uses a modular external-style power supply with soft power control:

| Parameter | Value |
|-----------|-------|
| Input voltage | 85–270 VAC, 47–63 Hz (universal) |
| +5V rail | 2–12 A |
| +12V rail | 20 mA – 1.5 A |
| −12V rail | 20 mA – 1 A |
| Maximum continuous | 90 W |
| Peak (15 sec, 10% duty) | 108 W |
| Standby | +5V at 1 mA (for soft power-on) |

### Soft Power Control

The IIcx supports soft power-on: the power supply provides a standby +5V rail that keeps VIA2 and the ADB transceiver minimally powered. A keyboard power key or a NuBus card can trigger the power supply to activate. Power-off is controlled by VIA2 PB2 (`v2PowerOff`) — writing 0 to this bit shuts down the power supply.

---

## Signal Polarity Summary

Most control and interrupt signals follow active-low conventions:

**Active-low inputs:**
- All slot /NMRQ lines (VIA2 PA0–PA5)
- vADBInt (VIA1 PB3)
- vSccWrReq (VIA1 PA7)
- SCSI IRQ and DRQ (VIA2 CB2, CA2)
- ASC IRQ (VIA2 CB1)
- VBlank (VIA1 CA1, falling-edge triggered)
- v2SndJck (VIA2 PB6) — 0 = jack inserted

**Active-low outputs:**
- vRTCEnb (VIA1 PB2) — 0 = RTC selected
- vSndEnb (VIA1 PB7) — 0 = sound on
- v2PowerOff (VIA2 PB2) — 0 = power off
- v2BusLk (VIA2 PB1) — 0 = bus locked

**Active-high outputs:**
- vOverlay (VIA1 PA4) — 1 = ROM overlaid at `$00000000`
- vSync (VIA1 PA3) — 1 = synchronous modem enabled

---

## IIcx vs. SE/30 — Key Differences

Despite sharing the same ROM and most logic, several hardware details differ between the IIcx and SE/30:

| Feature | IIcx | SE/30 |
|---------|------|-------|
| Built-in video | **None** — requires NuBus video card | 512×342 mono, VRAM at `$FEE00000`, VROM at slot `$E` |
| VIA1 PA6 | CPUID1 = 1 (machine ID input) | vPage2 — alternate screen buffer select |
| VIA1 PB6 | **Not connected** | vSyncEnA — video VSync interrupt enable (0 = on) |
| VIA2 PB3 | Hardwired **high** (machine ID = 1) | Hardwired low (machine ID = 0) |
| VIA2 PB6 (v2SndJck) | **Functional** — reads 0 when jack inserted | Always reads 0 (no jack detect hardware) |
| VIA2 PB2/PB1 | **Soft power-off** and NuBus bus lock | Routed to PDS connector |
| Expansion | **3 × NuBus slots** (IDs `$9`, `$A`, `$B`) | 1 × 120-pin PDS (pseudo-slots `$9`/`$A`/`$B`) |
| NuBus controller | **NuChip** (344S0606) | None (GLUE decodes PDS as pseudo-NuBus) |
| Sound output | ASC → Sony DACs; **speaker disconnects on jack insert** | ASC → Sony DACs + TL071 amp; dedicated speaker mixes L+R |
| Power supply | Modular 90 W, **soft power on/off** | Internal 75 W (SE case), hard power switch |
| Floppy | 1 internal + **1 external (DB-19)** | 1 internal only |
| Form factor | Compact modular desktop (separate monitor) | All-in-one compact with 9" CRT |
| Cache burst mode | Disabled (GLUE limitation) | Also disabled |

---

## Boot Sequence Summary

1. **Reset**: GLUE asserts ROM overlay (ROM at `$00000000`). CPU fetches SSP and PC from the ROM image.
2. **ROM overlay disable**: Early ROM code clears VIA1 PA4, exposing RAM at `$00000000`.
3. **Machine identification**: ROM reads VIA1 PA6 (=1) and VIA2 PB3 (=1) → identifies IIcx.
4. **RAM sizing**: Memory controller probes SIMMs. ROM configures Bank A and Bank B, writes IC density to VIA2 PA7:PA6. In 24-bit mode, max 8 MB recognized.
5. **PMMU setup**: 68030 PMMU configured for 24-bit address translation (masks upper 8 bits).
6. **Slot Manager scan**: Probes NuBus standard slot space at `$F9000000`, `$FA000000`, `$FB000000`. For each occupied slot, reads declaration ROM, executes `PrimaryInit`, loads drivers.
7. **Video initialization**: Slot Manager initializes the NuBus video card found during slot scan. If no video card is present, the machine has no display output.
8. **ADB initialization**: `_ADBReInit` sends `SendReset` (`$00`) to reset all devices, enumerates addresses 1–15 via `Talk R3`, builds device table (keyboard at address 2, mouse at address 3).
9. **RTC read**: Parameter RAM and clock data read via bit-banged serial protocol through VIA1 PB2/PB1/PB0.
10. **System startup**: OS loads from SCSI disk or floppy. Normal VBL-driven polling begins for ADB, cursor updates, and display refresh.
