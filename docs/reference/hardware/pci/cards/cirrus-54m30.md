# Cirrus Logic 54M30 — the Network Server's on-board display controller

**Contents:**

1. [Overview](#1-overview) — what the part is; which die is behind Apple's marking; placement on the
   PCI fabric; the board around it
2. [Register file](#2-register-file) — PCI configuration space; the host I/O register map; the
   extension register file; the linear frame buffer and memory-mapped I/O; reset state and the
   configuration latches
3. [Behaviour](#3-behaviour) — host access paths; the frame buffer and the byte-order contract;
   clocking; the DRAM interface; display generation; the BitBLT engine and the hardware cursor;
   interrupts
4. [Programming model](#4-programming-model) — what Open Firmware does; what AIX does; what Mac OS
   does; a mode-set without a BIOS; monitor sensing and DDC
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **Cirrus Logic 54M30** is the Apple Network Server 500/700's soldered-down display controller.
"The Network Server implements a Cirrus Logic 54M30 video controller, which provides a bit-mapped
1Mbyte DRAM frame buffer" [1] §2.8 p. 9. It is a discrete SVGA controller sitting on the machine's
PCI bus as an ordinary device — the platform's one departure from the Power Macintosh video
architecture of its generation: where the TNT machines hang a Control/Chaos framebuffer subsystem
off a dedicated video bus ([ans.md](../../../machines/ans/ans.md) §2.5, citing the TNT family
architecture), the Network Server has **no video bus at all**, and its on-board video is just
another PCI function on Bandit 1.

Three things make the part unusual for an Apple machine of this era:

- **The connector is industry-standard, not Apple's.** "The external hardware interface is standard
  VGA with DDC-2 monitor sense. However the operating system may or may not interact with DDC. Care
  must be taken to assure the monitor is multi-sync and compatible with the selected resolution and
  refresh rate" [1] §2.8 p. 9. The service manual names the port "SuperVGA (SVGA)" and specifies an
  HDI-15 cable [3] Specifications/I/O p. 5; a Macintosh Display Adapter was included in the box so
  that an Apple display with a DB-15 cable could be connected ([ans500.md](../../../machines/ans/ans500.md) §3.5).
- **The frame buffer is little-endian.** "This controller implements only a little-endian window into
  the packed-pixel frame buffer, hence Big Endian operating systems are limited to 8 bits per pixel
  unless low-level transformation routines are written" [1] §2.8 p. 9 — the single sentence that
  shapes the whole software story around this chip (§3.2).
- **It has no interrupt line.** "Note that the 54M30 video controller has no interrupt line" [1]
  "Network Server External Interrupt Map" p. 16. There is no vertical-blanking interrupt, no
  engine-completion interrupt, no retrace event of any kind delivered to software (§3.7).

Apple is candid that the part was chosen for cost, not speed: "Hardware acceleration and cursors are
available requiring software implementation. Pure bit-mapped mode will undoubtedly be visibly slow
and require significant CPU utilization. Screen savers should be discouraged for maximum system
performance" [1] §2.8 p. 9. The acceleration hardware exists in the silicon (§3.6); the shipped
software simply does not use it.

The "54M30" marking appears in no other Apple product and matches no public Cirrus Logic part
number — the identification argument that pins down which silicon family it belongs to is §1.2.

### 1.2 Which die is behind the marking

The part is a member of the Cirrus Logic **"Alpine" CL-GD543X/4X family** — the CL-GD5430, CL-GD5434,
CL-GD5436 and CL-GD5440 — and the evidence points specifically at the GD5430/GD5440 end of that
family, not the higher-end GD5434. The chain:

1. **The PCI identity is fixed by three independent artifacts.** The production boot ROM's Open
   Firmware names the node `54m30` and sets its `compatible` property to `pci1013,a0` [4]; the
   machine's published device tree shows the node at `54m30@F` [2] Ch. 6 pp. 48–49; and AIX 4.1.5's
   own device database carries the fileset `devices.pci.pci1013+a0` — vendor `$1013` (Cirrus Logic),
   device `$00A0` [6]. Vendor `$1013`, device `$00A0`, on Bandit 1 at IDSEL 15.
2. **The Alpine technical reference manual assigns `$00A0` to the low-end parts.** The PCI
   Device/Vendor ID register "will always return the value 00A0h for the CL-GD5430, 00A8h for the
   CL-GD5434, 00ACh for the CL-GD5436, and 00A0h for the CL-GD5440" [7] §4.14. The vendor ID is
   `1013h` for all [7] §4.14. So the device ID alone excludes the GD5434 and GD5436, and leaves the
   GD5430/GD5440 pair.
3. **The board's shape fits the low end.** The chip carries a 1 MB frame buffer; the CL-GD5430/'40
   support "1/2-, 1-, and 2-Mbyte display memory" while the 4 Mbyte capacity is the '34/'36's
   distinguishing feature [8]. And Apple's advertised maximum — "The buffer will support 1024x768 at
   8 bits" [1] §2.8 p. 9 — is the '30/'40's class of resolution ceiling rather than the '34's.

The remaining ambiguity is real: the device ID, the CR27 ID register (which reads `101000` for *both*
the GD5430 and the GD5440 [7] §9.50) and the register file itself cannot separate the two dies, since
the CL-GD5440 is an upward-compatible CL-GD5430 with an integrated video-processor block [7] Appendix
B10 §1. The discriminators that would settle it — the '5430-only CR28 Class ID register [7] §9.51,
the '5440's video-window registers (CR31–CR3F, [7] §9.52 ff.), the '5440's dedicated video FIFOs —
have not been read back on the ANS board, so the identification remains *inferred* (§6.1). Cirrus's
own errata literature names one-more variant worth knowing about: ordering codes of the form
**CL-GD5430-1MB** appear throughout the applications book [9], confirming that Cirrus sold
1-Mbyte-population variants of the '5430 as a distinct ordering class — as close to a catalog entry
for this board's configuration as the public record gets.

Because "54M30" matches no public Cirrus part number, the likeliest reading is a customer-specific
marking on a stock die (*inferred — unverified*); nothing in the evidence suggests a custom silicon
variant, and the register map that applies is the family-wide one [7]. Two honest caveats travel with
that: the TRM's own reset-value bit table marks Device ID bits [7:0] as part-dependent ("X") while its
prose fixes the per-part values above [7] §4.14, and no public document enumerates Apple's marking
convention. Neither affects the register-level contract.

### 1.3 Placement on the PCI fabric

The 54M30 sits on **Bandit 1** — the bridge Apple's board diagram labels "Bus 0", based at
`$F2000000` in the machine's I/O map ([ans.md](../../../machines/ans/ans.md) §3.2) — at **IDSEL 15**:
"Fast/Wide SCSI buses (Symbios Logic 53C825A) occupy the IDSEL positions 17 and 18. Slots 1 and 2 in
the Network Server remain at IDSEL 13 and 14. The Cirrus Logic 54M30 Video Controller occupies IDSEL
15" [1] §4.6.2 p. 16. Note the decimal/hex trap the whole machine is prone to: IDSEL **15 decimal**
is unit address **`@F`**, because the Open Firmware unit address is the IDSEL in hex
([ans.md](../../../machines/ans/ans.md) §3.3).

Configuration cycles reach it through Bandit 1's configuration address and data ports, exactly as for
any other device on that bus ([bandit.md](../../../machines/tnt/bandit.md) §2.2, §3.1). It is one of
**six** devices on Bandit 1 — the heaviest population any Bandit-1 bus in the platform family sees,
with no PCI-to-PCI bridge anywhere in the machine ([ans.md](../../../machines/ans/ans.md) §3.3).

Bar assignment order matters for reproducing the machine's device tree: "Open Firmware maps devices
for their requested spaces in discovery order. Order of discovery is in slot order; on board
input-output devices are configured prior to slots" [1] §4.4 pp. 12–13. The 54M30, Grand Central and
the two SCSI controllers therefore receive their BAR assignments *before* any expansion card, and an
enumeration that walks IDSELs in a different order produces a tree that does not match the published
one ([ans.md](../../../machines/ans/ans.md) §4.2).

The bus environment has two declared caveats that touch this chip directly: the Network Server
"attempts to be PCI 2.1 compliant. Open issues surrounding this compliance are: Special Cycle
support and discontiguous byte enables" [1] §7.1 p. 22 — the discontiguous-byte-enables caveat
interacts with a documented Alpine silicon erratum (§5), which is why it is worth naming here.

### 1.4 The board around it

| Attribute | Value | Source |
|---|---|---|
| Frame buffer | 1 MB DRAM, bit-mapped | [1] §2.8 p. 9 |
| Memory interface | 32-bit data bus at 1 MB (SRF[4:3] = `10`), extended-RAS timing | [7] §9.6, B9 §3; §3.4 below |
| Clocking | "Extended RAS is configured with adequate DRAM speed to accomplish 68 MHz clocking" | [1] §2.8 p. 9; §3.3 |
| Connector | SVGA, HDI-15, DDC-2 monitor sense | [1] §2.8 p. 9; [3] Specifications/I/O p. 5 |
| Modes | 640×480 at 60/70/72 Hz, 800×600 at 60/72/75 Hz, 1024×768 at 60/72/75 Hz, 256 colors | [3] Specifications/Sound and Video pp. 6–7 |
| Monitor reach | 14-, 15-, 17- and 20-inch multisync displays | [3] Basics/Overview p. 3 |
| Interrupt | none | [1] "Network Server External Interrupt Map" p. 16 |
| Package | 208-pin PQFP or HQFP (family-wide; the ANS's specific package is unobserved) | [8] |

The published mode list is the *service manual's* promise for the machine as shipped, all at 256
colors (8 bpp) [3] — the depth Apple's byte-order rule steers big-endian operating systems into
(§3.2). "However many monitor modes are supported" beyond that ceiling [1] §2.8 p. 9; the chip's
silicon repertoire runs to 1280×1024 and to 16-bit and 24-bit direct-color modes [8], none of which
the shipped software uses on this machine.

---

## 2. Register file

### 2.1 PCI configuration space

The chip implements a small PCI configuration header, "accessible and effective only if
CL-GD543X/4X is configured for PCI bus" [7] §4.14 — which the board's system-bus straps select
(MD[50:48] = `100` for PCI, [7] Appendix B9 Table B9-4). The registers, with their reset values:

| Offset | Register | Reset / fixed value | Source |
|---|---|---|---|
| $00 | Device/Vendor ID | `$00A0` `$1013` (RO) — `00A0h` for the CL-GD5430 and CL-GD5440 | [7] §4.14 |
| $04 (write) | Command | bit 0 = enable I/O accesses, bit 1 = enable memory accesses, bit 5 = enable DAC shadowing; bits 15:6, 4:2 reserved, must be 0 | [7] §4.15 |
| $04 (read) | Status | bits 26:25 = `00` (fast DEVSEL# timing, RO) | [7] §4.16 |
| $08 | Class code / revision | class `030000` (VGA-compatible controller, RO); revision part-dependent (RO) | [7] §4.17 |
| $10 | Display memory base address | bits 31:24 writable (16 MB block); bit 3 (prefetchable) reads `1` on the CL-GD5430/'40; bit 0 = 0 (memory) | [7] §4.18 |
| $14 | Relocatable I/O base address | bits 15:10 writable (512-byte I/O block); bit 0 reads CF3 (MD51) | [7] §4.19 |
| $30 | Expansion ROM base address | bits 31:24 writable (16 MB block); bit 0 = EROM enable | [7] §4.20 |
| $3C (low byte) | Interrupt line | R/W, "no direct effect on the CL-GD543X/4X chip" | [7] §4.21 |
| $3C (high byte) | Interrupt pin | `00` normally; `01` only if MD62 is pulled down (CL-GD5436/'40 only) | [7] §4.22, B9 §3 |

Several of these deserve prose beyond the table:

- **The memory BAR requests a 16 MB block for at most 2 MB of memory.** Bits [31:24] are the
  programmable base; "The display memory for the CL-GD5434 will be in the low 4 Mbytes of the range
  selected. The CL-GD5430 will respond to accesses in the low 2 Mbytes of the range selected" [7]
  §4.18. Open Firmware must therefore hand the 54M30 a 16 MB window inside Bandit 1's memory space,
  of which the bottom 1–2 MB is the frame buffer. The prefetchable bit "is always read as a '1'" on
  the '30/'40 [7] §4.18.
- **The I/O BAR is a 512-byte block** selected by bits [15:10], "combined with the linear address
  offset, provides hardware hooks for multiple VGA controllers in a single PCI system" [7] §4.19;
  whether relocation is enabled at all reads back the MD51 strap (CF3). On this host, I/O cycles
  carry the address Bandit forwards ([bandit.md](../../../machines/tnt/bandit.md) §3.4), so the chip's
  VGA register block appears in the machine's I/O space at whatever base firmware assigns — the
  register *indices* are unchanged (§2.2). The ANS board's MD51 strap state is unobserved (§6.3).
- **The expansion ROM BAR is a PC mechanism with no consumer here.** The EROM-enable bit swaps the
  decode: "When this bit is programmed to a '1'. the VGA BIOS at C000:0 is enabled and display
  memory is disabled (CAS0* is forced high)" [7] §4.20. No x86 BIOS ever executes on this machine, so
  this bit should never be set; the ROM BAR's size is nevertheless read by firmware ([4], §4.1) and
  what image, if any, is populated behind it is unknown (§6.5).
- **The interrupt pin register encodes the strap, not the wiring.** It reads `01` — "an indication
  that the CL-GD543X/4X Interrupt Request pin is connected to the INTA# pin" — only when MD62 is
  pulled down; otherwise `00` [7] §4.22. For the CL-GD5430 the MD62 latch is "reserved" outright [7]
  B9 Table B9-1 — the '5430 has no PCI interrupt request at all, which is the cleanest silicon-level
  reading of Apple's "no interrupt line" statement (§3.7).
- **The command register is one of three affected by a write-width erratum** on early '5430-1MB
  silicon: a byte or word write to $04, $10 or $30 clears the unwritten bytes to zero, so all three
  must be written as DWORDs [9] (§5).

### 2.2 The host I/O register map

The chip's functional registers live behind the classic VGA indexed-I/O ports. In PCI configuration
they are reached through the I/O base assigned to BAR $14 (§2.1); the port offsets below are the
offsets *within* that 512-byte block:

| Block | Index port | Data port | Registers |
|---|---|---|---|
| Sequencer | $3C4 (SRX) | $3C5 | SR0–SR4 VGA; SR2, SR6–SR1F extensions (§2.3) |
| CRT controller | $3B4 / $3D4 (CRX) | $3B5 / $3D5 | CR0–CR18 VGA; CR19, CR1A, CR1B, CR1D, CR25, CR27, CR28 extensions; `$3B` in monochrome emulation, `$3D` in color [7] Ch. 6 |
| Graphics controller | $3CE (GRX) | $3CF | GR0–GR8 VGA; GR0/GR1 dual role, GR9–GR33 extensions (§2.3) |
| Attribute controller | $3C0/$3C1 (ARX) | — | AR0–ARF palette, AR10–AR14 mode/overscan/pan/color-select |
| Miscellaneous | — | $3C2 write (MISC), read (FEAT) | clock select bits 3:2, I/O address select |
| Feature/status | — | $3BA/$3DA | FC write; FEAT/STAT status reads (retrace, sense bits) |
| Palette DAC | — | $3C6–$3C9 | pixel mask, pixel address read/write, pixel data; the hidden DAC register (HDR) at $3C6 [7] Ch. 9 roster |

Everything in the first five rows is industry-standard VGA; a re-implementation does not need this
page for those, only for the Alpine extensions and for how this machine actually drives the whole.
The `$3B`/`$3D` split is the VGA monochrome/color emulation pair: on this machine the part runs in
color emulation, so the CRTC lives at index port `$3D4`, data `$3D5` (*inferred*: color emulation is
the only configuration the shipped software path selects; the MISC register's I/O-address-select bit
chooses, [7] §4.5).

### 2.3 The extension register file

The Alpine family's extensions are reached through the same four indexed ports. The full roster is
[7] Chapter 9; the registers that matter for this machine, by block:

**Sequencer extensions** (via $3C4/$3C5):

| Register | Index | Function | Detail |
|---|---|---|---|
| SR2 | $02 | Enable writing pixel extension | write-mode 4/5 color expansion control |
| SR6 | $06 | Unlock ALL extensions | write `xxx1x010` → reads `$12` (unlocked); any other value → reads `$0F` (locked); **always unlocked on the CL-GD5430/'40** [7] §9.1, B5 note 4 |
| SR7 | $07 | Extended sequencer mode | bits 7:4 memory-segment select / linear addressing (§2.4); bits 3:1 pixel-clock control (§3.5); bit 0 high-resolution packed-pixel modes (§3.2) [7] §9.2 |
| SR8 | $08 | EEPROM control / DDC2B control | bit 6 selects the DDC2B meaning; bits 7/2 readbacks, bits 1/0 open-collector outputs (§4.5) [7] §9.3 |
| SR9, SRA | $09/$0A | Scratch-pad 0, 1 | "reserved for the exclusive use of the CL-GD543X/4X BIOS" — no BIOS runs here [7] §9.4 |
| SRB–SRE | $0B–$0E | VCLK0–VCLK3 numerator | 7-bit numerators; reset values in §2.5 [7] §9.5 |
| SRF | $0F | DRAM control | bus width, write buffer, FIFO depth, RAS-timing readback (§3.4) [7] §9.6 |
| SR10–SR13 | $10–$13 | Graphics cursor X, Y, attributes, pattern offset | 11-bit positions; 32×32/64×64 select; pattern selection (§3.6) [7] §9.7–§9.10 |
| SR14, SR15 | $14/$15 | Scratch-pad 2, 3 | BIOS-reserved [7] §9.11 |
| SR16 | $16 | Performance tuning | host-bus and FIFO tuning [7] §9.12 |
| SR17 | $17 | Configuration readback and extended control | bits 5:3 read back CF[2:0] (system bus select); bit 2 enables memory-mapped I/O; bit 6 selects its location; bit 1 DDL DRAM timing [7] §9.13 |
| SR18–SR1A | $18–$1A | Signature generator control/result | board-level test feature [7] §9.14–§9.16 |
| SR1B–SR1E | $1B–$1E | VCLK0–VCLK3 denominator/post-scalar | 5-bit denominator (6-bit on the '34, SR1B only), post-scalar bit 0 [7] §9.17 |
| SR1F | $1F | MCLK select | MCLK ≈ SR1F[5:0] × reference/8; bit 6 uses MCLK as VCLK (§3.3) [7] §9.18 |

**Graphics-controller extensions** (via $3CE/$3CF): GR0/GR1 double as the write-mode 4/5 background
and foreground color extensions; GR9/GR9A are the Offset 0/1 registers that implement the banked
memory map (4 KB granularity within 1 MB, 16 KB within 4 MB); GRB extends the graphics mode (offset-1
enable via SA15, 16 KB granularity via GRB[5], 2 MB linear reach — "This bit must always be set to
'0' for 1 Mbyte of linear addressing" [7] §9.21); GRC/GRD are color-key compare and mask; GRE is the
power-management register; GR10/GR11 are background/foreground color byte 1 for color expansion.
The **BitBLT register set** — GR20–GR23 width and height, GR24/GR25 and GR26/GR27 destination and
source pitch, GR28–GR2A and GR2C–GR2E destination and source start (three bytes each), GR2F
destination write mask (CL-GD5430 only), GR30 BLT mode, GR31 BLT start/status, GR32 raster operation,
GR33 mode extensions — is detailed in §3.6 [7] §9.28–§9.43.

**Extended CRTC registers** (via $3B5/$3D5): CR19 interlace end; CR1A miscellaneous control —
including the overlay-control field [3:2] that turns the P-bus pins into inputs for monitor-ID
sensing (§4.5), double-buffered display start (bit 1) and interlace enable (bit 0) [7] §9.44; CR1B
extended display controls — extended address wrap (bit 1), offset-register overflow (bit 4), screen
start bits 16–18 (§3.5) [7] §9.45; CR1D overlay extended control; CR25 part status; CR27 the ID
register (`101000` in bits 7:2 for the CL-GD5430 and CL-GD5440 alike) [7] §9.50; and CR28, the
CL-GD5430-only Class ID register: `$FF` standard CL-GD5430, `$01` DDC2 support, `$03` video windowing
plus DDC2 [7] §9.51 — the one register that could settle §6.1.

### 2.4 The linear frame buffer and memory-mapped I/O

With SR7[7:4] cleared, the chip answers as a standard VGA, "respond[ing] to access at Axxx:x and
Bxxx:x" [7] §9.2 — on PCI, within its BAR window at the corresponding offsets of the 16 MB block
(*inferred*: the TRM describes the PCI case only at the whole-block altitude; the legacy memory map
is served inside the assigned block). With **SR7[7:4] nonzero**, the chip switches to linear
addressing:

- "If the high order eight bits of the address on the AD pins matches the PCI Base Address register,
  and SR7[7:4] is a non-zero, the CL-GD543X/4X will be in linear addressing mode. This provides a
  16-Mbyte address space" [7] Appendix D2 §2.
- "The bottom of this space is used for Display Memory access and the top 256 bytes can be used for
  Memory-mapped I/O (for the CL-GD5430/36/40 only)" [7] Appendix D2 §2. The CL-GD5430 responds in the
  low 2 MB of the range [7] §9.2; the CL-GD5440 adds its video address space at the 8 MB mark [7]
  D2 §2.
- For the CL-GD5436 only, the same 16 MB space can be configured as four byte-swapping apertures,
  "for Power PC" [7] Appendix D2 §2 — a built-in endianness workaround the '30/'40 lack, which is why
  Apple's board can expose only the little-endian window (§3.2).

**Memory-mapped I/O** (SR17[2] = 1) exposes the BitBLT control registers as memory locations — "up
to four times faster than using I/O accesses when multiple registers must be changed at once" [7]
Appendix B20 §1. A 256-byte block is reserved: at `B800:0` (aliased at every 256-byte boundary
through `BFF0:0`) when linear addressing is off, or — for the CL-GD5430/'36/'40 with linear
addressing on — at the **last 256 bytes of the linear address space**, selected by SR17[6] [7]
Appendix B20 §1. The layout [7] Appendix B20 Table B20-1:

| Offset | Register | | Offset | Register |
|---|---|---|---|---|
| $00 | GR0 background color byte 0 | | $10–$12 | GR28–GR2A BLT destination start |
| $01 | GR10 background color byte 1 | | $14–$16 | GR2C–GR2E BLT source start |
| $04 | GR1 foreground color byte 0 | | $17 | GR2F destination write mask ('30/'40) |
| $05 | GR11 foreground color byte 1 | | $18 | GR30 BLT mode |
| $08/$09 | GR20/GR21 BLT width | | $1A | GR32 BLT raster operation |
| $0A/$0B | GR22/GR23 BLT height | | $40 | **GR31 BLT start/status — the only readable one** |
| $0C–$0F | GR24–GR27 destination/source pitch | | $41–$FF | reserved |

All of these are write-only through the memory path except GR31 at offset $40; reads of write-only
MMIO registers complete but return indeterminate data [7] Appendix B20 §1. Ordinary indexed I/O
keeps working alongside [7] Appendix B20 §1.

### 2.5 Reset state and the configuration latches

**Configuration latches.** "When RESET is active, the CL-GD543X/4X loads the levels on MD[63:48] in
16 internal latches. These latches control some fundamental properties of the device, such as the
host bus interface and DRAM configuration" [7] Appendix B9 §1. Each MD line has an internal ~250 kΩ
pull-up; a board installs a ~6.8 kΩ pull-down for a `0` [7] B9 §2. The latches, and where software
can read them back:

| MD bit | CF bit | Meaning (CL-GD5430/'40 where they differ) | Readable at |
|---|---|---|---|
| MD63 | CF15 | pin-scan test mode | — |
| MD62 | CF14 | PCI interrupt request control — *'36/'40 only*; no pull-down → pin register `00`, "the INTR pin must not be connected"; reserved on the '30 | PCI$3C[15:8] |
| MD61 | CF13 | reserved ('36: 3C3[0] reset state) | — |
| MD60 | CF12 | MCLK pin source / DAC-related strap | SRF[0] |
| MD59 | CF11 | asymmetric DRAM addressing (row MA[9:0], column MA[8:1]) | — |
| MD58 | CF10 | dual-CAS* (pull-down installed) vs multiple-WE* DRAMs | — |
| MD57 | CF9 | **extended-RAS\* timing** — random cycle 7 MCLK periods vs 6 | SRF[2] |
| MD56 | CF8 | power-on MCLK: 41 MHz class (no pull-down) / 50.1 MHz (pull-down) | SRF[1] |
| MD55 | CF7 | 64 KB ROM BIOS range — "If the CL-GD543X/'4X is configured for the PCI bus, this bit has no effect" | — |
| MD54 | CF6 | VESA zero-wait / '36 reserved — no effect on PCI | — |
| MD53 | CF5 | external MCLK (factory test only) | — |
| MD52 | CF4 | POS102 access — no effect on PCI | — |
| MD51 | CF3 | **I/O relocation enable (PCI, '30/'40)** | PCI$14[0] |
| MD50:48 | CF[2:0] | system bus select — `100` = PCI | SR17[5:3] |

[7] Appendix B9 Table B9-1 and §3. Which of these pull-downs the Network Server board installs is
directly established only for the one Apple names — extended RAS [1] §2.8 p. 9 (§3.3); the rest are
unobserved (§6.3), and every one of them is software-readable through the readback paths in the
table's last column, which is how a future interrogation of the hardware would settle them.

**Clock reset values.** The VCLK synthesizers power up with four stock clocks [7] §9.5:

| Clock | Frequency | N | D | P | Numerator (SRx) | Denominator/post-scalar (SR1x) |
|---|---|---|---|---|---|---|
| VCLK0 | 25.180 MHz | 102 | 29 | 1 | $66 (SRB) | $3B (SR1B) |
| VCLK1 | 28.325 MHz | 91 | 23 | 1 | $5B (SRC) | $2F (SR1C) |
| VCLK2 | 41.165 MHz | 69 | 24 | 0 | $45 (SRD) | $30 (SR1D) |
| VCLK3 | 36.082 MHz | 126 | 25 | 1 | $7E (SRE) | $33 (SR1E) |

The MCLK power-on default is 41 MHz-class with no MD56 pull-down, 50.1 MHz with one [7] B8 §2.2, B9
§3 — and "The MCLK default frequency is immediately overwritten by the Cirrus Logic BIOS at POST
time" [7] B8 §2.2. **No BIOS ever runs on this machine** (§4.4), so on the 54M30 the power-on MCLK
stands until an operating-system driver reprograms SR1F — one of several places where this board's
software must do work a PC BIOS would have done. Which MCLK the ANS driver actually selects is
unobserved (§6.2, §6.9).

The reset values of the VGA-core registers (CRTC, sequencer, graphics and attribute controllers)
are the IBM VGA power-on set, restated register by register in [7] Chapters 5–8; they are not
reproduced here. Extension registers that default to zero and matter to this machine's bring-up:
SR7[7:4] (standard VGA memory map at reset — linear addressing is *off* until software turns it on)
[7] §9.2 and SR17[2] (MMIO off) [7] §9.13.

---

## 3. Behaviour

### 3.1 Host access paths

On this machine the chip is reachable through exactly three surfaces:

1. **Configuration cycles**, generated through Bandit 1's configuration address/data ports
   ([bandit.md](../../../machines/tnt/bandit.md) §2.2–§2.3), targeting IDSEL 15 — used by Open
   Firmware at boot to size and assign the BARs (§4.1) and by anything that reads the identity
   registers.
2. **The I/O base** (BAR $14): the indexed VGA register file of §2.2, in Bandit's I/O cycle space.
3. **The memory base** (BAR $10): the 16 MB block whose bottom holds the frame buffer — banked as
   standard VGA with SR7[7:4] = 0, linear with SR7[7:4] ≠ 0 — and whose last 256 bytes can serve as
   memory-mapped I/O (§2.4).

The PC legacy decodes do not exist in this host's address map: there is no system memory at `A0000`
or `C0000` for the chip to answer to, and no `EMM386`-era machinery to shadow a BIOS through the
EROM window. Its standard-VGA and ROM-page responses only ever apply *inside* its assigned BAR
block (§2.4). The practical consequence for a driver is that the frame buffer is best used through
the linear window — and that is also the mode in which the byte-order contract of §3.2 is exact.

### 3.2 The frame buffer and the byte-order contract

The Alpine family's packed-pixel modes store "consecutive pixels ... at consecutive addresses"
whenever SR7[0] is set — "true packed-pixel addressing", used with 8-, 16-, 24- and 32-bit-per-pixel
modes [7] §9.2. In direct-color modes the bytes within a pixel run **blue at the lowest address,
green next, red highest** — the layout a little-endian host produces naturally [7] Chapter 5 (the
15/16/24-bit pixel descriptions; e.g. "The Blue value is stored in the lowest-addressed byte" for
24-bit true color). The palette-DAC 8-bit mode stores one byte per pixel, so byte order does not
enter at all.

That layout is what Apple's sentence is about, and the platform's byte order is what makes it bite.
The processor bus is big-endian and PCI data is little-endian; the Bandit bridge performs
address-invariant byte swapping between them ([bandit.md](../../../machines/tnt/bandit.md) §3.5). A
big-endian PowerPC storing a 16-bit pixel pair or a 32-bit pixel quad into display memory therefore
delivers its bytes to the frame buffer in the reverse of the order the chip's pixel pipeline expects.
At 8 bits per pixel every byte is a whole pixel and the swap is invisible; at 16/24/32 bits per pixel
the pixels (or their color bytes) come out scrambled unless the driver writes bytes individually or
byte-swaps each store. Hence: "This controller implements only a little-endian window into the
packed-pixel frame buffer, hence Big Endian operating systems are limited to 8 bits per pixel unless
low-level transformation routines are written" [1] §2.8 p. 9.

Two facts sharpen the trap:

- The family's built-in workaround does not exist on this die. The CL-GD5436 can present its 16 MB
  space as **four byte-swapping apertures** — no swap for the first, word swap for the second,
  full-dword swap for the third — explicitly "for Power PC" [7] Appendix D2 §2, B9 Table B9-2. The
  CL-GD5430/'40 have nothing equivalent, so the board exposes one window, little-endian, period. A
  display controller can satisfy a big-endian host and a little-endian bus "by offering multiple
  *apertures*, separate mappings of the same memory in different formats" ([bandit.md](../../../machines/tnt/bandit.md) §3.5, quoting the platform's PCI documentation) — this chip cannot.
- The advertised ceiling is byte order, not memory. 1024 × 768 at 8 bpp occupies 786,432 bytes of the
  1 MB buffer, leaving ~276 KB — cursor pattern storage (which lives in the upper 16 KB, §3.6) and
  alignment slack fit comfortably. The 1 MB could hold a 16-bit 1024×768 or a 24-bit 640×480 mode in
  silicon [7] Chapter 5; a big-endian operating system is simply not permitted the convenience.

For a faithful model of the machine this is the one fact not to transpose: the frame buffer is a
little-endian window, and any consumer that byte-swaps on the host's behalf produces
correct-looking output while diverging from the hardware — and conversely, guest code that *does*
implement the transformation renders correctly only against the true little-endian layout.

### 3.3 Clocking: VCLK, MCLK and the 68 MHz question

The chip carries a **dual-frequency synthesizer**: VCLK generates the monitor timing and pixel
clocks; MCLK drives "the display memory state machine, the BitBLT engine, and the host interface
state machine" [7] Appendix B8 §2.

- **VCLK**: four programmable clocks, each a numerator register (SRB–SRE), a denominator/post-scalar
  register (SR1B–SR1E) and the select bits in MISC $3C2[3:2] [7] B8 §3. The law is
  `VCO = Reference × N/D` (post-scalar 0) or `Reference × N/(D × 2)` (post-scalar 1) [7] B8 §3.2, with
  the reference the board's 14.31818 MHz crystal. Reset values are the stock table of §2.5 [7] §9.5.
- **MCLK**: programmed directly — `MCLK = SR1F[5:0] × Reference/8`, i.e. `MCLK ≈ SR1F[5:0] × 1.8 MHz`
  with a 14.31818 MHz reference [7] §9.18, B8 §2.1. Representative values [7] B8 Table B8-1:

| SR1F[5:0] | MCLK | | SR1F[5:0] | MCLK |
|---|---|---|---|---|
| $1C | 50.1 MHz | | $25 | 66.2 MHz |
| $1D | 51.9 MHz | | **$26** | **68.0 MHz** |
| $1E | 53.7 MHz | | $27 | 69.8 MHz |
| $1F | 55.5 MHz | | $28 | 71.6 MHz |
| $22 | 60.9 MHz | | $2D | 80.5 MHz |

  Bit 6 of SR1F additionally allows MCLK itself (or MCLK ÷ 2) to serve as VCLK, shutting down the VCLK
  oscillator to kill jitter between close frequencies [7] §9.18, B8 §4.

**Apple's one clocking statement** — "Extended RAS is configured with adequate DRAM speed to
accomplish 68 MHz clocking" [1] §2.8 p. 9 — reads against this table almost exactly: extended-RAS
timing is the MD57/CF9 strap (§2.5), and SR1F = $26 programs MCLK = 68.0 MHz. Extended RAS is
precisely the DRAM-timing measure that raises the ceiling on how fast the memory clock can run: with
the strap pulled down "A Random cycle will require seven MCLK periods" instead of six [7] B9 §3, and
Cirrus's own DRAM evaluation shows extended RAS lifting the calculated maximum MCLK for a given
device — e.g. 48.3 → 55.5 MHz for a −70-speed 256K×16 dual-CAS part, with RAS precharge the
standard-timing limit and RAS-hold the extended-timing one [9] (Application Alert 3). The sentence
therefore most plausibly means: the board straps extended-RAS timing and fits DRAM fast enough to
run MCLK at 68 MHz (*inferred*).

One tension is honestly noted rather than papered over: the fourth-edition manual caps the '5430's
SR1F at `$22` (60 MHz, "subject to change with silicon characterization") [7] Appendix B5 note 7, and
the family datasheet headlines "Memory clock programmable to 60 MHz (CL-GD5430/34/40)" [8] — while
the same manual's MCLK table continues to $2D and its manufacturing-test appendix offers a
"Variable MCLK test @ 37 MHz through 68 MHz" [7] B18. If Apple's 68 MHz is instead a pixel clock —
well inside the '30/'40's 86 MHz VCLK rating [8] — the strap sentence is then about the memory speed
needed to keep a 68 MHz pixel stream fed. Which reading is right is unresolved (§6.2); nothing else
in the shipped software contract depends on it.

### 3.4 The DRAM interface

A 1 MB frame buffer on this family is a **32-bit memory interface**: SRF[4:3] = `10` selects a
32-bit bus, the size row for 1 MB on the CL-GD5430/'40 [7] §9.6. Cirrus documents three ways to
populate 1 MB [7] Appendix B7 Table B7-2:

| Configuration | Devices | Total | Connection table |
|---|---|---|---|
| 256K × 4 | 8 | 1 MB | B7-4 |
| 256K × 16, dual-CAS*, symmetric | 2 | 1 MB | B7-7 |
| 256K × 16, dual-WE*, asymmetric or symmetric | 2 | 1 MB | B7-11 / B7-14 |

Which of these the Network Server uses — chip organization, CAS/WE steering (the MD58/CF10 and
MD59/CF11 straps), vendor and speed grade — is unobserved (§6.4); Apple's extended-RAS sentence
constrains the speed grade to the fast end of whatever was fitted ([9] Application Alert 3).

Two '5430-specific rules govern the memory interface regardless of population:

- **Bank switching is always enabled on the CL-GD5430**: "RAS1* connects to the first Mbyte, and
  RAS0* connects to the second Mbyte of display memory" [7] Appendix B5 note 5 — a 1 MB board is a
  single bank, resident entirely on RAS1*, and the bank-switch control bit SRF[7] exists only on the
  '34/'36 [7] §9.6.
- The SRF readback bits report the straps: SRF[2] = `0` is extended RAS (RAS high 3 MCLK, low 4 MCLK
  per cycle), `1` standard (2.5/3.5) [7] §9.6; SRF[1] reports MD56 and SRF[0] reports CF12 [7] §9.6,
  B9.

Display-memory refresh is sequencer-managed: clearing SR0[1] halts the sequencer and "disables
screen refresh and display memory refresh" [7] §5.2. The datasheet lists low-frequency DRAM refresh
among the CL-GD5430/'36/'40 features [8]; the SR17[7] refresh-disable bit, by contrast, is the one
extended control the '5430 does not support [7] Appendix B5 note 8.

### 3.5 Display generation

The display pipeline is standard VGA extended just far enough for the packed modes:

- **Timing** comes from the CRTC registers CR0–CR18, with the extensions carrying the wide fields:
  CR1B[1] ("Enable Extended Address Wrap") widens the character counter to 19 bits — "Character
  Counter Addresses CA[16] and CA[18] provide up to 256K bytes in each bit plane, or 1 Mbyte of
  packed-pixel memory" [7] §9.45 — and CR1B[3:2]/[0] supply screen-start address bits 17–18 and 16
  [7] §9.45. CR13 is the offset register (logical scanline width in units per mode), extendable by
  CR1B[4] [7] §9.45.
- **The pixel clock divider** SR7[3:1] selects how the CRTC and video shift registers are clocked:
  `000` normal, `001` ÷2 byte-serial for 16-bit pixels (offset $A0 for 640×480, $C8 for 800×600),
  `010` ÷3 for 24-bit (mode 71h), `011` 16-bit pixels at pixel rate, `100` 32-bit at pixel rate
  ('34/'36 only — "not supported on the CL-GD5430/'40") [7] §9.2. For the 8-bit packed modes this
  machine actually uses, SR7[0] plus `000` is the whole story.
- **Double-buffered display start**: CR1A[1] makes the display start address update on the VSYNC
  following a write to start-address low, "provid[ing] control of display frame switching without
  the need to explicitly monitor VSYNC" [7] §9.44 — the feature a driver leans on when there is no
  interrupt to tell it retrace happened (§3.7).
- **The palette DAC** is integrated, 24-bit, with a 6-bit-per-channel VGA lookup table extended by
  the hidden DAC register at $3C6 [7] Ch. 9 roster; two extra LUT entries (indices $00 and $0F,
  reached through SR12[1]) hold the hardware cursor's two colors, independent of the display palette
  [7] Appendix D3 §2. The DAC full-scale current is set by RSet; on the production revisions with an
  integrated current reference the ideal value is 135 Ω for standard VGA levels [7] B8 §5.

Since there is no retrace interrupt, software synchronizes to the raster by polling the status
register's vertical-retrace bit — the classic VGA register at $3BA/$3DA ([7] §4.8) — or by using
CR1A[1] and never synchronizing at all.

### 3.6 The BitBLT engine and the hardware cursor

The acceleration hardware Apple's "requiring software implementation" sentence refers to [1] §2.8
p. 9:

**The BitBLT engine.** A 32-bit BLT engine ("32-bit BitBLT engine (CL-GD5430)" [8]) parameterized
through GR20–GR33 (§2.3) or their memory-mapped aliases (§2.4). The geometry is set first — width,
height, destination and source pitch, and the three-byte destination and source start addresses —
then the mode and raster operation, then the engine is started by setting GR31[1]; GR31[0] reads busy
and GR31[3] reports progress; GR31[2] resets and abandons an operation [7] §9.40. GR30 carries the
direction bit (decrementing copies proceed right-to-left, bottom-to-top, "intended for screen-to-
screen BitBLTs only"), the color-expansion and pattern-copy enables, and the system-memory source
bit: with GR30[2] set, "the BLT source will be system memory rather than display memory. The CPU will
perform the system bus transfers ... The CPU must use DWORD transfers" [7] §9.39. Two operational
cautions come from the manual itself: start a BLT only with the CPU write buffer drained (SRF[6]
fast-page detection interacts with font loads, and "If the write buffer is not empty when a BitBLT
is started, the BitBLT will not take place properly" [7] §9.40), and never read back a
memory-mapped-written GR0, GR1, GR10 or GR11 — those reads do not work [9] (production-B errata
list; see §5).

**The hardware cursor.** A 32 × 32 or 64 × 64 pixel cursor in two planes — transparent, inverted
display data, or one of two fixed colors supplied by the palette DAC's extra LUT entries [7]
Appendix D3 §1–2. The pattern bitmap lives "in the upper 16K of display memory (upper 4K of each
logical memory plane)", holding 64 patterns of 32 × 32 or 16 of 64 × 64, selected by the pattern
address offset SR13; the X/Y position is an 11-bit value written through the index/data pair with
three position bits riding the index register's bits 7:5 [7] Appendix D3 §2, Table D3-2. The
attributes register SR12 enables the cursor, selects its size, and gates access to the cursor colors
[7] §9.9. A hardware cursor exists precisely so a GUI need not save/restore screen data under the
pointer — on this machine it was never used, so AIX's pointer (if any) is drawn by software into the
frame buffer like everything else (*inferred from the bit-mapped console path*, [1] §2.8).

### 3.7 Interrupts

**The part contributes nothing to the machine's interrupt map.** "Note that the 54M30 video
controller has no interrupt line" [1] "Network Server External Interrupt Map" p. 16; it has no EXT
line on Grand Central and no entry in the interrupt remap ([ans.md](../../../machines/ans/ans.md)
§5.1).

At the silicon level the statement decomposes cleanly:

- On the **CL-GD5430**, PCI interrupt request does not exist — MD62/CF14 is "reserved" in the
  configuration table for the '30 [7] B9 Table B9-1, so there is no strap that could even enable an
  INTA# request.
- On the **CL-GD5440** (and '36), the interrupt pin exists but is strap-gated: without an MD62
  pull-down "the PCI interrupt will not be requested and the INTR pin must not be connected", and
  the pin register reads `00`; with the pull-down the chip requests on INTA# and the register reads
  `01` [7] §4.22, B9 §3.

Either way, a board that wires nothing matches Apple's sentence; whether the die is a '30 (no
interrupt capability at all) or a '40 with the strap left unpulled is exactly the §6.1/§6.10 open
question.

The software consequences are structural, not incidental:

- **No vertical-blanking interrupt.** Page-flipping software polls the status register's retrace
  bit or uses the double-buffered display start of CR1A[1] (§3.5) — and "frame switching without the
  need to explicitly monitor VSYNC" [7] §9.44 reads like the feature this board exists to exercise.
- **No BLT-completion interrupt.** A driver using the engine polls GR31[0] for completion [7]
  §9.40.
- The machine's own vertical-blank machinery belongs to the *slot* cards, not to this device; an
  expansion video card in slots 1–6 interrupts through its own EXT line normally
  ([ans.md](../../../machines/ans/ans.md) §5.1).

---

## 4. Programming model

### 4.1 What Open Firmware does

The production ROM's Open Firmware (version 1.1.22) configures the device with a short FCode word,
`54m30-config` [4]:

```forth
: 54m30-config
    10 get-reg-size to mem-space-size
    14 get-reg-size to io-space-size
    30 get-reg-size to rom-space-size
    " 54m30"       device-name
    " pci1013,a0"  encode-string  " compatible" property
```

The word sizes the three BARs (memory at $10, I/O at $14, expansion ROM at $30), names the node
`54m30`, and publishes `compatible = "pci1013,a0"`. The node lands at `54m30@F` under
`/bandit@F2000000` — Apple's published tree nests it one level astray, but the ROM's own `screen`
alias, which points at `/bandit/54m30@F`, proves the correct placement ([ans.md](../../../machines/ans/ans.md) §4.2, correcting [2] Ch. 6 Listing 6-1's transcription defects).

What the firmware does *not* do is as load-bearing as what it does: the config word reads PCI
configuration registers and nothing else — no I/O register, no frame-buffer byte is touched by the
observed Open Firmware source [4]. Every mode-set, palette load and clock program belongs to the
operating system's driver. Whether the firmware ever paints a console screen on the VGA port before
AIX loads — POST reports on the front-panel LCD, not the display ([ans.md](../../../machines/ans/ans.md) §2.7) — is open (§6.7): the `screen` alias exists, and nothing observed exercises it.

### 4.2 What AIX does

AIX is the only shipping operating system that drives the part. The binding is the ODM contract: the
device's fileset is `devices.pci.pci1013+a0`; its `PdDv` record carries `devid = "pci1013,a0"`,
`type = "cirrus"`, `DvDr = "cirrusdd"`, `Configure = /etc/methods/cfgcirrus`, and the description
"Cirrus Graphics Adapter Software" [6]. The node *name* — `54m30` — must match an ODM entry or the
device is silently left unconfigured, the guarantee Apple states as "the name must be stored in the
Predefined Devices (PdDv) database of the ODM" [2] Ch. 6 ([ans.md](../../../machines/ans/ans.md) §4.2).

Two platform-level facts shape what the driver can assume:

- **AIX consumes the firmware's BAR assignment rather than programming BARs itself.** The Grand
  Central configuration method's embedded routine `resolve_pci_mem_space` converts Open Firmware's
  `assigned-addresses` into kernel mappings, and the `busresolve` service is not supported [6]
  ([ans.md](../../../machines/ans/ans.md) §4.2). Whatever Open Firmware handed the 54M30 at
  discovery time is what the driver lives with.
- **The driver drives it as a bit-mapped console at 8 bpp.** Apple's own performance characterization
  — "Pure bit-mapped mode will undoubtedly be visibly slow and require significant CPU utilization.
  Screen savers should be discouraged for maximum system performance" [1] §2.8 p. 9 — describes the
  shipped configuration: the acceleration registers and hardware cursor sit unused, and the 68 MHz
  memory clock's bandwidth is spent on CPU-driven pixel copies. AIX's LCD startup sequence names the
  graphics configuration stage among its steps ([ans500.md](../../../machines/ans/ans500.md) §5.4),
  but no register-level trace of `cirrusdd` exists in the evidence set — what VCLK/MCLK values,
  which offset registers, and whether it uses the linear window at all are unobserved (§6.9).

### 4.3 What Mac OS does

Nothing, in the shipping configuration. The production ROM contains no Mac OS driver for the part;
a Mac OS boot on this hardware goes through a PCI video card in a slot, and the 54M30 is simply an
unclaimed device in the tree ([ans.md](../../../machines/ans/ans.md) §2.5). Apple did build Mac OS
support and withhold it: the unreleased prototype ROM ("2.0", image dated 1998-01-14) carries a
native driver named `.Display_Video_Cirrus_54M30` [5]. Nothing in the evidence set records what
that driver does with the chip — dumb frame buffer or acceleration, sense or ignore DDC — only that
it exists (§6.8).

An operating system without a Cirrus driver must fall back to the serial console or a supported
slot card; the part gives no VGA-compatible text mode for free, because nothing executes the VGA
BIOS that would set one.

### 4.4 A mode-set without a BIOS

On a PC, the Cirrus BIOS owns mode state: it "unlocks the registers at POST" [7] §9.1, programs MCLK
at POST [7] B8 §2.2, and services the INT 10 mode calls. None of that machinery runs here — no x86
BIOS, no INT 10, no POST shadowing through the EROM window (§2.1). Every driver for this part is
therefore a bare-metal driver. The mode-set sequence the manual's own programming examples
assemble ([7] Appendix D1; the register semantics of §2.3), for one of the machine's published
1024×768-class modes [3]:

1. **Program the pixel clock.** VCLK numerator and denominator/post-scalar (SRB–SRE with
   SR1B–SR1E), then select the clock with MISC $3C2[3:2] [7] B8 §3. (The MCLK-as-VCLK option of
   SR1F[6] exists for close-frequency jitter [7] B8 §4.)
2. **Set the memory clock** if the DRAM timing calls for it: SR1F, chosen against the DRAM's speed
   grade with extended RAS in force ([9] Application Alert 3; §3.3).
3. **Lay out the CRTC**: CR0–CR18 with the mode geometry; CR13 offset (with CR1B[4] overflow);
   CR1B[3:2]/[0] and the start-address registers for the display start; CR1A[1] if page-flipping
   [7] §9.44–§9.45.
4. **Select the pixel organization**: SR7[0] for packed-pixel (8/16/24/32 bpp) with SR7[3:1] = `000`
   for 8-bit; SR7[7:4] nonzero for the linear window — the natural choice here (§2.4, §3.1) — with
   GRB[0] left `0` for 1 MB linear addressing [7] §9.21.
5. **Confirm the DRAM interface**: SRF[4:3] = `10` for the 32-bit/1 MB configuration; the RAS-timing
   readback SRF[2] should read `0` (extended) on this board [7] §9.6.
6. **Load the palette** through $3C8/$3C9 (the 6-bit VGA path suffices for 256-color modes; the
   hidden DAC register extends precision when wanted [7] Ch. 9 roster).
7. **Unlock and go**: on the '30/'40 the extension registers are always unlocked anyway — SR6
   exists to be written for family convention, and reads `$12`/`$0F` back per its last write [7]
   §9.1.

For a hardware cursor or engine use, the remaining steps would be cursor pattern upload to the upper
16 KB and SR12–SR13 programming [7] Appendix D3, or the BLT parameter sequence of §3.6 — steps no
shipped software on this machine performs.

### 4.5 Monitor sensing and DDC

Apple wires the connector for DDC and then disclaims the software: "standard VGA with DDC-2 monitor
sense. However the operating system may or may not interact with DDC" [1] §2.8 p. 9. The chip
supports both DDC levels, with a clear hardware split [7] Appendix B16:

- **DDC1** (monitor talks, host listens) is the sense-only scheme: the monitor ID pins of the VGA
  connector are brought to P-bus pins, the overlay-control field CR1A[3:2] turns those pins into
  inputs, and their levels are read through the status register; only MID1 (VGA pin 12) is the
  DDC-recommended connection, with Cirrus explicitly retracting its earlier advice to wire pin 15 —
  it "can cause monitors with DDC2B support to incorrectly enter DDC2B mode" [7] B16 §2.
- **DDC2B** (bidirectional, open-collector I²C-style) reuses the EEPROM pins: with SR8[6] set, the
  register's bits change meaning — EEDI becomes the SDA-style data line read back at SR8[7] and
  driven through SR8[1], EECS the clock read back at SR8[2] and driven through SR8[0] — and the
  recommended wiring lands them on MID1/MID3, i.e. VGA pins 12/15 [7] §9.3, B16 §3–4. Chip-revision
  support for the integrated version: CL-GD5434 production E, CL-GD5436 production A, CL-GD5440
  production A [7] §9.3; the CL-GD5430 reaches DDC2 only in the Class ID variants CR28 = `$01` or
  `$03` [7] §9.51.

Which scheme the Network Server board wires — DDC1 sense-only, DDC2B through the chip, or both — and
whether AIX's `cirrusdd` reads EDID at all, are unobserved (§6.6). The Apple sentence's second half
("Care must be taken to assure the monitor is multi-sync", [1] §2.8 p. 9) reads as the practical
consequence: if the operating system does not interrogate the monitor, the mode list is chosen blind
against the service manual's table [3].

---

## 5. Quirks & errata

- **The little-endian window is the contract.** A big-endian operating system gets 8 bpp and nothing
  more without software byte transformation [1] §2.8 p. 9; the chip offers none of the byte-swapping
  apertures its CL-GD5436 sibling carries (§3.2). Model the window little-endian, and do not
  "helpfully" swap on the host's behalf.
- **The name matches nothing in the catalog.** "54M30" appears in no public Cirrus document; the part
  identifies itself as vendor `$1013`, device `$00A0` — a value the CL-GD5430 and CL-GD5440 *share*
  [7] §4.14 — so even the PCI ID cannot separate the two candidate dies. CR27 cannot either [7]
  §9.50; CR28 can, and has not been read on this board (§6.1).
- **IDSEL 15 is `@F`.** Apple's document gives IDSEL positions in decimal [1] §4.6.2 p. 16; the
  device tree's unit address is the same number in hex. Tools that compute config-cycle addresses
  from the unit address must multiply, not copy ([ans.md](../../../machines/ans/ans.md) §3.3).
- **The memory BAR asks for 16 MB to serve at most 2 MB.** Bits [31:24] only are programmable; the
  '5430 answers in the low 2 MB of the block [7] §4.18. Firmware that tries to assign a smaller,
  finer-grained window cannot express it.
- **Byte and word writes to PCI configuration registers clear unwritten bytes.** On the affected
  silicon — CL-GD5430-1MB-Q-X and CL-GD5434-HC-B — "If a PCI Configuration Register is written with a
  BYTE or WORD I/O cycle, the portion of the register which is not written is cleared to zeroes",
  for registers $04, $10 and $30; fixed in the -Q-Y/'C steps [9]. DWORD-write them all.
- **Non-contiguous byte-enable writes hang the chip.** On the same silicon generation, a write with
  non-contiguous byte enables (BE1+BE3 without BE2, the product of chipset byte-merging) "will fail
  to return TRDY, hanging the system" [9]. The platform's own compliance statement names
  "discontiguous byte enables" as an open PCI 2.1 issue on this machine [1] §7.1 p. 22 — a bridge
  model that merges bytes into this target can wedge it, and a target model that accepts such writes
  diverges from the silicon.
- **The extension registers are always unlocked on the '30/'40.** The classic Cirrus unlock write
  (SR6 = `$12`) is a no-op convention here, but SR6 still reads `$12` after that write and `$0F`
  after any other [7] §9.1, B5 note 4 — code that *verifies* the unlock by readback works unchanged.
- **SR9/SRA/SR14/SR15 belong to the BIOS.** Cirrus reserves the scratch-pad registers for its BIOS
  [7] §9.4, §9.11 — which never runs here. They read `$00` at reset and are free storage, but any
  software written against a PC's post-BIOS register state will find different contents.
- **No BIOS means the power-on clocks stand.** The MCLK default (41/50.1 MHz per the MD56 strap) is
  "immediately overwritten by the Cirrus Logic BIOS at POST time" on a PC [7] B8 §2.2 — nothing
  performs that overwrite on this machine, so the first mode-set must program SR1F itself (§4.4).
- **The EROM enable trades the frame buffer away.** Setting PCI$30 bit 0 "enables" a BIOS at `C000:0`
  and forces CAS0* high, disabling display memory [7] §4.20 — a PC shadowing mechanism that, if ever
  set on this host, would blank the console. Nothing in the observed software goes near it (§6.5).
- **MMIO registers are write-only except GR31.** Reads complete but return indeterminate data [7]
  Appendix B20 §1; drivers must keep their own copies of BLT parameters.
- **Drain the write buffer before starting a BLT.** "If the write buffer is not empty when a BitBLT
  is started, the BitBLT will not take place properly" [7] §9.40; SRF[6] must also be set while
  loading "page mode" text fonts [7] §9.6.
- **Extended RAS and color expansion are a suspicious pair.** The one documented extended-RAS
  acceleration erratum — "Color expand (without pattern copy) BitBLT operations fail when extended
  RAS* (7 MCLK) is enabled", workaround "Verify no pulldown resistor is installed on MD57" [9] — is
  against the CL-GD5434 production B, and this board *does* install the MD57 pull-down [1] §2.8. No
  equivalent erratum is published for the '5430, and no shipped software exercised the engine here;
  treat color-expansion BLTs under extended RAS as unproven on this part (§6.13).
- **No vertical blank, ever.** No interrupt line exists [1] p. 16, so retrace-synchronized software
  polls STAT or uses the delayed display start of CR1A[1] (§3.7). A driver ported from an
  interrupt-driven VGA card must be restructured, not just re-pointed.
- **Screen savers are a performance hazard by design.** "Screen savers should be discouraged for
  maximum system performance" [1] §2.8 p. 9 — the console is CPU-copied pixels over a 32-bit memory
  bus shared with refresh; the machine's own documentation recommends against the workload.
- **68 MHz sits at the edge of the documented clock range.** If Apple's "68 MHz clocking" is the
  MCLK, it is above the datasheet's 60 MHz rating for the '30/'34/'40 [8] and at the manufacturing
  test's ceiling [7] B18; if it is a pixel clock, it is routine (§3.3). The page does not resolve it
  (§6.2).

## 6. Open questions

1. **The exact die behind the marking.** CL-GD5430, CL-GD5440, or a customer-specific variant of
   either — the PCI ID, CR27 and the register file cannot separate them (§1.2). The discriminators
   are all readable on the hardware: CR28 (a '5430-only register — what a '5440 returns from index
   $28 is itself undocumented), the '5440's video-window registers CR31–CR3F, CR25 part status, and
   the PCI revision ID at $08. None has been recorded on the ANS board.
2. **What Apple's "68 MHz clocking" is.** MCLK (SR1F = `$26` = 68.0 MHz, above the datasheet's 60 MHz
   ceiling for the '30/'34/'40) or a 68 MHz-class VCLK for the 1024×768 modes (§3.3)? The extended-RAS
   framing favors MCLK; the specification ceiling favors VCLK. A register trace of the AIX driver's
   mode-set would settle it.
3. **The MD strap population of the board.** Only MD57 (extended RAS) is attested, by Apple's own
   sentence [1] §2.8. MD56 (power-on MCLK), MD62 (interrupt pin), MD51 (I/O relocation), MD58/MD59
   (DRAM steering) are unobserved — and each is software-readable: SR17[5:3], SRF[0]/SRF[1], SRF[2],
   PCI$14[0], PCI$3C[15:8] (§2.5).
4. **The DRAM fit.** Organization (8 × 256K×4 vs 2 × 256K×16), CAS/WE steering, vendor, speed grade
   and the actual installed MCLK ceiling per [9] Application Alert 3's method (§3.4).
5. **The expansion ROM.** Is an image populated behind BAR $30, and what size does firmware see?
   Open Firmware sizes the BAR [4]; the published `.properties` example for this machine covers only
   a slot card [2] Ch. 6, so the 54M30's own `assigned-addresses` — where its memory, I/O and ROM
   windows actually land in Bandit 1's space — has never been published.
6. **The DDC wiring and its use.** DDC1 sense-only or DDC2B (or both) between the chip and the HDI-15
   connector; which chip class that implies (CR28 `$01`/`$03` on a '5430, or the integrated-DDC2B
   production revisions of the '44); and whether AIX's `cirrusdd` reads EDID or trusts the service
   manual's mode table (§4.5).
7. **Who performs the first mode-set.** Open Firmware configures BARs and touches nothing else
   (§4.1) — so does anything ever light the VGA port before AIX's driver runs? The `screen` alias
   exists [4]; POST reports on the LCD ([ans.md](../../../machines/ans/ans.md) §2.7); the boot-time
   state of the display (which mode, if any, is active at `bootapple` time) is unrecorded.
8. **The prototype-ROM driver's register usage.** `.Display_Video_Cirrus_54M30` [5] exists but is
   undumped at the register level: dumb frame buffer or BLT/cursor acceleration, DDC or fixed modes,
   linear window or banked — all open (§4.3).
9. **What `cirrusdd` actually writes.** No register trace of the AIX driver exists: its VCLK/MCLK
   programming, whether it uses the linear window (SR7[7:4]) or the GR9/GR9A banked map, its palette
   path (6-bit VGA or the hidden-DAC extension), and its mouse-pointer strategy.
10. **The interrupt pin byte as read.** PCI$3C[15:8] should read `00` on this board (no MD62
    pull-down, or a '530 with no such latch at all) (§3.7); the readback has not been recorded. A
    reading of `01` would contradict Apple's "no interrupt line" sentence and re-open the die
    question in the process.
11. **The in-BAR layout of the standard-VGA memory map.** With SR7[7:4] = 0 the chip "responds to
    access at Axxx:x and Bxxx:x" [7] §9.2 — the correspondence of those legacy windows to offsets
    inside the assigned 16 MB block is inferred, not documented (§2.4).
12. **The '5440 hypothesis's collateral.** If the die is a CL-GD5440, the video-window and capture
    hardware (CR1F, CR31–CR3F) is present but presumably unwired — the board exposes no video port
    or feature connector ([1] §2.8 mentions only the VGA connector). Whether those registers answer
    at all, and what a driver would make of them, is untested.
13. **Extended RAS versus the BLT engine on this silicon.** The '5434-B color-expand-under-extended-
    RAS erratum (§5) has no published '5430 analog; with the strap pulled down on this board and no
    shipping software exercising the engine, the combination is unproven either way.

## References

1. Apple Computer, Inc., *Network Server Hardware Developer Notes* (Apple Network Server 500/700),
   c. 1996 — §2.8 p. 9 (on-board video: the controller, the 1 MB bit-mapped frame buffer, the
   little-endian window and the big-endian 8 bpp ceiling, 1024×768 at 8 bits, extended RAS and
   68 MHz clocking, acceleration and cursors requiring software implementation, the screen-saver
   warning, the standard VGA DDC-2 interface); §4.4 pp. 12–13 (discovery order, on-board devices
   configured before slots); §4.6.2 p. 16 (IDSEL positions: slots 13–14, video 15, SCSI 17–18);
   "Network Server External Interrupt Map" p. 16 (the 54M30 has no interrupt line); §7.1 p. 22
   (PCI definition; the PCI 2.1 compliance caveats — special cycle support and discontiguous byte
   enables).
2. Apple Computer, Inc., *Developer's Reference Guide for the Apple Network Server* (Software
   Developer Notes), 1996 — Chapter 6 pp. 48–49 (the published Open Firmware device tree, Listing
   6-1, including the `54m30@F` node; the name/reg guarantee and the ODM PdDv requirement); Chapter 8
   (address translation, programmed I/O, `busresolve` not supported).
3. Apple Computer, Inc. (Service Source), *Network Server 500/700 Series* service manual —
   Specifications/I/O p. 5 (the "SuperVGA (SVGA)" port requiring an HDI-15 cable); Specifications/
   Sound and Video pp. 6–7 (the video mode table: 640×480 at 60/70/72 Hz, 800×600 at 60/72/75 Hz,
   1024×768 at 60/72/75 Hz, 256 colors); Basics/Overview p. 3 (14-, 15-, 17- and 20-inch multisync
   display support).
4. Apple Network Server 500/700 boot ROM, production release (Open Firmware 1.1.22) — 4 MB image,
   header checksum $962F6C13, version field $077D.28F2 (ROM identity evidence in
   [ans.md](../../../machines/ans/ans.md) §1.3). Evidence used here: the detokenized Open Firmware
   source — the `54m30-config` word (reg sizes read from $10/$14/$30, `device-name " 54m30"`,
   `compatible = "pci1013,a0"`), the `install-54m30` device installation, and the `screen` alias at
   `/bandit/54m30@F`.
5. Apple Network Server boot ROM, unreleased prototype ("2.0", image dated 1998-01-14) — 4 MB image,
   header checksum $49B2BE8F, version field $077D.7DD0, carrying the Mac OS driver
   `.Display_Video_Cirrus_54M30` and Open Firmware 2.0.
6. IBM AIX 4.1.5 for the Apple Network Server, installation media — the ODM device stanzas shipped
   in the install image: fileset `devices.pci.pci1013+a0` ("Cirrus 54M30 video"), `PdDv` with
   `devid = "pci1013,a0"`, `type = "cirrus"`, `DvDr = "cirrusdd"`, `Configure =
   /etc/methods/cfgcirrus`, description "Cirrus Graphics Adapter Software"; and the `cfggc`
   configuration method's embedded `resolve_pci_mem_space` routine.
7. Cirrus Logic, Inc., *Alpine VGA Family CL-GD543X/'4X Technical Reference Manual*, fourth edition,
   February 1995 — §4.14–§4.22 (the PCI configuration registers, with reset values); §4.5–§4.13
   (the general registers); Chapters 5–8 (VGA sequencer, CRT controller, graphics controller and
   attribute controller registers); Chapter 9 (extension registers: §9.1 SR6, §9.2 SR7, §9.3 SR8,
   §9.5 VCLK numerators, §9.6 SRF, §9.7–§9.10 cursor, §9.13 SR17, §9.18 SR1F, §9.19–§9.22 GR9–GRB,
   §9.28–§9.43 BitBLT, §9.44 CR1A, §9.45 CR1B, §9.50 CR27, §9.51 CR28); Appendix B5 (CL-GD5430
   notes), B7 (memory configurations and timing), B8 (synthesizer and DAC current reference), B9
   (configuration notes), B16 (DDC1/2B support), B18 (manufacturing test), B20 (memory-mapped I/O),
   D2 (linear addressing), D3 (hardware cursor).
8. Cirrus Logic, Inc., CL-GD543X/'4X family datasheet — the family feature summary: pixel clock
   programmable to 86 MHz (CL-GD5430/'40) and 135 MHz ('34/'36); memory clock programmable to 60 MHz
   (CL-GD5430/'34/'40) and 80 MHz ('36); PCI v2.0 with burst-cycle support ('36) and VESA VL-Bus
   v2.0 ('30/'34/'40); 1/2-, 1- and 2-Mbyte display memory support (CL-GD5430/'40); 64×64 hardware
   cursor; 208-pin PQFP package.
9. Cirrus Logic, Inc., *CL-GD543X Applications and Errata Book*, April 1994 — Application Alert 3
   ("Calculated Maximum MCLK for CL-GD5430/34", standard vs extended RAS timing limits per DRAM
   device and speed grade); the PCI configuration-write erratum (byte/word writes to $04/$10/$30
   clearing unwritten bytes, CL-GD5430-1MB-Q-X / CL-GD5434-HC-B, corrected in -Q-Y / -HC-C); the
   non-contiguous byte-enable erratum (Application Alert 8, adding the CL-GD5430-1MB); the
   CL-GD5434 Rev AF (production B) errata list (including "BitBLT: Color expand with Extended RAS*"
   and the memory-mapped I/O readback restriction on GR0/GR1/GR10/GR11).
