# Macintosh Plus SCSI Subsystem Documentation

This document provides a comprehensive description of the SCSI subsystem of the Apple Macintosh Plus (1986). It integrates information about the host bus integration, the NCR 5380/53C80 host controller, the SCSI bus, and connected devices. It is designed to serve as a reference for emulator development and includes register-level detail, bus phases, protocol semantics, and device command handling.

---

## 1. Host Integration

### 1.1 Memory-Mapped Addressing

* The Macintosh Plus integrates the **NCR 5380 SCSI controller** into the second 4 MB address block of the 68000 CPU memory map.
* SCSI registers appear at base address `$580000` with decoding to support mirrored access and pseudo-registers for DMA.
* Address formation:

  * Bits encode read/write (`n`), DMA acknowledge (`d`), and register number (`r`).
  * Formula: `$580drn`.
* **Mirroring**: Registers repeat across the block due to partial decoding; emulator must replicate mirroring.
* **Access granularity**:

  * Reads must occur at **even addresses**, writes at **odd addresses**.
  * Illegal access patterns (e.g., word or longword) generate **bus errors** on real hardware.

### 1.2 Read/Write Side Effects

* Many registers clear interrupt or status latches on read (e.g., CSR, BSR, Reset/Parity)【12†source】.
* Correct emulation requires modeling of these **read-to-clear semantics**.

### 1.3 Cycle Accuracy

* Accesses to the SCSI chip may stall the 68000 by inserting wait states if READY is deasserted【10†source】.
* For high-fidelity emulation, timing windows (BUS FREE, ARBITRATION DELAY, SELECTION timeout, etc.) must be observed【14†source】.

### 1.4 Endianness and Alignment

* The 5380 is an **8-bit peripheral**; all accesses are byte-oriented.
* The 68000 big-endian alignment must be respected: registers map only to the **even or odd halves** of the data bus.

---

## 2. Host Controller: NCR 5380

The NCR 5380 provides a minimal but flexible host adapter, exposing direct SCSI line control and DMA facilities. 

### Programmed I/O vs. DMA Modes in the 5380

The NCR 5380 provided a spectrum of data transfer methodologies, ranging from complete CPU management to semi-autonomous operations.

**Programmed I/O (PIO):** In this fundamental mode, the host CPU is responsible for every aspect of the data transfer. This includes directly managing the SCSI bus handshake by polling status registers and manually asserting the /REQ (Request) and /ACK (Acknowledge) signals by writing to the chip's control registers. While this offers granular control, it fully occupies the CPU, making it inefficient for large data transfers.

**DMA Mode**: The 5380 provides a "DMA Mode," which can be enabled by setting a specific bit in its Mode Register. When active, the chip's internal state machine automatically handles the high-frequency /REQ//ACK handshaking on the SCSI bus. This significantly reduces the burden on the host CPU. However, the term "DMA Mode" in the context of the 5380 is ambiguous, as the chip itself does not perform memory access. It merely provides signals to facilitate DMA by an external controller.   

**"Pseudo DMA"**: This term describes a specific implementation technique where the 5380 is placed in its hardware-handshaking DMA mode, but the host CPU (not a dedicated DMA controller) is responsible for moving each byte of data between system memory and the chip's data register. The CPU determines when the chip is ready for the next byte by polling a status bit or monitoring a hardware signal. This hybrid approach, which offloads the SCSI bus handshake but still uses the CPU for data movement, is the foundation of the classic MacOS "blind transfer" method.

### Pseudo DMA in MacOS

The process worked as follows: The SCSI Manager would initiate a blind transfer by entering a tight software loop that repeatedly executed a MOVE instruction to read from or write to the memory-mapped address of the NCR 5380's data register. The custom logic on the Macintosh motherboard connected the SCSI controller's "data ready" status to the CPU's /DTACK line. When the CPU executed a MOVE instruction to access the SCSI chip, the bus cycle would begin. If the SCSI controller was not yet ready for the data transfer (for example, it was still waiting for the /REQ//ACK handshake to complete on the SCSI bus), the motherboard logic would withhold the /DTACK signal from the CPU.

A 68000-family processor is designed to pause, or enter "wait states," whenever /DTACK is not asserted during a bus cycle. This effectively freezes the CPU—and the software loop—at the hardware level until the SCSI controller signals its readiness by allowing /DTACK to be asserted. Once /DTACK is received, the bus cycle completes, the byte is transferred, and the CPU immediately proceeds to the next iteration of the loop to transfer the next byte.

This created an exceptionally efficient transfer method. The software was "blind" because it contained no polling logic, but the system was not. The CPU was synchronized to the peripheral at the hardware bus cycle level, eliminating the overhead of a software polling loop. While this achieved very high speeds for a single-tasking system, its reliance on tight timing made it brittle. On later machines like the Macintosh SE and II, additional hardware support was added to make this /DTACK handshake more robust, but on the original Macintosh Plus, it remained a delicate, timing-dependent balance.   

### Interrupt-driven Model in AUX

The core principle of a multitasking UNIX kernel is that no single process should be able to monopolize the CPU or halt the entire system while waiting for a slow operation like I/O. The CPU-stalling /DTACK method used by classic MacOS was therefore architecturally unacceptable for A/UX. Instead, A/UX implemented a conventional and robust interrupt-driven model, using the specific hardware signals provided by the NCR 5380 for this purpose.

The 5380 controller provides both a DRQ (DMA Request) signal to indicate that it is ready to transfer a byte of data and an IRQ (Interrupt Request) signal to alert the CPU. Technical notes for the Macintosh II hardware explicitly mention a memory-mapped region for "SCSI (pseudo-DMA w/DRQ on II)," confirming that the DRQ signal was wired to be accessible by the system for I/O control. In the A/UX model, the DRQ signal would be configured to trigger a hardware interrupt. This allows the CPU to initiate a SCSI transfer, then immediately switch to executing another process. When the SCSI controller is ready for data, the DRQ-triggered interrupt forces the CPU to pause its current task, execute a small, fast Interrupt Service Routine (ISR) to handle the data transfer, and then resume its previous work. This asynchronous model is the foundation of efficient multitasking I/O.


### Macintosh Plus

The IRQ and DRQ interrupt signals do not generate CPU interrupts in the Macintosh Plus. Software must poll the SCSI controller's Bus and Status register to determine whether a SCSI interrupt is pending.



Its registers control all initiator and target signals, with specific side effects (read-to-clear latches, arbitration tracking, REQ/ACK edge sampling). For emulation, faithfully implement:

* Each register’s bitfields and dual read/write semantics.
* Automatic arbitration (AIP/LA) and interrupt latch behavior.
* Strict read/write address alignment rules.
* Pseudo-DMA mappings unique to Macintosh Plus.

---

## 2.1 Register Map and Addressing

The 5380 exposes **eight logical registers** addressed through A2–A0 with read/write strobes. The Macintosh Plus maps these into the `$580000` region of the 68000 memory map, with mirroring and special cases for DMA acknowledges【41†Inside\_Macintosh†L250-L255】.

| A2 A1 A0 | Access | Register Name                 |
| -------- | ------ | ----------------------------- |
| 000      | R      | Current SCSI Data (CSR)       |
| 000      | W      | Output Data Register (ODR)    |
| 001      | R/W    | Initiator Command (ICR)       |
| 010      | R/W    | Mode Register (MR)            |
| 011      | R/W    | Target Command (TCR)          |
| 100      | R      | Current Bus Status (CBSR)     |
| 100      | W      | Select Enable Register (SER)  |
| 101      | R      | Bus and Status Register (BSR) |
| 101      | W      | Start DMA Send                |
| 110      | R      | Input Data Register (IDR)     |
| 110      | W      | Start DMA Target Receive      |
| 111      | R      | Reset Parity / Interrupt      |
| 111      | W      | Start DMA Initiator Receive   |

Reads must be on **even addresses**, writes on **odd addresses**; otherwise a bus error occurs【41†Inside\_Macintosh†L252-L253】.

---

## 2.2 Data Registers

### 2.2.1 Current SCSI Data Register (CSR, addr=0, R-only)

* Transparent view of DB\[7:0] lines on the bus.
* Used for monitoring arbitration or checking incoming bytes.
* Parity may be checked if enabled.
* Not latched: reflects current state【42†NCR5380†L10-L11】.

### 2.2.2 Output Data Register (ODR, addr=0, W-only)

* Drives data onto DB\[7:0] when acting as initiator or target.
* Also used during arbitration to present host ID.
* When written with DACK asserted, supports DMA output【42†NCR5380†L10-L11】.

### 2.2.3 Input Data Register (IDR, addr=6, R-only)

* Latches data from bus during DMA input (REQ/ACK edges).
* Parity optionally checked when loaded.
* Can be read under programmed I/O or DMA with DACK【42†NCR5380†L11-L12】.

---

## 2.3 Initiator Command Register (ICR, addr=1, R/W)

This register controls key initiator signals and reflects arbitration state【42†NCR5380†L11-L12】.

* **Read bits:**

  * Bit 7: Assert RST (status)
  * Bit 6: Arbitration in Progress (AIP)
  * Bit 5: Lost Arbitration (LA)
  * Bits 4–0: reflect asserted lines (ACK, BSY, SEL, ATN, DATA)

* **Write bits:**

  * Bit 7: ASSERT RST → drives RST onto SCSI bus.
  * Bit 6: TEST MODE → disables outputs for diagnostics.
  * Bit 5: DIFF ENBL (unused on 5380; 5381 only).
  * Bit 4: ASSERT ACK
  * Bit 3: ASSERT BSY
  * Bit 2: ASSERT SEL
  * Bit 1: ASSERT ATN
  * Bit 0: ASSERT DATA (drives ODR value).

Special semantics:

* Setting **ASSERT RST** forces internal reset (except this bit).
* AIP/LA bits automatically update during arbitration.

---

## 2.4 Mode Register (MR, addr=2, R/W)

Controls global operation modes【42†NCR5380†L13-L14】.

* Bit 7: DMA Mode Enable (enables DRQ/DACK transfer).
* Bit 6: Target Mode Enable.
* Bit 5: Parity Check Enable.
* Bit 4: Parity Enable (generation on output).
* Bit 3: Arbitration Enable.
* Bit 2: Reserved.
* Bit 1: Monitor Bus Mode (forces CSR continuous sample).
* Bit 0: Block Mode (affects DMA burst behavior).

---

## 2.5 Target Command Register (TCR, addr=3, R/W)

Asserts phase lines during target role【42†NCR5380†L14-L15】.

* Bit 2: Assert MSG
* Bit 1: Assert C/D
* Bit 0: Assert I/O
* Bits 7–3: reserved.

These three lines define the **current bus phase** (together with REQ/ACK).

---

## 2.6 Current Bus Status Register (CBSR, addr=4, R-only)

Read-only snapshot of SCSI signals【42†NCR5380†L15-L16】.

* Bit mapping directly mirrors ATN, BSY, SEL, ACK, RST, MSG, C/D, I/O.
* Polled to detect bus transitions.

---

## 2.7 Select Enable Register (SER, addr=4, W-only)

Defines which target IDs the chip will respond to during reselection【42†NCR5380†L15-L16】.

* Writing a bit mask enables recognition of that ID on SEL.
* Example: setting bit 7 allows recognition as host ID 7.

---

## 2.8 Bus and Status Register (BSR, addr=5, R-only)

Provides latched status and interrupt sources【42†NCR5380†L15-L16】.

* Bits:

  * Phase Match / Mismatch.
  * Interrupt pending.
  * Arbitration lost.
  * End of Process (EOP).
* Must be cleared by reading Reset/Parity register.

---

## 2.9 DMA Registers (Pseudo)

### 2.9.1 Start DMA Send (addr=5, W-only)

* Arms the DMA engine for sending data from host to SCSI bus.
* Writing triggers DMA request cycle【42†NCR5380†L16-L17】.

### 2.9.2 Start DMA Target Receive (addr=6, W-only)

* Prepares to receive data from initiator into IDR.

### 2.9.3 Start DMA Initiator Receive (addr=7, W-only)

* Prepares to receive data from target into IDR.

---

## 2.10 Reset Parity / Interrupt Register (addr=7, R-only)

* Reading clears latched parity error and interrupt pending conditions【42†NCR5380†L17-L18】.
* Also acknowledges completion of many events.

---

## 2.11 Interrupt Semantics

* Events generating IRQ (not wired on Mac Plus, but emulated):

  * REQ transition (phase change)
  * Parity error
  * Arbitration lost
  * End of DMA process
  * Bus reset detected【42†NCR5380†L18-L22】
* IRQ is level-active until cleared by Reset/Parity read.

---

## 2.12 Operational Notes

* **Read side effects:** reading BSR or Reset register clears latched conditions.
* **Write semantics:** many registers are write-only; reads return undefined values.
* **DMA:** pseudo-DMA via CPU accesses to special mirrored addresses asserts DACK automatically on Macintosh Plus【41†Inside\_Macintosh†L252-L253】.

---

## 3. SCSI Bus

### 3.1 Electrical Characteristics

* Open-collector, active-low signaling with **wired-OR resolution**【12†source】.
* Data: **DB\[0–7] + DBP (odd parity)**.
* Control: BSY, SEL, ATN, ACK, REQ, MSG, C/D, I/O, RST.
* Single-ended (Mac Plus DB-25 connector) vs standard 50-pin ribbon.
* No terminator power provided by Mac Plus.

### 3.2 Bus Phases【14†source】

1. **BUS FREE** – no device drives BSY or SEL.
2. **ARBITRATION** – initiators assert BSY+ID; highest ID wins.
3. **SELECTION** – initiator asserts SEL+target ID.
4. **RESELECTION** – target reconnects initiator.
5. **COMMAND** – initiator → target CDB.
6. **DATA IN/OUT** – bulk transfer with REQ/ACK.
7. **STATUS** – target sends completion byte.
8. **MESSAGE IN/OUT** – exchange of protocol messages.

### 3.3 Arbitration

* Priority: ID7 highest, ID0 lowest【14†source】.
* Bus settle and arbitration delay timers (\~2.4 µs in SCSI-1, 2.2 µs earlier).
* Lost arbitration detected by ICR.LA bit.

### 3.4 Handshake Protocol (REQ/ACK)

* Target drives REQ, initiator responds with ACK.
* One byte transferred per REQ/ACK handshake【14†source】.
* Setup/hold requirements define minimum period; asynchronous only on 5380.

### 3.5 Error Conditions

* Parity error detected if odd parity not met.
* Illegal phase combinations trigger Bus+Status mismatch interrupt【12†source】.

---

## 4. SCSI Devices and Command Set & Data Structures

This chapter specifies the exact wire-level formats for SCSI commands, data, status, and messages required to implement a correct Macintosh Plus–era emulator. Unless noted, multi-byte numeric fields are **big-endian** (most significant byte first) and lengths are in **bytes**. The 5380/53C80 transports data **asynchronously**; synchronous/wide options may be rejected by targets or the emulator.

### Historical Disk Models

Apple's original SCSI hard drives used in the Macintosh Plus era were typically rebranded drives from various manufacturers. The following table documents some of the specific models and their characteristics:

| Apple Model | Vendor     | Model       | Cylinders | Sectors/Track | Heads | Total Blocks |
|-------------|------------|-------------|-----------|---------------|-------|--------------|
| HD20SC      | Seagate    | ST225N      | 615       | 17            | 4     | 41820        |
| HD20SC      | Miniscribe | M8425S      | 612       | 17            | 4     | —            |
| HD40SC      | Conner     | CP2040A     | 1026      | 40            | 2     | —            |
| HD80SC      | Quantum    | Q280        | 823       | 32            | 6     | —            |
| HD160SC     | CDC        | Vren V HH   | —         | —             | —     | —            |

These specifications reflect the physical geometry reported by the drives, though modern emulation typically abstracts away cylinder-head-sector (CHS) addressing in favor of logical block addressing (LBA).

### 4.2 Command Descriptor Blocks (CDBs)

SCSI groups opcodes by CDB length:

* **Group 0 (6‑byte)**: opcodes `0x00–0x1F` (most core SCSI‑1 commands).
* **Group 1 (10‑byte)**: opcodes `0x20–0x5F` (READ(10), WRITE(10), READ CAPACITY(10), VERIFY(10), etc.).
* **Group 5 (12‑byte)**: opcodes `0xA0–0xBF` (READ(12), WRITE(12) and some optical/other).

#### 4.2.1 Common 6‑byte (Group 0) CDB layout

| Byte | Bits | Meaning                                                                     |
| ---- | ---- | --------------------------------------------------------------------------- |
| 0    | 7..0 | **Operation Code**                                                          |
| 1    | 7..5 | **LUN** (legacy addressing)                                                 |
| 1    | 4..0 | **High 5 bits** of LBA (for READ/WRITE/VERIFY) or command-specific          |
| 2    | 7..0 | **Middle 8 bits** of LBA                                                    |
| 3    | 7..0 | **Low 8 bits** of LBA                                                       |
| 4    | 7..0 | **Transfer Length** (0 means 256 blocks for READ/WRITE) or parameter length |
| 5    | 7..0 | **Control** (usually 0)                                                     |

> Notes: Some Group 0 commands do not carry LBA fields (e.g., TEST UNIT READY, INQUIRY). For those, bytes 1–4 are command-specific.

#### 4.2.2 Common 10‑byte (Group 1) CDB layout

| Byte | Meaning                                                                |
| ---- | ---------------------------------------------------------------------- |
| 0    | Operation Code                                                         |
| 1    | Flags (RelAddr, FUA, DPO, etc.; device-type specific) + LUN (obsolete) |
| 2–5  | **Logical Block Address (LBA)**                                        |
| 6    | Group Number (upper LBA bits in some older devices)                    |
| 7–8  | **Transfer Length** (number of blocks)                                 |
| 9    | Control                                                                |

#### 4.2.3 Common 12‑byte (Group 5) CDB layout

| Byte | Meaning                |
| ---- | ---------------------- |
| 0    | Operation Code         |
| 1    | Flags + LUN (obsolete) |
| 2–5  | **LBA**                |
| 6–9  | **Transfer Length**    |
| 10   | Reserved/flags         |
| 11   | Control                |

### 4.3 Status Byte (target → initiator)

|   Code | Name                     | Meaning                                                      |
| -----: | ------------------------ | ------------------------------------------------------------ |
| `0x00` | **GOOD**                 | Command completed successfully                               |
| `0x02` | **CHECK CONDITION**      | Sense data available; initiator must issue **REQUEST SENSE** |
| `0x08` | **BUSY**                 | Target/LUN busy; retry later                                 |
| `0x18` | **RESERVATION CONFLICT** | Access denied due to another initiator’s reservation         |
| `0x28` | **QUEUE FULL** (SCSI‑2)  | Target queue depth exceeded (if tagged queuing)              |

> The emulator should terminate each command with STATUS then **MESSAGE IN: COMMAND COMPLETE (0x00)** unless an exception occurs.

### 4.4 Message System (selection of common messages)

**Single‑byte messages (Message In unless noted):**

* `0x00` **COMMAND COMPLETE**
* `0x02` **SAVE DATA POINTER**
* `0x03` **RESTORE POINTERS**
* `0x04` **DISCONNECT**
* `0x06` **MESSAGE REJECT**
* `0x07` **NO OPERATION**
* `0x08` **MESSAGE PARITY ERROR**
* `0x0C` **BUS DEVICE RESET**
* `0x0D` **ABORT**
* `0x5A` **INITIATOR DETECTED ERROR** (Message Out)

**IDENTIFY (Message Out)**: `0x80 | lun | (disconnect bit)`; bit7=1, bits2..0=LUN, bit6=1 if disconnects allowed.

**Extended Messages:**

* `0x01, 0x03, 0x01, <period>, <offset>` — **SYNCHRONOUS DATA TRANSFER REQUEST (SDTR)**
* `0x01, 0x02, 0x03, <width>` — **WIDE DATA TRANSFER REQUEST (WDTR)** (reject in Mac Plus async implementation)

> A 5380‑based Mac Plus implementation typically stays **asynchronous 8‑bit**. It should **MESSAGE REJECT** SDTR/WDTR unless you choose to emulate optional negotiation.

---

## 4.5 Mandatory & Common SCSI Commands (direct‑access devices)

Below, **CDB** shows byte‑accurate layouts and **Data** shows exact payloads. If a field/flag is marked “(opt)”, older SCSI‑1 devices may ignore it. Multi‑byte numeric fields are big‑endian.

### 4.5.1 TEST UNIT READY — `0x00` (6)

**Purpose:** Poll ready state; no data phase.

**CDB:** `00 | (lun<<5) | 00 | 00 | 00 | 00`

**Phases:** COMMAND → STATUS → MESSAGE IN.

**Status:** `GOOD` if ready; `CHECK CONDITION` with Sense Key **NOT READY** when spun down/media absent.

---

### 4.5.2 REQUEST SENSE — `0x03` (6)

**Purpose:** Fetch **fixed‑format sense data** after `CHECK CONDITION`.

**CDB:**

* Byte0 `0x03`
* Byte1 `(lun<<5)`
* Byte2 `0x00`
* Byte3 `0x00`
* Byte4 **Allocation Length** (suggest 0x12 (18) or 0x1B (27) for SCSI‑2)
* Byte5 `Control`

**Data (Fixed format, response code 0x70/0x71):**

|  Byte | Field                                                           |
| ----: | --------------------------------------------------------------- |
|     0 | Response Code (`0x70` current, `0x71` deferred)                 |
|     1 | Obsolete                                                        |
|     2 | **Sense Key** (4‑bit)                                           |
|   3–6 | Information (vendor/command‑specific or LBA for certain errors) |
|     7 | Additional Sense Length (n)                                     |
|  8–11 | Command‑specific information                                    |
|    12 | **ASC**                                                         |
|    13 | **ASCQ**                                                        |
|    14 | Field Replaceable Unit (FRU) (opt)                              |
| 15–17 | Sense‑key specific (sksv + cd\_bp) (opt)                        |
|  18.. | Additional bytes as reported by byte7                           |

**Common Sense Keys:** NO SENSE(0x0), RECOVERED(0x1), NOT READY(0x2), MEDIUM ERROR(0x3), HARDWARE ERROR(0x4), ILLEGAL REQUEST(0x5), UNIT ATTENTION(0x6), DATA PROTECT(0x7), BLANK CHECK(0x8), ABORTED COMMAND(0xB). Populate **ASC/ASCQ** (e.g., 0x3A/0x00 = Medium Not Present; 0x28/0x00 = Not Ready to Ready Change; 0x20/0x00 = Invalid Command; 0x24/0x00 = Invalid Field in CDB).

---

### 4.5.3 INQUIRY — `0x12` (6)

**Purpose:** Return device identity & capability.

**CDB:**

* Byte0 `0x12`
* Byte1 `(lun<<5) | (evpd<<0)` where **EVPD**=1 returns Vital Product Data page
* Byte2 **Page Code** (when EVPD=1; else 0)
* Byte3 `0x00`
* Byte4 **Allocation Length** (max bytes host accepts)
* Byte5 `Control`

**Data (Standard, EVPD=0):**

|  Byte | Field                                                                        |
| ----: | ---------------------------------------------------------------------------- |
|     0 | Peripheral Qualifier/Device Type (lower 5 bits type 0x00=Direct‑access disk) |
|     1 | RMB (bit7) Removable                                                         |
|     2 | Version (e.g., 0x02 = SCSI‑2)                                                |
|     3 | Response Data Format (lower nibble), flags                                   |
|     4 | **Additional Length** (n = remaining bytes)                                  |
|   5–7 | Flags (CmdQue, etc.)                                                         |
|  8–15 | **Vendor Identification** (8 ASCII)                                          |
| 16–31 | **Product Identification** (16 ASCII)                                        |
| 32–35 | **Product Revision Level** (4 ASCII)                                         |
|  36.. | (SCSI‑2+) optional fields                                                    |

**Data (EVPD):**

* Page `0x00` — Supported VPD pages
* Page `0x80` — Unit Serial Number
* Page `0x83` — Device Identification descriptors (ASCII/WWN/etc.)

---

### 4.5.4 MODE SENSE(6) — `0x1A` (6) and MODE SENSE(10) — `0x5A` (10)

**Purpose:** Read current/changeable/default/saved **Mode Pages** and optional **Block Descriptors**.

**CDB(6):** `1A | (dbd<<3)|(pc<<6)|(lun<<5) | Page Code | Subpage | 0 | Allocation Length | Control`

* **PC** (page control): 0=current, 1=changeable, 2=default, 3=saved
* **DBD**: 1 = **Disable Block Descriptors** (skip the block descriptors section)

**Response header (6‑byte form):**

| Byte | Field                                                                                                                        |
| ---: | ---------------------------------------------------------------------------------------------------------------------------- |
|    0 | **Mode Data Length** (excluding this byte)                                                                                   |
|    1 | Medium Type                                                                                                                  |
|    2 | Device‑Specific Parameter (bit7=WP for disks)                                                                                |
|    3 | Block Descriptor Length (m)                                                                                                  |
|  4.. | **Block Descriptors** (m bytes; 8 per descriptor for Direct‑access): Density, Number of Blocks(3), Reserved, Block Length(3) |
|   .. | **Mode Pages**: sequence of pages (see below)                                                                                |

**Common Mode Pages (Direct‑access):**

* `0x01` **Read‑Write Error Recovery** — retry, correction flags
* `0x02` **Disconnect/Reconnect** — BUS IN/OUT disconnect thresholds, bus inactivity timer
* `0x03` **Format Device** — sectors/track, interleave, track skew, cylinder skew
* `0x04` **Rigid Disk Geometry** — cylinders, heads, rpm
* `0x08` **Caching** — enable write cache, prefetch parameters
* `0x0A` **Control** — DQue, QErr, Busy timeout behavior, Task set mgmt

Each page begins with: `Page Code (7 bits) | PS (bit7)`, `Page Length`, followed by page‑specific bytes.

---

### 4.5.5 MODE SELECT(6) — `0x15` (6) and MODE SELECT(10) — `0x55` (10)

**Purpose:** Write mode pages (and optionally block descriptors). Often requires Unit Attention clearing and may be restricted (write‑protected, invalid fields).

**CDB(6):** `15 | (sp<<4)|(pf<<4)|(lun<<5) | 00 | 00 | Parameter List Length | Control`

* **PF** (Page Format) must be 1 for SCSI‑2 style pages
* **SP** (Save Pages) requests saving parameters in NVRAM if supported

**Parameter List:** same format as MODE SENSE header + descriptors + pages. Emulator must validate **changeable** bits (from PC=1 page) and reject invalid fields with `CHECK CONDITION` / `ILLEGAL REQUEST` (ASC/ASCQ `0x26/0x00` invalid field in parameter list).

---

### 4.5.6 READ(6) — `0x08` / WRITE(6) — `0x0A`

**Purpose:** Transfer logical blocks.

**CDB:** per Group 0 layout: LBA is 21 bits across bytes 1–3; **Transfer Length** byte (0 → 256 blocks).

**Phases:**

* READ: COMMAND → **DATA IN** (length = blocks × block\_size) → STATUS → MESSAGE IN.
* WRITE: COMMAND → **DATA OUT** → STATUS → MESSAGE IN.

**Errors:**

* LBA beyond last block: `CHECK CONDITION` **ILLEGAL REQUEST** (ASC/ASCQ `0x21/0x00` LBA out of range).
* If **DATA PROTECT** (write protect): `DATA PROTECT` (0x07) with `0x27/0x00`.

---

### 4.5.7 READ(10) — `0x28` / WRITE(10) — `0x2A`

**Purpose:** Same as 6‑byte but with 32‑bit LBA and 16‑bit transfer length.

**CDB(READ(10)) bytes:** `28, flags, LBA[31..24], LBA[23..16], LBA[15..8], LBA[7..0], group, xferLen[15..8], xferLen[7..0], control`.

**Behavior:** If `Transfer Length` = 0 → no data (some devices interpret as no-op). Prefer explicit non‑zero.

---

### 4.5.8 READ CAPACITY(10) — `0x25`

**Purpose:** Report media geometry for block I/O.

**CDB:** `25 00 00 00 00 00 00 00 00 00`

**Data (8 bytes):**

* **\[0–3]**: **Last Logical Block Address** (LBA of final block)
* **\[4–7]**: **Block Length in bytes**

> Capacity in blocks = last\_lba + 1. If image size changes hot, set Unit Attention (ASC/ASCQ `0x2A/0x09` parameters changed). For disks > 2TB you’d need READ CAPACITY(16) (SCSI‑3+), not applicable to Mac Plus era.

---

### 4.5.9 START STOP UNIT — `0x1B`

Controls spindle or load/unload (for removable).

**CDB:** `1B | (immed<<0) | 00 | 00 | (LoEj<<1)|(Start) | Control`

* `Start=1` spin up; `0` spin down
* `LoEj` load/eject for removable

**Emulation:** optionally simulate spin‑up delay; on not‑ready return `NOT READY` with `ASC/ASCQ 0x04/0x02` (becoming ready) until spun up.

---

### 4.5.10 PREVENT/ALLOW MEDIUM REMOVAL — `0x1E`

**CDB:** `1E 00 00 00 (Prevent=1?0x01:0x00) 00`

**Semantics:** If Prevent=1, reject eject operations with `ILLEGAL REQUEST`/`0x53/0x02` (medium removal prevented). Emulator maps to image lock.

---

### 4.5.11 RESERVE — `0x16` / RELEASE — `0x17`

**Purpose:** Simple (pre‑SCSI‑3) reservations per LUN or extent.

**CDB(RESERVE(6))**: `16 | (3rdParty<<5)|(lun<<5) | 00 | 00 | ParamListLen | 00`

* Most Mac Plus setups are single‑initiator; it’s acceptable to implement **no‑op** behavior, or honor basic “RESERVATION CONFLICT” for other initiators on multi‑initiator buses.

---

### 4.5.12 VERIFY(10) — `0x2F`

Verifies data on media without transfer; may be used by some utilities.

**CDB:** like READ(10) with op `0x2F`. If mismatch detection isn’t implemented, return `GOOD` (or support byte‑by‑byte compare with optional BYTCHK bit where present).

---

### 4.5.13 FORMAT UNIT — `0x04`

Low‑level format / defect map; rarely issued by classic Mac OS to fixed disks in the field (typically vendor tools). It carries a **Parameter List** describing defect lists and interleave.

**CDB:** `04 | DefectListFmt | Vendor | InterleaveMSB | InterleaveLSB | Control`

**Parameter List (when PF):** Header + optional defect list. For emulator, accept and ignore formatting parameters but **reinitialize** image (zero‑fill) if desired; otherwise reject with `ILLEGAL REQUEST` if Write‑Protect.

---

## 4.6 Sense, Unit Attention, and Error Policy

### 4.6.1 CHECK CONDITION flow

1. Target ends command with **CHECK CONDITION**.
2. Initiator issues **REQUEST SENSE** (same nexus) to retrieve sense data.
3. Emulator returns fixed‑format sense per 4.4.2.

### 4.6.2 Common conditions to model

* **UNIT ATTENTION** on power‑on/reset, media change, or parameters changed.

  * Examples:

    * Power on/Reset: ASC/ASCQ `0x29/0x00` (Power On, Reset, or Bus Device Reset occurred)
    * Media change: `0x28/0x00` (Not ready to ready change) or `0x3A/0x00` (Medium not present)
    * Mode parameters changed: `0x2A/0x01` or `0x2A/0x09`
* **ILLEGAL REQUEST** `0x20/0x00` (invalid command) for unsupported opcodes; `0x24/0x00` (invalid field in CDB) for bad flags; `0x25/0x00` (LUN not supported) if LUN≠0 and not implemented.
* **DATA PROTECT** for write when write‑protected (ASC/ASCQ `0x27/0x00`).
* **MEDIUM ERROR/HARDWARE ERROR** for injected faults.

### 4.6.3 Per‑LUN Sense Buffer

Maintain a per‑LUN sense structure. **Auto Sense** is not present in SCSI‑1; initiator must issue REQUEST SENSE explicitly (classic Mac drivers do this).

---

## 4.7 Data Transfer Semantics

* **Block length**: default 512; expose via READ CAPACITY and Mode pages. Enforce consistent lengths across READ/WRITE.
* **Residuals**: If host supplies fewer/more data bytes than expected, prefer to set `CHECK CONDITION/ABORTED COMMAND` (`0x0B/0x00`) or short‑transfer with appropriate status if your bus engine supports it. Classic 5380 paths are byte‑exact via REQ/ACK.
* **REQ/ACK**: one byte per handshake; for emulator, generate N handshakes for the exact payload length.

---

## 4.8 Tagged Command Queuing (Optional, SCSI‑2)

If enabled, targets can accept multiple outstanding commands per LUN per initiator. Queue tag messages (Message Out):

* **SIMPLE QUEUE TAG** `0x20, tag`
* **ORDERED QUEUE TAG** `0x21, tag`
* **HEAD OF QUEUE TAG** `0x22, tag`

For a Mac Plus–authentic emulator, you may **disable** tagged queuing and respond with `MESSAGE REJECT` to queue tag messages. If implemented, each active command must carry an **l\_T\_L\_Q nexus** keyed by the (initiator, target, LUN, tag).

---

## 4.9 Worked Transaction Examples

### 4.9.1 Standard READ(6) of 2 blocks @ LBA 0x012345 (block size 512)

1. **COMMAND** (6 bytes): `08 | 0x01 | 0x23 | 0x45 | 0x02 | 0x00`
2. **DATA IN**: 1024 bytes (2×512) over 1024 REQ/ACK handshakes.
3. **STATUS**: `0x00` (GOOD)
4. **MESSAGE IN**: `0x00` (COMMAND COMPLETE)

### 4.9.2 READ CAPACITY(10)

1. **COMMAND**: `25 00 00 00 00 00 00 00 00 00`
2. **DATA IN** (8 bytes): `[last_lba (MSB..LSB)] [block_len (MSB..LSB)]`
3. **STATUS** GOOD → **COMMAND COMPLETE**.

### 4.9.3 Failing WRITE(10) to write‑protected medium

* Emulator detects WP → end with `CHECK CONDITION` and stage Sense: **DATA PROTECT**, ASC/ASCQ `0x27/0x00`.
* Initiator issues REQUEST SENSE; emulator returns fixed sense conveying the error.

---

## 4.10 Emulator Implementation Notes

* **Opcode table**: Build a dispatch by opcode → handler; validate CDB length and fields.
* **LUN decode**: For Group 0, extract `(cdb[1]>>5)&0x7`; for Group 1/5, LUN bits are obsolete—use IDENTIFY when present; most Mac stacks assume LUN 0.
* **Capacity**: Compute `last_lba = (image_size / block_size) - 1` (if zero‑length, report 0 and block size).
* **Sense default**: On power‑on, queue **UNIT ATTENTION** for each LUN until it is reported once to each initiator.
* **Mode pages**: Provide reasonable defaults; MODE SENSE(PC=1) should expose only changeable bits. MODE SELECT must mask and reject illegal bits.
* **Timing**: Although byte‑accurate timing is optional, enforce protocol order: COMMAND → (optional DATA) → STATUS → MESSAGE.
* **Parity**: You may simulate parity as always correct; if parity error injection is enabled, set sense `MISCOMPARE`/`PARITY ERROR` with appropriate ASC/ASCQ.

---

## 4.11 Quick Reference: Direct‑Access Opcodes Used by Classic Mac OS

| Opcode | CDB Len | Name                                      |
| -----: | :-----: | ----------------------------------------- |
| `0x00` |    6    | TEST UNIT READY                           |
| `0x03` |    6    | REQUEST SENSE                             |
| `0x04` |    6    | FORMAT UNIT                               |
| `0x07` |    6    | REASSIGN BLOCKS                           |
| `0x08` |    6    | READ(6)                                   |
| `0x0A` |    6    | WRITE(6)                                  |
| `0x0B` |    6    | SEEK(6) (often ignored by modern devices) |
| `0x12` |    6    | INQUIRY                                   |
| `0x15` |    6    | MODE SELECT(6)                            |
| `0x16` |    6    | RESERVE(6)                                |
| `0x17` |    6    | RELEASE(6)                                |
| `0x1A` |    6    | MODE SENSE(6)                             |
| `0x1B` |    6    | START STOP UNIT                           |
| `0x1E` |    6    | PREVENT/ALLOW MEDIUM REMOVAL              |
| `0x25` |    10   | READ CAPACITY(10)                         |
| `0x28` |    10   | READ(10)                                  |
| `0x2A` |    10   | WRITE(10)                                 |
| `0x2F` |    10   | VERIFY(10)                                |
| `0xA8` |    12   | READ(12) (optional)                       |
| `0xAA` |    12   | WRITE(12) (optional)                      |

---

### 4.12 Minimal Compliance Matrix for Emulator Targets

* Implement **INQUIRY, TEST UNIT READY, REQUEST SENSE, READ/WRITE(6 or 10), READ CAPACITY(10), MODE SENSE(6), MODE SELECT(6)**.
* Implement **START STOP UNIT** (no‑op acceptable) and **PREVENT/ALLOW** (map to image lock).
* Implement **UNIT ATTENTION**, **LUN not supported**, **Illegal Request** paths.
* Optional: **REASSIGN BLOCKS**, **FORMAT UNIT**, **VERIFY(10)**, **Tagged Queuing**.

This chapter gives all structures needed to parse/construct SCSI transactions in the emulator. For full device‑type coverage (tape/CD‑ROM/etc.), replicate the pattern above with their device‑specific CDBs and data formats.
