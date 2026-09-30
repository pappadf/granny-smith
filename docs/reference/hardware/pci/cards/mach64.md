# ATI mach64 GX and VT

**Contents:**

1. [Overview](#1-overview) — what the chips are, the Macintosh cards built on them, host machines and
   the onboard variants, the evidence base
2. [Register file](#2-register-file) — apertures and the I/O views; the register classes;
   configuration, CRTC, DAC, memory, draw-engine control, draw-engine trajectory registers; the
   context block; reset state
3. [Behaviour](#3-behaviour) — endianness, the command FIFO, the pixel data path, trajectories,
   contexts, monitor sensing, clocking, interrupts, the hardware cursor
4. [Programming model](#4-programming-model) — PCI configuration space, the FCode bring-up and its
   register-init table, the RAMDAC bring-up, CRTC modes, the Mac OS driver, the ATI Graphics
   Accelerator, observed run-time behaviour
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the parts are

The **ATI mach64** is a family of 64-bit GUI graphics accelerators: a VLSI graphics controller
combining "a 64-bit GUI accelerator and a VGA-compatible graphics controller" — the accelerator
being also called the coprocessor or **draw engine** — usable for board- or system-level designs
across ISA, VLB, EISA and PCI buses, with VRAM or DRAM in 1 MB to 8 MB configurations [3] §8-1.
The family's accelerator register model is common to its members: the later chips are
"register-compatible with other controllers in the ATI mach64 accelerator series" [3] §1-1.
Four accelerator register classes exist on the GX generation — Setup and Control, Accelerator
CRTC and DAC, Draw Engine Control, and Draw Engine Trajectory [1] Ch. 1 — extended on the VT
generation by Multimedia, PCI Configuration Space, VGA, and (on the 3D RAGE members)
Scaler/3D classes [3] §1-1.

Two members of this family matter on the Power Macintosh, and this page covers both as PCI
graphics-card subjects:

- **mach64 GX** — the ATI 88800GX, the 1994 first release. The GX is a *discrete-DAC* design:
  the palette DAC and the clock synthesizer are separate chips on the card, identified through
  `CONFIG_STAT0`'s straps and programmed through `DAC_CNTL`/`DAC_REGS` and `CLOCK_CNTL`
  respectively [1] pp. 3-4, 3-31, 3-12. Apple's 1995 display card for the Power Macintosh 9500
  — the **Apple Accelerated PCI Graphics Card**, ATI board 109-32900, card codename *Spinnaker*
  — is a mach64 GX board (§1.2) [5] [6].
- **mach64 VT** — the ATI-264VT (up to revision VT-A4), the successor that folds the DAC and
  the clock onto the die: it adds "a video coprocessor to provide superior hardware video
  acceleration" with a video scaler, integrated video line buffer, colour space conversion and
  video/graphics keying, a "True Color palette DAC, supporting pixel clock rates to 135 MHz"
  and an "Internal dual clock synthesizer" [3] §1-1. The VT generation also reorganises the
  register address space into two 1 KB blocks at the top of the aperture (§2.1) [3] §2-1.

The 3D RAGE (mach64 GT) line that follows the VT is register-compatible with the VT core [3]
§1-1 and appears onboard in Apple hardware (§1.3); its 3D and scaler extensions are out of this
page's scope. The GX-generation CT (integrated-DAC, ISA/VLB-oriented) is covered here only
where the Apple GX card's own ROM handles it (§4.2): the card's firmware is written to serve
both chips.

### 1.2 The cards on the Power Macintosh

| Card | Board | Chip | PCI IDs (vendor:device) | VRAM | Video connectors | Notes |
|---|---|---|---|---|---|---|
| Apple Accelerated PCI Graphics Card ("Spinnaker") | Apple/ATI 109-32900 | 88800GX, silicon rev GX-2 (*observed*: PCI revision 2 [7]) | 1002:4758 | 2 MB soldered, 4 MB with expansion module 109-31600-00 [6] §"Video Display Card" p. 13 | DB-15 | the 9500's bundled card; ROM 113-32900-1xx [5] |
| ATI Xclaim GA (retail twin of the above) | 109-33200 | 88800GX | 1002:4758 | same | DB-15 + HD-15 | FCode publishes `name = "ATY,XCLAIM"` on this board [7]; retail driver update 1.2 in the archive record [5] |
| mach64 VT Mac card | 109-36300-00 | 264VT | 1002:5654 (*observed* in the ROM's PCI data structure [5]) | not established | not established | ROM `262VT V1.0B14`, 64 KB, FCode image [5]; the VT's capture/overlay capability [3] Ch. 5 is the tuner-adjacent feature of this generation |

The GX card is Apple's own display card for the Power Macintosh 9500: "a graphics accelerator
with a 64-bit data bus to either 2 MB or 4 MB of fast VRAM", accelerated on monitors up to
1280 by 1024, with "firmware on the card [containing] a video device driver that supports the
initialization required during system startup" [6] §"Video Display Card" pp. 13-14. The 9500
developer note's card contract — IEEE 1275 startup ROM, PCI rev 2.0 compliance, DB-15
connector, three monitor sense lines — is the machine page's
([pm9500.md](../../../machines/tnt/pm9500.md) §3.8); this page supplies the silicon. The two
board numbers are twins in Apple's parts system and in the ROM parts they carry: the *-104*
ROM revision publishes `ATY,Rom# = "113-32900-104"`, `ATY,Mem# = "100-31602-00"`,
`ATY,Card# = "102-329XX-XX"`, while the card Technote 1062 photographs publishes
`ATY,Rom# = "113-33200-110"`, `ATY,Card# = "109-33200-00"` over the same `ATY,Mem#` [4] [7].
The two boards differ in the second connector (HD-15 on the 109-33200) and in the ROM's
published name — `ATY,mach64` versus `ATY,XCLAIM` — nothing else in the evidence distinguishes
them (§6.2).

Third-party mach64 cards of the same generation follow the same pattern (the driver-archive
record lists the installer family as covering the 9500 card, the retail Xclaim twins and
`ATY,264VT`/`ATY,264GT-B` parts [5]); their board-level details are not in the evidence set.

### 1.3 Host machines and the onboard variants

The GX card's target host is the **Power Macintosh 9500** — the TNT-platform machine that ships
with *no* video hardware of its own, so that "a display card in a PCI slot is a requirement, not
an option" ([pm9500.md](../../../machines/tnt/pm9500.md) §2.5; [6] §"Video Display Card" p. 13).
The same card works in any PCI slot of the whole PCI Power Macintosh generation: the three-slot
7500/8500 (whose onboard Control/Chaos video is *not* a mach64 — it is the TNT video subsystem
of [tnt.md](../../../machines/tnt/tnt.md) §2.4), the six-slot 9500 served by two Bandit
bridges ([bandit.md](../../../machines/tnt/bandit.md) §1), and the Apple Network Server — whose
onboard video is a Cirrus 54M30 with no Mac OS driver, so that "a Mac OS boot on this hardware
therefore goes through a PCI video card in a slot" ([ans.md](../../../machines/ans/ans.md)
§2.5; the slot map is ans.md §3.3). The PCI bus itself — configuration cycles, the memory and
I/O windows, the big-endian host to little-endian bus seam — is [pci.md](../pci.md)'s.

The **onboard mach64 variants** are the 3D RAGE members: the Power Macintosh 5500/6500
(Gazelle) carry an onboard ATI 3D Rage II — the mach64 GT-B — as their graphics accelerator
[10], and the beige G3 generation carries Rage II+/Rage Pro parts of the same lineage. These
are register-compatible with the VT core [3] §1-1; their onboard wiring (integrated DAC, SGRAM,
the device-tree node `ATY,264GT-B`) follows the VT model of §2 and §3, and their Rage-specific
extensions are not covered here. The mach64 GX card and the VT card never appear onboard in
Apple hardware (*inferred from the absence of any such configuration in the Apple documentation
in the evidence set*).

### 1.4 Evidence base

Every hardware fact on this page is derived from primary evidence: the ATI register references
and the mach64 accelerator programmer's guide for the GX [1] [2] and the VT/3D RAGE [3]; the
detokenized FCode of the Apple GX card's own expansion ROM (two revisions, byte-exact walks)
[4]; the physical record of the cards — EPROM dumps and board photographs — plus the
Macintosh driver-archive record [5]; Apple's 9500 developer note [6]; Apple's Technote 1062
device-tree dump of a live card [7]; the IBM RGB514 palette-DAC data sheet for the GX card's
RAMDAC [8]; and a recorded-observation log of the card running under Mac OS [9]. Claims that
rest on a single reading are marked *inferred*; claims no shipped code exercises are left to
[§6](#6-open-questions). What is *not* in the evidence set: ATI's chip-level specification for
the 88800GX (GCS-C012001-00, not publicly available [5]), the 5500/6500 developer note's
onboard-video chapter in extractable form, and any FCode decode of the VT card's ROM — the
honest consequence is a register file complete at the documented level but thin on silicon
internals, and a large Open questions section.

## 2. Register file

### 2.1 Address spaces and apertures

The mach64 presents three accelerator address spaces, plus the expansion ROM:

| Space | Where | Contents |
|---|---|---|
| PCI configuration space | config cycles | vendor/device/class/revision, command, BAR(s) (§4.1) |
| Memory aperture | memory BAR(s) | linear framebuffer + the memory-mapped register blocks |
| I/O | the I/O decode (§2.2) | the "setup and control" and CRTC/DAC registers — everything except the draw engine |

- **GX**: the linear aperture is "4M or 8M in size, and is locatable on 4M or 8M boundaries
  respectively, anywhere in the 4G address space"; it is off at power-up and enabled only via
  `CONFIG_CNTL` [2] §"Big Aperture". The memory-mapped registers sit in the **top 1 KB** of
  the enabled aperture: at base + $3FFC00 for a 4 MB aperture or base + $7FFC00 for an 8 MB
  aperture [1] Ch. 1 "How To Find The Registers"; [2] §"Register Mapping". Apple's GX card runs
  the 8 MB form (*observed*: the FCode programs an 8 MB aperture, §4.2 [4]).
- **GX small apertures**: two paged 32 KB windows at host $A000/$A800, usable in accelerator
  mode when `CFG_MEM_VGA_AP_EN` is set — but "VGA apertures are not supported if
  CFG_BUS_TYPE = PCI. A 4M or 8M linear aperture must be used for PCI bus implementation"
  [1] p. 3-70. On the Mac the linear aperture is the only framebuffer path.
- **VT**: "In linear aperture mode, the aperture size is fixed at 8MB" and the position comes
  from the PCI configuration space; "the upper 1KB block is known as block 0" and a second 1 KB
  **block 1** is added just below it — registers at aperture + $7FFC00 (block 0) and + $7FF800
  (block 1) [3] §2-1, Table 2-1. Block notation below is `MM:block_offset`, e.g. `MM:1_09` is
  block 1, dword 9 [3] §"Memory Mapping".

All registers are 32 bits wide except `DAC_REGS`, which is four 8-bit registers [1] Ch. 1.

### 2.2 The I/O views: sparse and block

The registers outside the draw engine are I/O mapped as well as memory mapped — with one
load-bearing exception: on the GX, **`CONFIG_CNTL` is I/O-mapped only** ("all except
CONFIG_CNTL have memory mapped register aliases" [1] Ch. 1; [2] §"Register Mapping"). Since
the apertures are disabled at power-up, sparse I/O is the only way to reach `CONFIG_CNTL` to
turn one on — the bootstrapping fact the Apple card's firmware depends on (§4.2).

Two decode styles exist [3] §2-3:

- **Sparse I/O** (ISA style, strap-selected base): the address is
  `(I/O select << 10) + I/O base`, where the base is 2ECh, 1CCh or 1C8h and the top six bits
  of the address are the register's I/O select [1] Ch. 1; [3] §2-3. Example from the manual:
  `SCRATCH_REG1` (select 11h) at base 2ECh answers at 46ECh [3] §2-4. Sub-dword access works
  by adding the byte lane to the computed address (0–3) — the manual's worked example reaches
  the `DAC_MASK` byte at 5EEEh [3] §"VGA Registers". Apple's GX card uses base 2ECh, so its
  `CONFIG_CNTL` (select 1Ah) answers at **$6AEC** — with byte lanes $6AEC–$6AEF — and the
  card's firmware drives it with **16-bit** writes at $6AEC and $6AEE (*observed* [4]).
- **Block I/O** (PCI only, from the VT generation): "registers not associated with the draw
  engine and multimedia map into a continuous block that starts at the I/O base address
  specified in the PCI configuration registers"; the address is
  `I/O base + (BLK select << 2)` [3] §2-3. All block-0 registers are visible in block I/O,
  including those with no sparse alias [3] §2-4.

### 2.3 Register class summary

The GX register file, by class — MM is the dword offset from the register block base; "I/O" is
the sparse select [1] Ch. 2:

| Class | Register | R/W | I/O | MM | Detail |
|---|---|---|---|---|---|
| Clock | CLOCK_CNTL | R/W | 12h | 24h | [1] p. 3-4 |
| CRTC | CRTC_H_TOTAL_DISP | R/W | 0h, 1Fh | 0h | [1] p. 3-20 |
| CRTC | CRTC_H_SYNC_STRT_WID | R/W | 1h | 1h | [1] p. 3-19 |
| CRTC | CRTC_V_TOTAL_DISP | R/W | 2h | 2h | [1] p. 3-24 |
| CRTC | CRTC_V_SYNC_STRT_WID | R/W | 3h | 3h | [1] p. 3-23 |
| CRTC | CRTC_VLINE_CRNT_VLINE | R/W | 4h | 4h | [1] p. 3-25 |
| CRTC | CRTC_OFF_PITCH | R/W | 5h | 5h | [1] p. 3-22 |
| CRTC | CRTC_INT_CNTL | R/W | 6h | 6h | [1] p. 3-21 |
| CRTC | CRTC_GEN_CNTL | R/W | 7h | 7h | [1] p. 3-17 |
| Overscan | OVR_CLR, OVR_WID_LEFT_RIGHT, OVR_WID_TOP_BOTTOM | R/W | 8h–Ah | 10h–12h | [1] pp. 3-72–3-74 |
| Cursor | CUR_CLR0, CUR_CLR1, CUR_OFFSET, CUR_HORZ_VERT_POSN, CUR_HORZ_VERT_OFF | R/W | Bh–Fh | 18h–1Ch | [1] pp. 3-26–3-30 |
| Scratch/test | SCRATCH_REG0, SCRATCH_REG1 (TEST_REG0–7 in test modes) | R/W | 10h–11h | 20h–21h | [1] pp. 3-84–3-109 |
| Bus | BUS_CNTL | R/W | 13h | 28h | [1] p. 3-2 |
| Memory | MEM_CNTL | R/W | 14h | 2Ch | [1] p. 3-67 |
| Memory | MEM_VGA_WP_SEL / MEM_VGA_RP_SEL | R/W | 15h/16h | 2Dh/2Eh | [1] pp. 3-70–3-71 |
| DAC | DAC_REGS | R/W | 17h | 30h | [1] p. 3-33 |
| DAC | DAC_CNTL | R/W | 18h | 31h | [1] p. 3-31 |
| Test | GEN_TEST_CNTL | R/W | 19h | 34h | [1] p. 3-57 |
| Config | CONFIG_CNTL | R/W | 1Ah | — (I/O only) | [1] p. 3-9 |
| Config | CONFIG_CHIP_ID | R | 1Bh | 38h | [1] p. 3-8 |
| Config | CONFIG_STAT0 | R | 1Ch | 39h | [1] p. 3-12 |
| Config | CONFIG_STAT1 / CRC_SIG | R | 1Dh | 3Ah | [1] p. 3-14 |
| Data path | DP_BKGD_CLR, DP_FRGD_CLR, DP_WRITE_MSK, DP_CHAIN_MSK, DP_PIX_WIDTH, DP_MIX, DP_SRC | R/W | — | B0h–B6h | [1] pp. 3-34–3-41 |
| Colour compare | CLR_CMP_CLR, CLR_CMP_MSK, CLR_CMP_CNTL | R/W | — | C0h–C2h | [1] pp. 3-5–3-7 |
| Context | CONTEXT_MASK, CONTEXT_LOAD_CNTL | R/W | — | C8h, CBh | [1] pp. 3-15–3-16 |
| Engine status | FIFO_STAT, GUI_STAT | R | — | C4h, CEh | [1] pp. 3-56, 3-61 |
| Engine control | GUI_TRAJ_CNTL | R/W | — | CCh | [1] p. 3-62 |
| Host data | HOST_CNTL, HOST_DATA[0:15] | R/W, W | — | 90h, 80h–8Fh | [1] pp. 3-64–3-65 |
| Pattern | PAT_REG0, PAT_REG1, PAT_CNTL | R/W | — | A0h–A2h | [1] pp. 3-75–3-77 |
| Scissor | SC_LEFT, SC_RIGHT, SC_LEFT_RIGHT, SC_TOP, SC_BOTTOM, SC_TOP_BOTTOM | R/W, W | — | A8h–ADh | [1] pp. 3-78–3-83 |
| Destination | DST_OFF_PITCH … DST_CNTL (13 registers) | R/W | — | 40h–4Ch | [1] pp. 3-42–3-55 |
| Source | SRC_OFF_PITCH … SRC_CNTL (15 registers) | R/W | — | 60h–6Dh | [1] pp. 3-86–3-100 |

All draw-engine registers (MM offsets ≥ 40h) are memory-mapped only and are written through
the command FIFO (§3.2); registers below 40h are not FIFOed [2] §"The Command FIFO". On the
VT the same file is spread over blocks 0 and 1 (`MM:0_xx` / `MM:1_xx`), the CRTC/DAC file gains
`EXT_CRTC_GEN_CNTL` (VT-A4) at MM:1_17h, and the multimedia registers of [3] Ch. 5 occupy
further block-1 space [3] Ch. 3.

### 2.4 Configuration registers

**CONFIG_CNTL** (I/O 1Ah, I/O-only on the GX; MM 0_37h on the VT) configures the linear
aperture and multi-card identity [1] p. 3-9; [3] p. 4-16:

| Field | Bits | Meaning |
|---|---|---|
| CFG_MEM_AP_SIZE | 1:0 | GX: 0 = aperture disabled, 1 = 4 MB, 2 = 8 MB [1] p. 3-9. VT/GT: read-only, fixed 2 ("2x8 MByte apertures") [3] p. 4-16 |
| CFG_MEM_VGA_AP_EN | 2 | VGA aperture enable [1] p. 3-9 |
| CFG_MEM_AP_LOC | 11:4 | aperture base in 4 MB increments (8 MB apertures ignore bit 4) — the mask `(config & 0x3ff0) >> 4 << 22` recovers the base [1] p. 3-9; [2] §"Sample Code LINAPP.DOC" |
| CFG_CARD_ID | 19:16 | card select control for multi-card systems; defaults to the `CFG_INIT_CARD_ID` strap [1] p. 3-9 |
| CFG_VGA_DIS | upper bits | VGA disable: 0 = VGA enabled, 1 = disabled [1] p. 3-9 |

On the VT in a PCI system the aperture size and location are read-only — the PCI configuration
space fixes them [3] p. 4-16. The Apple GX card's firmware writes this register three times at
boot (§4.2).

**CONFIG_CHIP_ID** (read-only) returns chip identity: `CFG_CHIP_TYPE` — an alphanumeric code
built from two uppercase letters, 5 bits each, upper 6 bits zero — with `CFG_CHIP_CLASS` and
`CFG_CHIP_REV` as numeric codes [1] p. 3-8:

| ASIC | CFG_CHIP_TYPE | CFG_CHIP_CLASS | CFG_CHIP_REV |
|---|---|---|---|
| GX-0 | 'GX' = D7h | 00h | 00h |
| GX-1 | 'GX' = D7h | 00h | 01h |
| GX-2 | 'GX' = D7h | 00h | 02h |
| CX | 'CX' = 57h | 00h | 03h |
| EX | 'EX' = 97h | 00h | 03h |
| CT | 'CT' = 53h | 00h | 00h |

**CONFIG_STAT0** (read-only) returns the board configuration as the straps encoded it [1]
p. 3-12. The GX-relevant fields:

| Field | Meaning |
|---|---|
| CFG_BUS_TYPE | host bus: 0 = PCI, 1 = VLB, 6 = EISA, 7 = ISA [1] p. 3-12 |
| CFG_MEM_TYPE | memory type: the VRAM/DRAM families, x4/x16, short shift register (GX-2 adds enhanced-VRAM encodings) [1] p. 3-12 |
| CFG_INIT_DAC_TYPE | DAC type strap: TVP3020, ATI68875, BT476/BT478, BT481, ATI68860/68880, STG1700, SC15021 — the discrete-DAC list of the GX era [1] p. 3-12 |
| CFG_INIT_CARD_ID | card ID strap, 0–6, 7 = feature disabled [1] p. 3-12 |
| CFG_VGA_EN, CFG_CHIP_EN, CFG_ROM_DIS, CFG_ROM_ADDR, CFG_EXT_ROM_ADDR | VGA/chip/ROM straps [1] p. 3-12 |
| CFG_TRI_BUF_DIS, CFG_AP_4GBYTE_DIS, bus-option straps | electrical and configuration options [1] p. 3-12 |

The VT replaces the DAC-strap field with `DAC_TYPE = 0 = internal DAC` — there is no discrete
DAC to identify [1] p. 3-31; [3] p. 4-40. **CONFIG_STAT1** continues the file (the PCI
DAC-latch configuration and the 1C8h I/O-base strap on the GX) and doubles as the display-CRC
result register `CRC_SIG` in test mode [1] p. 3-14. Note the read-only contract: the Apple
card's firmware *writes* to CONFIG_STAT0's I/O alias anyway, and a working part must accept and
discard that write (§4.3) [4].

### 2.5 CRTC and clock registers

The accelerator CRTC is a separate register file from the VGA CRTC [1] Ch. 1. Horizontal
parameters are in characters (pixels × 8), vertical parameters in lines [1] pp. 3-20, 3-24.

| Register | Fields |
|---|---|
| CRTC_H_TOTAL_DISP (MM 0) | CRTC_H_TOTAL bits 11:0, CRTC_H_DISP bits 23:16 — both stored as count − 1, in characters [1] p. 3-20 |
| CRTC_H_SYNC_STRT_WID (MM 1) | horizontal sync start, width, polarity [1] p. 3-19 |
| CRTC_V_TOTAL_DISP (MM 2) | CRTC_V_TOTAL bits 11:0, CRTC_V_DISP bits 26:16, count − 1, in lines [1] p. 3-24 |
| CRTC_V_SYNC_STRT_WID (MM 3) | vertical sync start (11:0), width (23:16), polarity (bit 29) [1] p. 3-23 |
| CRTC_VLINE_CRNT_VLINE (MM 4) | CRTC_VLINE — the line that triggers the vertical-line interrupt — bits 11:0, read-only CRTC_CRNT_VLINE bits 26:16 [1] p. 3-25 |
| CRTC_OFF_PITCH (MM 5) | CRTC_OFFSET bits 19:0 (display start, in 64-bit words), CRTC_PITCH bits 38:20 (display pitch, in pixels × 8) [1] p. 3-22 |
| CRTC_INT_CNTL (MM 6) | interrupt control and status, §3.8 [1] p. 3-21 |
| CRTC_GEN_CNTL (MM 7) | master control, below [1] pp. 3-17–3-18 |

CRTC_GEN_CNTL, the fields the Apple card's firmware touches [1] pp. 3-17–3-18:

| Field | Meaning |
|---|---|
| CRTC_DBL_SCAN_EN / CRTC_INTERLACE_EN | double scan, interlace |
| CRTC_HSYNC_DIS / CRTC_VSYNC_DIS | sync output disables (DPMS) |
| CRTC_CSYNC_EN | composite sync on the horizontal sync output |
| CRTC_DISPLAY_DIS | forces blanking active |
| CRTC_PIX_WIDTH (bits 11:8) | 1 = 4 bpp, 2 = 8 bpp, 3 = 15 bpp, 4 = 16 bpp, 5 = 24 bpp, 6 = 32 bpp |
| CRTC_BYTE_PIX_ORDER | pixel order within a byte (4 bpp) |
| CRTC_FIFO_LWM | display-FIFO low water mark (DRAM configurations; FIFO is 16 entries) |
| CRTC_EXT_DISP_EN (bit 24) | 0 = VGA display, 1 = extended (accelerator) display |
| CRTC_EN (bit 25) | 0 = CRTC reset, 1 = CRTC enable |

The VT adds `CRTC_FIFO_OVERFILL`, `VGA_128KAP_PAGING`, `CRTC_DISPREQ_ONLY`, `CRTC_LOCK_REGS`,
`CRTC_SYNC_TRISTATE`, and (VT-A4) a fifth LWM bit in EXT_CRTC_GEN_CNTL [3] pp. 4-27–4-29.

**CLOCK_CNTL** (I/O 12h, MM 24h) selects the pixel clock [1] p. 3-4. On the GX it drives
*external* clock hardware: `CLOCK_SEL`/`CLOCK_DIV`/`CLOCK_STROBE` for fixed-frequency chips of
the ATI1881x type, or `CLOCK_SERIAL_DATA_EN`/`CLOCK_SERIAL_DATA`/`CLOCK_STROBE` bit-banged
serial programming for programmable synthesizers [1] p. 3-4. The Apple GX card's clock is the
programmable-synthesizer case — its PLL lives inside the RGB514 DAC and is programmed through
the DAC register path, not through CLOCK_CNTL (§4.4) [4]. On the VT the register instead
addresses the **internal** dual clock synthesizer through `PLL_WR_EN`, `PLL_ADDR` and
`PLL_DATA`, with only bits 0–1 of `CLOCK_SEL` used because "the internal clock synthesizer in
the VT and 3D RAGE has only 4 programmable pixel clock settings" [3] p. 4-38; the PLL register
layout is [3] Appendix B.

### 2.6 DAC registers

**DAC_CNTL** (I/O 18h, MM 31h) configures the DAC interface [1] p. 3-31:

| Field | Meaning |
|---|---|
| DAC_EXT_SEL (bits 2:0) | extended register select lines to a discrete DAC's extended address bits |
| DAC_8BIT_EN (bit 8) | 8-bit (versus 6-bit VGA) palette/DAC mode |
| DAC_PIX_DLY / DAC_BLANK_ADJ | pixel/blank pipeline delays, in pixel clocks |
| DAC_VGA_ADR_EN | allows the VGA I/O DAC addresses to work in extended display mode |
| DAC_TYPE | mirror of the `CFG_INIT_DAC_TYPE` strap — the discrete-DAC identification on the GX |
| DAC_MON_ID_STATE (bits 26:24) | monitor ID pin state — the sense read-back (§3.6) |
| DAC_MON_ID_DIR (bits 29:27) | monitor ID pin direction: 0 = all input; 1, 2, 4 = pin 0, 1 or 2 output |
| DAC_MON_CMP_STATE, DAC_CMP_OUTPUT (GX-2) | RGB level comparator read-outs |

**DAC_REGS** (I/O 17h, MM 30h) is "actually a group of four 8-bit registers" aliased to the
VGA DAC ports — `DAC_W_INDEX`, `DAC_DATA`, `DAC_MASK`, `DAC_R_INDEX` — and "must be accessed
in 8-bit chunks"; the palette data port auto-increments its index after three consecutive
accesses [1] p. 3-33. The `DAC_EXT_SEL` bits of DAC_CNTL add the upper address lines a discrete
DAC needs to reach its *extended* register file — the mechanism by which the GX reaches the
RGB514's indexed registers (§4.4) [1] p. 3-31; [4].

### 2.7 Memory control registers

**MEM_CNTL** (I/O 14h, MM 2Ch) configures the memory interface [1] pp. 3-67–3-68. The fields
that matter to a re-implementation:

| Field | Meaning |
|---|---|
| MEM_SIZE (bits 2:0) | GX-0/GX-1: 1 = 1 MB … 5 = 8 MB (default 512K; value 0 reserved on GX-2); the firmware writes the strap-visible memory size [1] p. 3-67 |
| MEM_BNDRY (bits 8:9), MEM_BNDRY_EN (bit 12) | the VGA/accelerator memory boundary |
| MEM_CYC_LNTH | non-page memory cycle length (5–7 memory clocks) |
| latch/delay fields | RAM-port and serial-access-port data latch enables and half-clock delays — board-tuning values, written once at boot |
| MEM_REFRESH_RATE (CT) | refresh rate per horizontal total |
| MEM_PIX_WIDTH (GX-2) | big-endian memory aperture pixel width — used "only in PCI bus configuration with the CFG_BIG_ENDIAN_EN strap set" [1] p. 3-68 |

**MEM_VGA_WP_SEL / MEM_VGA_RP_SEL** (I/O 15h/16h, MM 2Dh/2Eh) are the read/write page pointers
for the two 32 KB small apertures — unused on a PCI Mac card, since "a 4M or 8M linear aperture
must be used for PCI bus implementation" [1] pp. 3-70–3-71.

### 2.8 Draw-engine control registers

The data-path group selects what the engine draws with [1] pp. 3-34–3-41:

| Register | MM | Function |
|---|---|---|
| DP_BKGD_CLR | B0h | background colour (width follows the mode: 1/4/8/15-16/24/32 bits used) |
| DP_FRGD_CLR | B1h | foreground colour |
| DP_WRITE_MSK | B2h | per-bit write mask: zero bits preserve the destination bit |
| DP_CHAIN_MSK | B3h | carry-chain mask for mix $17 ((D+S)>>1); per-depth normal values tabulated in [1] p. 3-35 (8 bpp → $8080) |
| DP_PIX_WIDTH | B4h | three independent width selects — destination, source, host — plus DP_BYTE_PIX_ORDER; the only conversion supported is 1 bpp monochrome to any depth [1] p. 3-39 |
| DP_MIX | B5h | background and foreground mix selects (§3.3) |
| DP_SRC | B6h | background, foreground and monochrome *source* selects: background/foreground colour, host data, blit source, pattern registers; monochrome: Always_'1', pattern, host, blit [1] p. 3-40 |

The compare group (`CLR_CMP_CLR` C0h, `CLR_CMP_MSK` C1h, `CLR_CMP_CNTL` C2h) compares a colour
against destination or source pixels and can veto writes (§3.3) [1] pp. 3-5–3-7. The scissor
group (`SC_LEFT` A8h … `SC_TOP_BOTTOM` ADh) clips every destination pixel to a rectangle; the
pattern group (`PAT_REG0/1` A0h/A1h, `PAT_CNTL` A2h) holds the 8×8 monochrome and 4×2/8×1
colour fixed patterns. `HOST_CNTL` (90h) and the sixteen-fold `HOST_DATA[0:15]` (80h–8Fh) feed
host pixel data (§3.3). `FIFO_STAT` (C4h) and `GUI_STAT` (CEh) report engine state (§3.2).
`GEN_TEST_CNTL` (I/O 19h, MM 34h) collects the EEPROM pins, the overscan output
enable/polarity, the cursor enable, the engine reset, the VRAM block-write enable, and the
diagnostic test modes [1] pp. 3-57–3-59. **GUI_TRAJ_CNTL** (CCh) is documented as "a composite
of registers DST_CNTL, SRC_CNTL, PAT_CNTL, and HOST_CNTL" [1] p. 3-62 — bits 15:0 are the
DST_CNTL bits and bits 20:16 the SRC_CNTL bits (DST_X_DIR at bit 0, DST_Y_DIR at 1,
SRC_PATT_EN at 16, SRC_PATT_ROT_EN at 17, per the two registers' own charts [1] pp. 3-46,
3-86), with the pattern and host control bits above them — including `HOST_BIG_ENDIAN_EN`
(§3.1) [1] p. 3-63. Context control is §3.5's subject.

### 2.9 Draw-engine trajectory registers

The destination group defines where pixels land and *initiates* every draw [1] pp. 3-42–3-55:

| Register | MM | Function |
|---|---|---|
| DST_OFF_PITCH | 40h | destination offset (bits 19:0, in 64-bit words) and pitch (bits 38:20, pixels × 8) |
| DST_X / DST_Y / DST_Y_X / DST_WIDTH / DST_HEIGHT / DST_HEIGHT_WIDTH / DST_X_WIDTH | 41h–47h | destination coordinates; the three write-only composites initiate rectangular draws: DST_HEIGHT_WIDTH = (width << 16) \| height, DST_Y_X = (x << 16) \| y — the first-named field is always the LOW halfword (§5) |
| DST_BRES_LNTH | 48h | line length — initiates the Bresenham line |
| DST_BRES_ERR / DST_BRES_INC / DST_BRES_DEC | 49h–4Bh | line error and terms (§3.4) |
| DST_CNTL | 4Ch | direction/octant and modifier bits: DST_X_DIR (0 = right-to-left, 1 = left-to-right), DST_Y_DIR, DST_Y_MAJOR, DST_X_TILE/DST_Y_TILE (side effects, §3.4), DST_LAST_PEL, DST_POLYGON_EN, the 24-bpp rotation pair, DST_BRES_SIGN [1] pp. 3-46–3-47 |

The source group defines where pixels come from [1] pp. 3-86–3-100:

| Register | MM | Function |
|---|---|---|
| SRC_OFF_PITCH | 60h | source offset/pitch, same encoding as DST_OFF_PITCH |
| SRC_X / SRC_Y / SRC_Y_X | 61h–63h | source origin |
| SRC_WIDTH1 / SRC_HEIGHT1 / SRC_HEIGHT1_WIDTH1 | 64h–66h | source window 1 |
| SRC_X_START / SRC_Y_START / SRC_Y_X_START | 67h–69h | pattern-rotation restart point |
| SRC_WIDTH2 / SRC_HEIGHT2 / SRC_HEIGHT2_WIDTH2 | 6Ah–6Ch | pattern block size for rotation |
| SRC_CNTL | 6Dh | trajectory selects (§3.4): SRC_PATT_EN, SRC_PATT_ROT_EN, SRC_LINEAR_EN, SRC_BYTE_ALIGN, SRC_LINE_X_DIR |

### 2.10 The context block

The engine can load its whole state from a 64-dword block in video memory (§3.5). The block
layout [2] §"Draw Engine Contexts":

| Dword | Register | Dword | Register |
|---|---|---|---|
| 0 | CONTEXT_MASK | 11 | DP_BKGD_CLR |
| 1 | Reserved | 12 | DP_FRGD_CLR |
| 2 | DST_OFF_PITCH | 13 | DP_WRITE_MASK |
| 3 | DST_Y_X | 14 | DP_CHAIN_MASK |
| 4 | DST_HEIGHT_WIDTH | 15 | DP_PIX_WIDTH |
| 5 | DST_BRES_ERR | 16 | DP_MIX |
| 6 | DST_BRES_INC | 17 | DP_SRC |
| 7 | DST_BRES_DEC | 18 | CLR_CMP_CLR |
| 8 | SRC_OFF_PITCH | 19 | CLR_CMP_MASK |
| 9 | SRC_Y_X | 1A | CLR_CMP_CNTL |
| A | SRC_HEIGHT1_WIDTH1 | 1B | GUI_TRAJ_CNTL |
| B | SRC_Y_X_START | 1C | CONTEXT_LOAD_CNTL |
| C | SRC_HEIGHT2_WIDTH2 | 1D–3F | Reserved |
| D–10 | PAT_REG0, PAT_REG1, SC_LEFT_RIGHT, SC_TOP_BOTTOM | | |

`CONTEXT_LOAD_CNTL` itself is MM CBh: `CONTEXT_LOAD_PTR` bits 14:0, `CONTEXT_LOAD_CMD` bits
17:16 (0 = no load, 1 = load, 2 = load + rectangular fill, 3 = load + Bresenham line),
`CONTEXT_LOAD_DIS` bit 31 [1] p. 3-15. The command constants appear in the mach64 SDK headers:
`CONTEXT_LOAD 0x10000`, `CONTEXT_LOAD_AND_DO_FILL 0x20000`, `CONTEXT_LOAD_AND_DO_LINE
0x30000`, `CONTEXT_CMD_DISABLE 0x80000000` [11]. `CONTEXT_MASK` (MM C8h) has one bit per
block dword, bit 0 for dword 0; the mask entries for CONTEXT_MASK and CONTEXT_LOAD_CNTL
themselves are ignored and always loaded [1] p. 3-16; [2] §"Draw Engine Contexts".

### 2.11 Reset state

Documented defaults are few, and they are load-bearing: the apertures "should be disabled on
power-up" [2] §"Boot-time Initialization"; `CFG_MEM_AP_SIZE` defaults to 0 [1] p. 3-9;
`CRTC_EXT_DISP_EN` and `CRTC_EN` default to 0 (VGA display, CRTC in reset) [1] p. 3-17;
`MEM_SIZE` defaults to 512K on the GX-0/GX-1 encoding [1] p. 3-67; the interrupt enables of
`CRTC_INT_CNTL` and `BUS_CNTL` default to 0 [1] pp. 3-21, 3-2; the monitor-ID direction
defaults to all-input [1] p. 3-31. The power-on values of the draw-engine latches (mix,
source selects, trajectories, scissors) are not documented anywhere in the evidence set — and
are *reachable* in practice, because the Apple card's accelerator extension relies on context
loads to define them (§4.7) (§6.10).

## 3. Behaviour

### 3.1 Endianness

The mach64 is a little-endian PCI device on a big-endian host: the platform's seam rule —
"byte order on the processor bus is big-endian and byte order on the PCI bus is
little-endian", with the bridge performing the swapping — is
[tnt.md](../../../machines/tnt/tnt.md) §2.5's, and the card sits on the little-endian side of
it. A PowerPC dword read of a register therefore returns the byte-reversed value: a register
bit 23 reads as `$00008000` through a big-endian load (*observed* [9]). Two in-chip
endianness controls exist and are not to be confused with the bus seam:

- `HOST_BIG_ENDIAN_EN` (`HOST_CNTL` bit 0, mirrored in GUI_TRAJ_CNTL): "In 15 bpp and 16 bpp
  modes the bytes within each word are swapped. In 32 bpp mode the order of the four bytes
  within each dword is reversed" — it governs only the HOST_DATA path [1] p. 3-64.
- `MEM_PIX_WIDTH` (MEM_CNTL, GX-2): the "big endian memory aperture pixel width", active only
  "in PCI bus configuration with the CFG_BIG_ENDIAN_EN strap set" [1] p. 3-68 — a host-order
  framebuffer aperture, strap-enabled. Whether the Apple cards strap `CFG_BIG_ENDIAN_EN` is
  not established (§6.11).

### 3.2 The command FIFO and engine status

All writes to draw-engine registers (dword offsets ≥ 40h) go through a "32-bit-wide,
16-entry-deep, command FIFO", consumed in write order; register reads are never FIFOed, and
writes to registers below 40h are not FIFOed [2] §"The Command FIFO". Three contract points:

1. **FIFO discipline is mandatory before every engine-register write.** "Prior to any writes
   to any draw engine register, it is essential to check the state of the command FIFO to
   ensure that enough FIFO entries are available. Failure to do so may cause the draw engine
   to lock." `FIFO_STAT` reports the filled entries; overflow sets the `FIFO_ERR` bit and
   locks the engine, recoverable only through the engine reset in `GEN_TEST_CNTL` [2] §"The
   Command FIFO"; [1] pp. 3-56, 3-57.
2. **FIFOed writes must be 32 bits.** "Since the GUI memory mapped registers are FIFOed during
   a write operation, the write width must be 32 bits" [2] §"Sample Code CHKFIFO.DOC".
3. **Idle is not empty FIFO.** "A state of idleness implies 16 free FIFO entries, but 16 free
   FIFO entries do not imply a state of idleness" — waiting for `GUI_ACTIVE` (GUI_STAT bit 0)
   is the only idle test, and register read-backs of engine-updated registers (DST_X, the
   GUI_STAT scissor bits) require it [2] §"Sample Code CHKIDLE.DOC"; [1] p. 3-61.

`GUI_STAT` also exposes four live clipping observations — DSTX left of / right of the
scissors, DSTY above / below — read-only, meaningful when the engine is idle [1] p. 3-61.

### 3.3 The pixel data path

Per destination pixel the engine runs: a **monochrome source select** (Always_'1', pattern,
host data, blit source) which chooses, per pixel, between the **foreground** and
**background** colour source and mix; the colour source select (background/foreground colour
register, host data, blit source, pattern registers); a 16-function ALU mix; and an optional
colour compare against `CLR_CMP_CLR` — "If the result of the comparison is FALSE, the result
of the ALU is written to the destination; otherwise, the destination data is written to the
destination" [2] §"Source and Destination Mixing Logic". The mix functions [2] §"Source and
Destination Mixing Logic":

| Mix | Function | Mix | Function |
|---|---|---|---|
| 0 | not D | 8 | (not D) or (not S) |
| 1 | 0 | 9 | D or (not S) |
| 2 | 1 | A | (not D) or S |
| 3 | D | B | D or S |
| 4 | not S | C | D and S |
| 5 | D xor S | D | (not D) and S |
| 6 | (not D) xor S | E | D and (not S) |
| 7 | S | F | (not D) and (not S) |
| 17 | (D+S) >> 1 | | (requires DP_CHAIN_MSK) |

"The physical data path is actually 64 bits wide for all color data": eight 8-bpp pixels are
processed simultaneously [2] §"Logical Pixel Data Path". Host data arrives through
`HOST_DATA[0:15]` — one logical register mapped at sixteen addresses to admit string-move
transfers — with pixels consumed least-significant-first for left-to-right rectangular
draws, and `HOST_CNTL`'s `HOST_BYTE_ALIGN` advancing consumption to a byte boundary on Y
advance in 1/4 bpp [1] pp. 3-64–3-65. Two failure contracts: "If a draw operation expects
host data and any other draw engine register is written, the draw operation will panic and
complete the draw operation with a garbage color" (interruptible through BUS_CNTL) [1]
p. 3-65; and host data written when not expected is discarded [1] p. 3-65.

### 3.4 Trajectories

Rectangles and lines are the two destination trajectories; source trajectories are four [2]
§"Trajectories":

| Destination | Registers | Initiator |
|---|---|---|
| Rectangle | DST_OFF_PITCH, DST_X/Y, DST_WIDTH, DST_HEIGHT, DST_X_DIR, DST_Y_DIR | DST_WIDTH, DST_HEIGHT_WIDTH or DST_X_WIDTH |
| Line (Bresenham) | DST_OFF_PITCH, DST_X/Y, DST_BRES_LNTH/ERR/INC/DEC, DST_X_DIR, DST_Y_DIR, DST_Y_MAJOR | DST_BRES_LNTH |

| Source | SRC_CNTL selects | Registers |
|---|---|---|
| Strictly linear | SRC_LINEAR_EN=1 | SRC_OFFSET only |
| Unbounded Y | all three selects 0 | + SRC_PITCH, SRC_X/Y, SRC_WIDTH1 |
| General pattern | SRC_PATT_EN=1 | + SRC_HEIGHT1 (the SRC_Y_END wrap is gated on this bit) |
| General pattern with rotation | SRC_PATT_ROT_EN=1 | + SRC_X/Y_START, SRC_WIDTH2/HEIGHT2 |

The line parameters and step rule: `DST_BRES_ERR = 2·min(|dx|,|dy|) − max(|dx|,|dy|)`,
`DST_BRES_INC = 2·min(...)`, `DST_BRES_DEC = 2·[min − max]`, `DST_BRES_LNTH = max + 1`; on
each step a negative error takes the axial step and adds INC, otherwise the diagonal step and
DEC, with the octant fixed by the three DST_CNTL bits [2] §"Destination Trajectory 2, Line".
The direction contract — the one scrolling code depends on — is stated identically under
source trajectories 2–4: "If the destination trajectory is rectangular, SRC_X_DIR and SRC_Y_DIR
track DST_X_DIR and DST_Y_DIR" [2] §"Trajectories". The rotated-pattern semantics are pinned
by the manual's own wording: `SRC_WIDTH1` is "the horizontal distance (in pixels) from DST_X
to the right edge of a pattern block" for rotated sources, `SRC_WIDTH2` "the width of the
pattern", `SRC_X_START` the "pattern source X start for pattern rotation" [1] pp. 3-93,
3-94, 3-96 — consume WIDTH1 steps, then restart at SRC_X_START and cycle with period
WIDTH2; the manual's worked sample draws a 40×40 pattern into a 160×160 destination with
WIDTH1/HEIGHT1 = 24, WIDTH2/HEIGHT2 = 40 and SRC_X/Y_START = 10, commented "wrap source 4
times in X & Y" [2] §"General Pattern With Rotation".

Side effects after a draw: the source pointer resets to the original SRC_X/SRC_Y; the
destination pointer advances by width/height when the DST_X_TILE/DST_Y_TILE bits are set;
lines leave DST_X/DST_Y on the last pixel, drawn or not per DST_LAST_PEL [2] §"Side Effects".
Pattern alignment is fixed per source (fixed patterns destination-aligned N×N; host and blit
sources source-aligned, rotated within the quadword to align with the destination) [2]
§"Source and Destination Alignment".

### 3.5 Contexts

"The mach64 can save its complete draw engine state to memory. A saved state in memory is a
context." Context blocks live on any 64-dword boundary anywhere in screen memory, "allocated
in reverse order in screen memory: context number 0 resides at the top of screen memory
minus 64 DWORDS, context 1 resides at top of screen memory minus 128 DWORDS, etc." [2]
§"Draw Engine Contexts". Writing `CONTEXT_LOAD_CNTL` with a pointer and a command loads the
block (layout §2.10); bit n of the block's CONTEXT_MASK gates dword n, except CONTEXT_MASK
and CONTEXT_LOAD_CNTL themselves are always loaded, and the block's own CONTEXT_LOAD_CNTL
"must be set to no-op to halt the chain" — otherwise the load chains to the next block,
making a context chain an atomic multi-draw [2] §"Draw Engine Contexts"; [1] p. 3-15. Context
*save* operators are not supported on this generation [2] §"Draw Engine Contexts". The VT
widens the mechanism to "up to 32,768 draw engine contexts" via the 15-bit pointer [3] §2-1.

### 3.6 Monitor sensing

Monitor identity arrives over the connector's three sense lines, driven and read back through
`DAC_CNTL`'s `DAC_MON_ID_DIR` and `DAC_MON_ID_STATE` fields: "the monitor ID is determined by
manipulating the state and direction of three monitor ID pins and reading back the state"
[1] p. 3-31. The Apple GX card's firmware performs the full four-step extended sense
(*observed* in the FCode [4]):

1. Set all three pins to input with the output latches high, and read the primary 3-bit code.
2. Drive each pin low in turn (DIR = 1, 2, 4) and read back the other two, giving three 2-bit
   contributions packed `(pin1,pin0) << 4 | (pin2,pin0) << 2 | (pin2,pin1)` — a 6-bit
   extended code.
3. Restore all-input.

The card aborts — typing "No monitor" and refusing to open the display — **only** when the
primary code is 7 *and* the extended code is $3F: all three pins floating high in all four
configurations, i.e. no cable attached. Any real Apple sense code passes [4]. A second bail,
"No address", fires when Open Firmware assigned no memory BAR [4]. This matches the 9500
developer note's contract: three sense lines at the DB-15, with the extended codes
distinguishing multisync 15/17/20-inch, VGA and 19-inch displays [6] §"Detecting the Monitor
Type" p. 15.

### 3.7 Clocking

The GX's pixel clock is board territory: `CLOCK_CNTL` either selects among an ATI1881x-type
fixed/programmable clock chip's frequencies (CLOCK_SEL/CLOCK_DIV/CLOCK_STROBE) or bit-bangs
a programmable synthesizer's serial line [1] p. 3-4 — on the Apple card the synthesizer is
the RGB514's on-chip PLL, reached through the DAC path (§4.4), and the card's own firmware
never touches CLOCK_CNTL (*observed* [4]; §6.6). The RGB514's PLL is programmed by reference
and VCO divider counts with post-divide DF bits, sixteen F0–F15 programming register pairs
or eight M/N pairs selectable by FS[3:0], enabled by the PLL ENAB bit of its Miscellaneous
Clock Control register [8] §"Clocking". The VT replaces all of this with the internal dual
synthesizer behind CLOCK_CNTL's PLL fields [3] p. 4-38, Appendix B.

### 3.8 Interrupts

Four interrupt sources exist: command FIFO overflow and host data error (both in `BUS_CNTL` —
it "controls FIFO error and host error interrupts"), and the CRTC's vertical blank and
vertical line interrupts [2] §"Boot-time Initialization", §"Interrupts". `CRTC_INT_CNTL`
[1] p. 3-21:

| Bit | Field | Meaning |
|---|---|---|
| 0 | CRTC_VBLANK | R — vertical blank status |
| 1 | CRTC_VBLANK_INT_EN | enable (default 0) |
| 2 | CRTC_VBLANK_INT / CRTC_VBLANK_ACK | R — latched interrupt; W — write-1-to-clear acknowledge |
| 3 | CRTC_VLINE_INT_EN | enable (default 0) |
| 4 | CRTC_VLINE_INT / CRTC_VLINE_ACK | latched line interrupt at CRTC_VLINE, write-1-to-clear |
| 5 | CRTC_VLINE_SYNC | even/odd scan line |
| 6 | CRTC_FRAME | interlaced odd/even frame |

The Apple GX card initializes `CRTC_INT_CNTL` to zero — interrupts off — at boot (§4.3) [4],
and its Mac OS driver then uses the vertical-blank interrupt: enabled and acknowledged
through bit writes to this register (*observed*: the acknowledge write is EN plus ACK, value
$06 to the little-endian register, and the interrupt level at the slot's Grand Central line
clears on the write [9]). The card's PCI interrupt pin reports INTA (§4.1); the slot lines
are collected per slot with INTA–INTD OR-combined into Grand Central's slot sources
([bandit.md](../../../machines/tnt/bandit.md) §3.9;
[grand-central.md](../../../machines/tnt/grand-central.md) §3.2).

### 3.9 The hardware cursor

The mach64 hardware cursor is a 64×64×2-bit sprite: pixel values 00 = cursor colour 0,
01 = cursor colour 1, 10 = transparent, 11 = complement of the underlying pixel; "Cursor
pitch is always 64 pixels ... 16 bytes per line ... regardless of the actual cursor width",
in Intel order (first pixel in the low two bits of the low byte); the colours are palette
indices in pseudo-colour and 24-bit true colour in direct colour [2] §"Hardware Cursor".
Position is `CUR_HORZ_VERT_POSN` (H bits 10:0, V bits 26:16); `CUR_HORZ_VERT_OFF` offsets
into the block for clipped cursors ("offset = 64 − size"), and the cursor is not displayed at
all at negative positions — the driver must saturate to zero and adjust the offset register
instead [2] §"Hardware Cursor". The enable is `GEN_CUR_EN` (GEN_TEST_CNTL bit 7) [1] p. 3-57;
the cursor block location is `CUR_OFFSET`, in 64-bit words [1] p. 3-30. The Apple GX card
never enables this cursor under Mac OS — `GEN_TEST_CNTL` reads $00000300 after a full boot
(engine and block-write enables only, cursor enable clear) and all cursor registers read
zero; Mac OS draws its cursor in software (*observed* [9]).

## 4. Programming model

### 4.1 PCI configuration space

The mach64's PCI identification, as observed on the Apple cards and documented for the VT:

| Field | GX (Apple card) | VT |
|---|---|---|
| Vendor ID | $1002 (*observed* [7]) | $1002, "ATI's assigned PCI vendor ID" [3] §7-1 |
| Device ID | $4758 (*observed* [7]) | $5654 — "ASCII characters 'VT'" — or $4754 ('GT') on the RAGE [3] §7-1 |
| Class code | $030000 display controller (*observed* [5]) | display controller [3] §7-1 |
| Command | I/O and memory access enables, bus master always 0 [3] §7-1 | same |
| BAR | one 32-bit memory BAR, size $01000000 (16 MB) — no I/O BAR at all (*observed* [7]; the GX card publishes only config space plus BAR0 in `reg` [4]) | memory aperture base + block-decoded I/O base [3] §7-3 |
| Interrupt pin | INTA (*observed*: `interrupts 00000001` [7]) | INTA by default, 00h if strapped disabled [3] §7-4 |
| min-grant / max-latency / devsel-speed | 0 / 0 / 1 (*observed* [7]) | — |

The GX's 16 MB BAR covers the 8 MB aperture with the register blocks at its top; a card that
answers the BAR with a smaller aperture would place its registers outside the decoded window
(§6.12). On the VT the ASIC identification (major/foundry/minor revision, e.g. 08h = NEC
VT-A3, 48h = NEC VT-A4, 40h = SGS VT-A4) appears both at PCI configuration offset 08h and in
`CONFIG_CHIP_ID` [3] §7-2.

### 4.2 The expansion ROM and the FCode bring-up

The Apple GX card's expansion ROM is a 32 KB image carrying a standard PCI ROM header, a PCI
data structure (vendor $1002, device $4758, class $030000, code type $01 — Open Firmware,
indicator $80 — last image), the FCode program beginning with token `start1` at image offset
$40, and an embedded PowerPC PEF driver published as the `driver,AAPL,MacOS,PowerPC` property
(*observed* [5] [4]). Two ROM revisions are on record — 113-32900-104 (FCode declared and
consumed at 30,840 bytes, 62 words defined) and 113-32900-101 (30,196 bytes, 68 words) [4].

The card's Open Firmware node is built by that FCode: `name "ATY,mach64"`, `model
"ATY,88800GX"`, `device_type "display"`, `character-set "ISO8859-1"`, `depth` published as
the literal 8, `width`/`height`/`linebytes` computed from the selected mode,
`iso6429-1983-colors`, the identity strings of §1.2, and the ndrv [4]. Technote 1062's live
dump of the sibling board shows the same schema as built by a real 9500's Open Firmware
1.0.5 — including `AAPL,interrupts 00000017`, `AAPL,slot-name`, `fcode-rom-offset 0`,
`width 280` `height 1E0` `depth 8` `linebytes 280` for a 640×480 8-bpp boot,
`assigned-addresses` placing BAR0 at $81000000 non-prefetchable, and a ~$7063-byte
`driver,AAPL,MacOS,PowerPC` with the standard display method set (`open close
draw-rectangle fill-rectangle color@ color! set-colors get-colors draw-logo read-rectangle
restore write read`) [7].

The bring-up, decoded from the card's own FCode [4], runs in this order:

1. Ask the parent bridge (`$call-parent "map-in"`) for a 64 KB I/O window covering the
   card's sparse-I/O addresses — with the memory BAR assigned ($81000000 in the observed
   configuration), the card needs PCI I/O space to reach CONFIG_CNTL, since the GX gives
   that register no memory alias (§2.2).
2. Read configuration dword 0 and compare against $43541002 — a mach64 **CT** (device $4354,
   revision 2). The GX reports $4758xxxx, the compare fails, and a chip-variant flag stays
   clear: the same ROM serves both chips, and the flag only selects the GX or CT column of
   the init table (§4.3). Nothing bails on a GX.
3. Set the command register's I/O-access enable (config dword at +$04, command |= 1) — the
   card turns on its own I/O decode.
4. Read BAR0's assigned base (config +$10) and write `CONFIG_CNTL = (base >> 18) | 2`:
   aperture size 2 = 8 MB, location = base in 4 MB units ($204 → $81000000). From this
   moment the memory-mapped register block at BAR + $7FFC00 answers.
5. Walk the register-init table (§4.3) — all writes 16-bit, to sparse-I/O addresses, half of
   them to base+2 lanes.
6. Run the four-step monitor sense (§3.6), bailing on "No monitor" / "No address".
7. Write $000F to the upper halfword of CONFIG_CNTL — card ID 7 (the strap value for
   "feature disabled": a single-card system) and VGA disable — turning off the never-used
   VGA core.
8. Clear the command register's I/O-access enable (command &= ~1) and map the I/O window
   out. The card boots with memory decode only.

The properties and the depth literal are published around this: Open Firmware has already
chosen 8 bpp as the boot depth, matching the CRTC_PIX_WIDTH = 2 the init table writes [4].

### 4.3 The register-init table

The card's power-on register program, as an (io_address, gx_value, ct_value) table walked with
halfword writes [4] — the GX column is what the Apple card executes:

| I/O addr | GX value | Register | Decoded meaning |
|---|---|---|---|
| $4EEC / $4EEE | $10FF / $5F0E | BUS_CNTL low/high | bus and ROM wait states, the FIFO wait-state ceiling; interrupts disabled (§3.8's defaults) |
| $1AEC | $0000 | CRTC_INT_CNTL | all CRTC interrupts off |
| $1EEC | $0200 | CRTC_GEN_CNTL low | CRTC_PIX_WIDTH = 2 = **8 bpp** |
| $1EEE | $0108 | CRTC_GEN_CNTL high | CRTC_EXT_DISP_EN set, CRTC_EN still clear — extended display selected, CRTC held in reset |
| $66EC / $66EE | $0020 / $0000 | GEN_TEST_CNTL | overscan output enable for the external DAC (bit 5); cursor, engine reset, block write untouched (*inferred* from the bit chart [1] p. 3-57) |
| $52EC / $52EE | $35F2 / $0000 | MEM_CNTL | MEM_SIZE = 2 = **2 MB**; the remainder is the board's memory-latch tuning |
| $62EC | $0100 | DAC_CNTL | DAC_8BIT_EN set — an 8-bit palette/DAC path |
| $72EC | $0000 | CONFIG_STAT0 | read-only register: the write must be accepted and ignored |

In the card's own words: 2 MB of VRAM (the soldered configuration, §1.2), 8-bit DAC, 8-bpp
pixel width, extended display, interrupts disabled. The 4 MB expansion module implies a
different MEM_SIZE value on boards fitted with it — not in the evidence set (§6.7).

### 4.4 The RAMDAC bring-up

With the aperture live, the FCode reaches the RGB514 through `DAC_REGS` crossed with
`DAC_CNTL`'s `DAC_EXT_SEL`: RS[2] (the extended-register select) goes through DAC_CNTL byte
0, RS[1:0] through the four `DAC_REGS` byte cells, reset to 0 afterwards [4]. The RGB514's
I/O map has eight cells — palette address (write), palette data, pixel mask, palette address
(read), index low (cell 4), index high (cell 5), index data (cell 6), index control — the last
four reaching its 11-bit indexed register file [8] §"I/O Addresses". The card's indexed
bring-up writes [4], all matching the RGB514 register set [8]:

| Value | Index | RGB514 register |
|---|---|---|
| 1 | $70 | Miscellaneous Control 1 — 64-bit VRAM interface |
| 1 | $10 | PLL Control 1 |
| 0 | $90 | VRAM Mask Low |
| 0 | $91 | VRAM Mask High |
| 0 | $05 | Power Management — all powered up |
| 1 | $02 | Miscellaneous Clock Control — PLL programming enabled |
| $45 | $71 | Miscellaneous Control 2 — internal PLL, 8-bit colour, VRAM pixel port |
| 3 | $0A | Pixel Format — **8 BPP** |
| 2 | $04 | Horizontal Sync Position |

followed by a write of $FF to the Pixel Mask cell (RS = 010) — no masking. The palette
itself is loaded later by the OS driver through the same palette cells (§4.8).

### 4.5 CRTC mode tables and the published mode

The FCode carries a per-monitor case statement of CRTC timing tuples [4]. The 640×480 arm,
with the field layout of §2.5:

```
CRTC_H_TOTAL_DISP = $004F0067   H_DISP  = $4F (79 -> 80 chars = 640 px)
                                H_TOTAL = $67 (103 -> 104 chars = 832)
CRTC_V_TOTAL_DISP = $01DF020C   V_DISP  = $1DF (479 -> 480 lines)
                                V_TOTAL = $20C (524 -> 525 lines)
```

exactly the geometry Technote 1062 reports from the live card (`width 280`, `height 1E0`,
`linebytes 280`) [7]. The arms for the other monitor sense codes are present but not
transcribed in the evidence set (§6.13).

### 4.6 The Mac OS driver

The ROM's `driver,AAPL,MacOS,PowerPC` property is a PowerPC PEF — embedded in the FCode token
stream as a long `encode-bytes` chain over string literals, not as a contiguous blob —
reassembling to a driver whose loader section imports exactly four libraries:
`NameRegistryLib` (10 symbols), `DriverServicesLib` (14), `PCILib` (2), `VideoServicesLib`
(3), exporting two symbols [4]. No QuickDraw, no acceleration library: it is a pure display
driver — mode setting, the Name Registry properties, PCI config access and the VBL interrupt
service; it does not blit [4]. The 9500 developer note's description — the card's firmware
"contains a video device driver that supports the initialization required during system
startup ... the video driver provides the same function calls and has the same capabilities
as the NuBus video driver" [6] §"Video Display Card" p. 14 — matches: this is the Mac OS
display driver for the card, and the drawing acceleration comes from elsewhere (§4.7).

### 4.7 The ATI Graphics Accelerator

Mac OS 7.5.5 and later ship the **ATI Graphics Accelerator** (an INIT plus the ATI Graphics
Driver ndrv stack) for the `ATY,mach64` family, with the retail universal installers and the
Xclaim GA 1.2 update covering the same silicon [5]; the archive index records that the ATI
Displays control panel does *not* support the Spinnaker GX [5]. On a stock install the
extension — not the ROM driver — drives the draw engine, and hard: recorded boots show 3,467
engine register writes inside a 200-million-instruction window [9]. The observed engine
contract [9]:

- `DP_MIX` ($00070007: both mixes = S) and `DP_SRC` ($00010100) are written **only at
  initialisation**, as a register test (pattern, zero, pattern, zero — ending on zero). All
  later data-path state arrives through context loads: 941 `CONTEXT_LOAD_CNTL` writes in
  one boot, every one carrying command 3 (load + line), over exactly **two** context blocks
  whose masks load `DP_WRITE_MASK`/`DP_MIX`/`DP_SRC`/`GUI_TRAJ_CNTL` ($08C80000; 920 uses)
  and `SC_LEFT_RIGHT`/`SC_TOP_BOTTOM` ($00018000; 21 uses).
- The blocks sit in VRAM at the addresses the reverse-order pointer arithmetic of §3.5
  predicts, carrying the init-time `DP_MIX` value at dword $16 — the context mechanism is
  how the accelerator keeps its two-colour desktop pattern alive.
- Because the masks supply no trajectory operands, a context load with command 3 but no
  operand dwords draws nothing meaningful — the engine loads state and, with no line
  operands in the block, produces at most stale-coordinate artefacts. The Macintosh driver's
  usage *depends* on the load-with-line command being inert without operands (*observed*:
  the desktop paints correctly only when loads are masked to the supplied dwords [9]).
- The desktop itself is painted by **pattern blits with rotation** (source trajectory 4):
  an 8×8 tile cached offscreen, entered at the phase the destination needs —
  `SRC_X_START`/`SRC_Y_START` = (128,480), `SRC_WIDTH2`/`HEIGHT2` = 8×8, with the source
  window re-anchored per operation so the dither phase lines up (§3.4) [9].
- `GEN_TEST_CNTL` settles at $00000300 — draw engine enabled and **VRAM block write**
  enabled, cursor off (§3.9) [9].

### 4.8 Observed run-time behaviour

Recorded boots of the card in a Power Macintosh 9500 under Mac OS 7.6 [9], the facts a
re-implementation must reproduce:

- The card boots at **8 bpp**, 640×480 — the FCode's published depth and CRTC geometry
  (§4.2, §4.5) — and the first image on the monitor is drawn by the card's own FCode boot
  driver, there being no built-in display to initialize ([pm9500.md](../../../machines/tnt/pm9500.md) §5).
- The vertical-blank interrupt asserts once per frame on the card's slot line at Grand
  Central, level-sensitive; the driver acknowledges through the `CRTC_INT_CNTL`
  enable-plus-acknowledge write, and the level clears on the write (§3.8).
- The desktop dither is drawn with two palette indices — the Apple 8-bit CLUT's neutral and
  grey-ramp entries — and the *values* on screen depend entirely on the CLUT/gamma the
  driver loads into the RGB514: the card's ndrv writes a different gamma table than the
  platform's built-in video path, so the same logical colours land on different DAC values.
  The RGB514 palette is 8-bit and the load is a straight byte copy (*observed*; the CLUT
  contents are driver territory, not silicon [9]).
- Scrolling correctness depends on the direction contract of §3.4: a blit that overlaps in
  the direction of travel must walk from the far edge (DST_X_DIR/DST_Y_DIR clear), and the
  source direction follows the destination's — a register-level overlapping-copy
  reproduction, in both directions, byte-exact [9].

## 5. Quirks & errata

- **CONFIG_CNTL is I/O-only on the GX.** "All except CONFIG_CNTL have memory mapped register
  aliases" [1] Ch. 1 — and the apertures are off at power-up [2] §"Boot-time Initialization".
  Sparse I/O is the only bootstrap path, and the card must therefore decode fixed legacy I/O
  addresses without ever having been assigned an I/O BAR (§4.2).
- **The card enables — then disables — its own PCI I/O decode.** The FCode sets the command
  register's I/O-enable, does its sparse-I/O work, and clears the bit again (§4.2) [4]. A
  device that latches the I/O-enable but ignores the clear, or vice versa, breaks the boot.
- **Read-only registers get written.** The firmware writes CONFIG_STAT0's I/O alias and the
  part must accept and discard the write (§4.3) [4]; [1] p. 3-12 says read-only.
- **The GX/CT flag is not a gate.** The $43541002 compare in the FCode only selects the init
  table's column; a GX fails the compare and proceeds (§4.2) [4].
- **FIFO overflow locks the engine.** Writing past 16 unacknowledged entries sets FIFO_ERR
  and locks; only the GEN_TEST_CNTL engine reset clears it (§3.2) [2].
- **FIFOed writes must be dwords** — but the boot-time sparse-I/O writes are **halfwords**,
  at base, base+2 and base+3 byte lanes (§4.2, §4.3) [4]. Both access widths must decode.
- **GUI_TRAJ_CNTL is an alias, not a separate register.** Its low 16 bits *are* DST_CNTL and
  bits 20:16 *are* SRC_CNTL [1] p. 3-62: writes to either name land in the same flip-flops.
  The Macintosh accelerator writes only the composite [9] — a model with separate state
  misses every draw.
- **Combined registers put the first-named field in the low halfword.** `DST_Y_X` is
  `(X << 16) | Y`, `DST_HEIGHT_WIDTH` is `(WIDTH << 16) | HEIGHT` (§2.9) — the transposed
  reading is self-consistent on most traffic and geometrically wrong.
- **Contexts live at the top of VRAM, counted backwards.** Address =
  `(vram_size − (ptr + 1) × 256) mod vram_size`; the block layout and mask semantics of
  §2.10 apply, with the mask entries for the two context registers ignored [2] §"Draw
  Engine Contexts"; [9].
- **A context load with a draw command but no operand dwords draws garbage at best.** The
  load-with-line command executes the line engine from whatever the registers hold; the
  Macintosh driver relies on masked loads without operands being inert (§4.7) [9].
- **The source wrap gate.** "`SRC_Y_END` will be used only if this bit [`SRC_PATT_EN`] is
  enabled" — the source window wraps only for pattern sources; a blit that wraps
  unconditionally tiles whatever the pattern registers last held [1] p. 3-86.
- **Source direction tracks destination direction** for rectangular trajectories [2]
  §"Trajectories" — the rule that makes overlapping scrolls correct; its violation produces
  per-column garbage (§4.8) [9].
- **Host-data panic.** Writing any engine register while a draw expects host data "will
  panic and complete the draw operation with a garbage color" [1] p. 3-65.
- **The VT does not bus-master.** The VT's PCI configuration space marks Bus Master Enable
  "Always 0, disabled" [3] §7-1; all data is host-pushed. (The GX's command register is not
  documented field-by-field in [1] — §6.12.)
- **VGA apertures do not exist on PCI.** "VGA apertures are not supported if CFG_BUS_TYPE =
  PCI. A 4M or 8M linear aperture must be used for PCI bus implementation" [1] p. 3-70 —
  MEM_VGA_WP/RP_SEL are dead weight on a Mac card.
- **The VT is not a GX plus extras.** The register space moves into two blocks (§2.1), the
  aperture is fixed 8 MB and CONFIG_CNTL's aperture fields go read-only in PCI systems, the
  internal DAC/clock replace the discrete parts, and the GX-era ATI extended VGA registers
  at 1CEh/1CFh are gone [3] §1-1, §2-1.
- **The card publishes depth 8 literally.** The `depth` property is the constant $00000008,
  not a computed value, and the init table's CRTC_PIX_WIDTH = 2 agrees (§4.2, §4.3) [4].

## 6. Open questions

1. **GX silicon internals.** ATI's chip-level specification for the 88800GX (GCS-C012001-00)
   is not publicly available [5]; memory-interface timings, FIFO tolerances, and the
   undocumented draw-engine power-on values of §2.11 are therefore unestablished.
2. **109-32900 versus 109-33200.** Which board shipped with which machine, why the ROM's
   published name differs (`ATY,mach64` versus `ATY,XCLAIM`), and whether the HD-15
   connector is the only hardware delta between the two board numbers (§1.2).
3. **The ROM revision pair.** What changed between 113-32900-101 (68 words, zeroed identity
   strings) and -104 (62 words, real part numbers), and which shipped on which boards
   (§4.2) [4] [5].
4. **The OF I/O window arithmetic.** The FCode's `map-in` call to the parent bridge and the
   64 KB region it yields are observed as literals [4], but the bridge-side mapping of PCI
   sparse I/O on the Bandit platforms is not established here — it belongs to
   [pci.md](../pci.md) and [bandit.md](../../../machines/tnt/bandit.md) §3, and no document
   in the evidence set states the I/O window's placement.
5. **The VT card beyond its header.** The 262VT ROM is verified only at the PCI data
   structure level (vendor $1002, device $5654) [5]; its FCode bring-up, monitor handling
   and Mac OS driver surface are undecoded, as are the board's VRAM and connectors (§1.2).
6. **CLOCK_CNTL on the GX card.** The firmware never writes it [4] — the RGB514 PLL is
   programmed through the DAC path instead — so whether the GX board carries a second,
   CLOCK_CNTL-driven clock chip (ICS2595 and ATI18818 appear in the board record [5]) and
   who programs it is open.
7. **The 4 MB expansion module.** The MEM_SIZE value, the boundary setting and the CRTC
   modes the 109-31600-00 module enables are not in the evidence set (§4.3) [6].
8. **DAC variants on Apple GX boards.** The board record shows an IBM RGB514 on the
   photographed board [5], but the GX's strap list names many DACs [1] p. 3-12 and the
   driver-archive record suggests other assemblies carried other parts — which DAC is on
   which board number is unconfirmed.
9. **The hardware cursor under Mac OS.** No observed Macintosh driver enables it (§3.9)
   [9]; whether any shipping ATI Mac driver or utility ever does is unknown.
10. **Draw-engine reset values.** The power-on state of the mix/source/trajectory latches is
    undocumented (§2.11), and the Macintosh accelerator deliberately never depends on it
    (§4.7); a driver that read them before its first context load would be relying on
    unknown state.
11. **The big-endian aperture strap.** `MEM_PIX_WIDTH` and the `CFG_BIG_ENDIAN_EN` strap
    (§3.1) exist on GX-2 silicon [1] p. 3-68; whether Apple's boards strap it, and what a
    strapped card's framebuffer looks like to a big-endian host, is untested.
12. **GX PCI configuration space, field by field.** The VT's Chapter 7 documents the
    configuration space explicitly [3] Ch. 7; for the GX the evidence is the observed
    `reg`/`assigned-addresses` of [4] [7] plus the FCode's config accesses — cache line
    size, latency timer, and whether Bus Master reads 0 as on the VT (§5) are unverified on
    the GX.
13. **The other monitor arms.** The FCode's per-monitor CRTC case statement beyond the
    640×480 arm is present in the token stream but not transcribed (§4.5) [4] — the card's
    full mode list (the 9500 note's multisync/VGA/19-inch set [6] p. 15) cannot be
    tabulated from the evidence.
14. **The onboard Rage II's exact configuration.** The 5500/6500 developer note is present
    but not in extractable form [10]; the onboard GT-B's VRAM, sense wiring and device-tree
    node (`ATY,264GT-B`) are known from the register family and the card record, not from
    Apple text (§1.3).
15. **Direct modes on the GX card.** The GX's 15/16/24/32 bpp support is register-level fact
    (§2.5) and the RGB514 is a direct-colour DAC [8], but no observed Mac OS session
    exercises a direct-colour mode on the Apple GX card — the boot path is 8 bpp throughout
    (§4.2).

## References

1. ATI Technologies Inc., *mach64 Register Reference Guide*, P/N RRG-S00700-05, Release 5.0,
   October 1994 — Chapter 1 "Register Classifications" (register mapping: apertures, register
   block offsets, sparse-I/O base); Chapter 2 "Cross Reference" (the full
   class/mnemonic/I/O-select/dword-offset tables); Chapter 3 "Register Reference"
   pp. 3-1–3-109 (per-register bit charts and usage, cited by page in the prose).
2. ATI Technologies Inc., *mach64 Accelerator Programmer's Guide*, P/N PRG-S00700-05, second
   draft, 1994 — Chapter 2 "Programming Model" (mach64 detection; mode switching; the
   linear and paged memory apertures; register mapping; the command FIFO; the logical pixel
   data path; trajectories; mixing logic; draw engine contexts; the hardware cursor; draw
   operations); Chapter 3 "Simple Draw Operations" (bitblt, patterns, line draw, monochrome
   expansion, scissoring); Chapter 5 "Advanced Topics II" (boot-time initialization; DAC
   programming; diagnostic features).
3. ATI Technologies Inc., *mach64 Register Reference Guide: ATI-264VT and 3D RAGE*, P/N
   RRG-G02700 Rev. 0.10, preliminary draft, July 1996 — Chapter 1 (VT and 3D RAGE features,
   register classes); Chapter 2 (aperture modes, register blocks 0/1, sparse and block I/O
   mapping, non-Intel platforms); Chapter 3 (cross reference); Chapter 4 (accelerator
   register reference, cited by page); Chapter 5 (multimedia registers); Chapter 7 (PCI
   configuration space registers); Appendix B (programming the internal PLL).
4. Apple Accelerated PCI Graphics Card (mach64 GX) expansion ROM — the FCode detokenization
   and annotated decode of ROM revisions 113-32900-104 and 113-32900-101 (byte-exact token
   walks; the published property list, the bring-up sequence, the register-init table, the
   monitor-sense program and bail-outs, the CRTC mode case statement, the RAMDAC indexed
   writes, and the extraction of the `driver,AAPL,MacOS,PowerPC` ndrv with its
   loader-section import tables).
5. The mach64 Macintosh card physical record — EPROM dumps and board photographs of the
   Apple GX boards (109-32900 and 109-33200; bitsavers 109-32900-10 archive), the mach64 VT
   card ROM `262VT V1.0B14` (board 109-36300-00), the Xclaim 3D board ROM `M64_3D_1.F10`
   (109-39200-00), the ATI mach64 SDK headers (m64sdkr3, CHAP5), and the Macintosh ATI
   driver-archive record (the OS-bundled extension sets, the universal retail installers,
   the Xclaim GA 1.2 update and the installer/card index documenting the "Power Macintosh
   9500 display card – Apple Mach64 GX (Spinnaker)" family). PCI data structures cited
   from these images: vendor $1002; devices $4758 (GX), $5654 (VT), $4754 (GT); class
   $030000; code type $01; indicator $80.
6. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers*, Developer
   Press, 1995 — §"Video Display Card" pp. 13–14 (the card, its VRAM, its boot ROM and
   driver), §"VRAM and Screen Sizes" p. 13, §"Monitor Connector" p. 14, §"Detecting the
   Monitor Type" p. 15.
7. Apple Computer, Inc., *Apple Technote 1062, "Fundamentals of Open Firmware, Part II:
   The Device Tree"*, September 1996 — §"A Look at the ATI Card: Its Properties &
   Methods": the complete `.properties`/`words` dump of a mach64 GX card (name
   `ATY,XCLAIM`, model `ATY,88800GX`, vendor/device/revision IDs, `width`/`height`/`depth`/
   `linebytes`, `reg`/`assigned-addresses` with the single 16 MB memory BAR at $81000000,
   the embedded `driver,AAPL,MacOS,PowerPC` and the display method set).
8. IBM Microelectronics, *RGB514 High-Performance Palette DAC* product description
   (preliminary data), 1994 — product highlights (220 MHz, 64-bit pixel path, 4–32 bpp
   formats, on-chip video clock generation); §"I/O Addresses" Table 1 (the eight RS[2:0]
   cells); §"Clocking" (PLL reference/VCO dividers, F0–F15 and M/N programming, PLL ENAB);
   the indexed control register set (Miscellaneous Control 1–3, PLL Control 1–2, Pixel
   Format, Power Management, VRAM Mask, Horizontal Sync Position).
9. Recorded guest behaviour of the Apple mach64 GX card — observation logs of Mac OS 7.6
   boots on the Power Macintosh 9500: the engine register traffic (the init-time DP_MIX/
   DP_SRC register test, the 941 context loads over two masked blocks, the pattern-rotation
   desktop tiles), `GEN_TEST_CNTL` reading $00000300 with the cursor registers zero, the VBL
   acknowledge sequence at `CRTC_INT_CNTL` and its Grand Central slot line, the CLUT/gamma
   difference against built-in video, and the register-level overlapping-scroll reproduction
   in both directions.
10. Apple Computer, Inc., *Power Macintosh 5500/6500 Developer Note*, 1997 — the onboard
    ATI 3D Rage II (mach64 GT-B) video subsystem of the Gazelle platform (present in the
    evidence corpus as a page-image PDF; cited for the part's presence, not page-level
    detail).
11. ATI Technologies Inc., *mach64 Software Development Kit* (m64sdkr3), headers
    `CHAP5/DEFINES.H` and `CHAP5/ATIM64.H` — the context command constants and the
    CONTEXT_MASK/CONTEXT_LOAD_CNTL offsets, confirming the encoding of [1] p. 3-15.
