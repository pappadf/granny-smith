# ATI 3D Rage II and Rage II+ DVD

**Contents:**

1. [Overview](#1-overview) — the mach64GT generation and its lineage; what the "DVD" in the name covers; the
   Macintosh cards; the host machines and onboard fits; the evidence base
2. [Register file](#2-register-file) — apertures and the I/O views; the register map by class; configuration,
   CRTC, DAC and clock/PLL registers; memory control; the draw engine and its GT deltas; the scaler pipe
   and 3D registers; the multimedia block; the bus-master registers; reset state
3. [Behaviour](#3-behaviour) — endianness; the command FIFO; the pixel data path; trajectories; the
   front-end scaler; overlay, capture and the VMC port; monitor sensing and DDC; clocking; interrupts;
   the hardware cursor
4. [Programming model](#4-programming-model) — PCI identity; the expansion ROM and FCode bring-up; the
   published properties; the embedded Mac OS driver; mode setting; the Mac OS software stack; the 3D and
   video paths under Mac OS
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the parts are

The **3D Rage II** is a member of ATI's *mach64GT* generation — the 3D-equipped branch of the mach64CT
family. In ATI's own taxonomy, "the mach64CT Family encompasses the mach64CT (ATI264CT), mach64VT
(ATI264VT) and mach64GT (3D RAGE) variants", and the family's defining characteristics are the
integrated DAC, the integrated clock synthesizer, the absence of VRAM support, and a "pure" VGA
controller that "is not independently programmable from the accelerator controller" [2] §1.3. The
mach64GT group within that family spans "3D RAGE, RAGE II, II+, IIC, RAGE PRO" [2] §1.3.2: the first
3D RAGE (the **GT-A2** silicon the 1996 register reference documents), the **Rage II** (**GT-B**),
the **Rage II+** (**GTB**), the IIC and the Pro. The generation is therefore the middle one of the
three ATI lineages this tree covers: it descends from the mach64 GX/VT cards
([mach64.md](mach64.md) §1.1) and is succeeded by the 128-bit Rage 128
([rage-128.md](rage-128.md) §1.1), whose Concurrent Command Engine and dual engine/memory
architecture abandon the GT register file entirely.

The mach64GT "introduces hardware support for 3D operations" and "includes all mach64VT features
with the addition of hardware 3D acceleration and improved video filtering" [2] §1.3.2. The 1996
register reference, which documents the VT and the 3D RAGE (GT-A2) as one register-compatible pair,
lists the 3D additions: a trapezoidal trajectory, six texture filtering modes, perspectively correct
texture mapping, video textures, Gouraud shading, horizontal and vertical scaling, alpha blending,
fog effects, double buffering, dithering of limited colours (8 and 16 bpp), and the new pixel
formats RGB332, RGB444, YUV422, YUV444 and Y8 [1] §1-1. The VT's own additions — the integrated
true-colour palette DAC, the internal dual clock synthesizer, the video scaler, colour space
conversion and the overlay/capture machinery — carry over unchanged ([mach64.md](mach64.md) §1.1;
[1] Ch. 5).

The generation's membership, as the programmer's guide identifies it:

| Part | Generation name | PCI device ID | Notes |
|---|---|---|---|
| GT (3D RAGE, GT-A2) | 3D RAGE | 0x4754 ('GT') | the 1996 register reference's subject [1]; no bus mastering — the II/II+ list omits it [2] §8.10.1 |
| GT-B | Rage II | 0x4754 ('GT') | distinguished from GT-A2 by ASIC identification, not device ID [2] Table A-6 |
| GTB | Rage II+ | 0x4755 | SGRAM block write; bus mastering [2] Table 3-1, Table 7-1 |
| — | Rage IIC (PQFP PCI) | 0x4756 | plus AGP packages 0x475A/0x4757 [2] Table 3-1 |
| — | Rage Pro | 0x4742/0x4744/0x4749/0x4750/0x4751 | superset generation, out of this page's scope [2] Table 3-1 |

Two identification traps are visible in the table. First, **the GT device ID covers more than one
silicon generation**: 0x4754 is the power-up default for the 3D RAGE *and* remains the ID the
Rage II answers, so a driver must read the ASIC identification — the revision byte at PCI
configuration offset 08h, or the query-structure ASIC type (GT = 0x4700, GT-A2/NEC = 0x4701,
GT-B/SGS = 0x473A, GT-B/UMC = 0x475C) — to tell them apart [1] §7-2; [2] Table A-6. Second, the
generation-to-generation *hardware* deltas are tabulated by the programmer's guide, not by a register
reference: the source FIFO grows from 8x32 (VT, 3D RAGE) to 16x32 (Rage II) to 32x32 (Rage II+),
the maximum memory speed from 66 MHz to 83 MHz at the same step, the memory page-hit cost drops
from 2 to 1 cycle at the II+, and SGRAM block write appears with the II+ ("no" for VT, 3D RAGE and
Rage II) [2] Table 7-1.

### 1.2 "DVD" in the name: what the part does and does not decode

Apple's documentation calls the onboard part of the original Power Macintosh G3 the "**ATI 3D RAGE
II+DVD**" — "the graphics controller used in the original Power Macintosh G3 computer", one of the
board's five core ICs [4]. The suffix needs an honest reading, because no ATI document in the
evidence set defines a "II+DVD" silicon variant, and the decode assistance the name implies is
narrower than the name suggests:

- What the generation *does* have is the mach64VT-derived **video pipeline**: a front-end scaler with
  bilinear hardware interpolation, colour space conversion, and the overlay window machinery
  (§3.5, §3.6) — the features behind Apple's claim of "hardware acceleration of video for full
  screen, full motion, TV-quality playback of Cinepak and MPEG1 QuickTime movies" [4] and the 6500
  launch's "QuickTime and QuickTime MPEG acceleration for full-screen animation playback" [5].
- The scale/3D pipe carries one bit explicitly aimed at MPEG work: `SIGNED_DST_CLAMP` in
  `SCALE_3D_CNTL`, documented as "this mode is used for MPEG motion compensation" [1] p. 6-5 — the
  destination is assumed signed during alpha blending so that prediction residuals accumulate
  correctly.
- What the generation does *not* have is an MPEG-2 decoder: in ATI's own account, motion
  compensation is "integrated (RAGE PRO only)" [2] §1.3.2 and the iDCT arrives only with the RAGE
  MOBILITY [2] §1.3.5. On the beige G3, watching DVD movies "requires the Apple DVD-Video and
  Audio/Video Card and a DVD-ROM drive" — the *card* "incorporates ... additional circuitry to
  support the playback of DVD movies", not the Rage II+DVD [4].

The "DVD" is therefore the scaler/overlay assistance path for software-decoded MPEG (the scaler can
scale "software MPEG or AVI video data written from the host" [1] §5 "Video Input and Output
Formats"), plus a name for the market position — a reading that matches the part being marketed as
a DVD-capable multimedia controller while the actual DVD decode lives on a companion card. Whether
"II+DVD" denotes a distinct silicon stepping or a board feature set is an open question (§6.15).

### 1.3 The cards on the Macintosh

| Card | Silicon / PCI ID | Memory | Expansion ROM | Notes |
|---|---|---|---|---|
| **ATI Xclaim 3D** | mach64 GT-B; device 1002:4754 (*observed* in the ROM's PCI data structure [3]); OF model `ATY,GT-B` (*observed* [3]) | base size not published by the FCode; two upgrade-module part numbers published (*observed* [3]) | 113-39203-124, FCode `APL-1.0b42`, board 109-39200-00 (*observed* [3]) | retail 3D card; software record from update 2.01 to 3.1 [7] |
| **ATI Xclaim VR** | same silicon family (*inferred — unverified*: no ROM held) | not established | not held (§6.8) | adds video input/output; "for use with the XCLAIM VR and XCLAIM TV" per its software update [7] |
| Apple-branded 3D Rage II card | not established in the evidence set | not established | not established (§6.4) | the Apple-labelled member of the card family; no primary document in the evidence set identifies it |

The evidence for the retail cards is one physical ROM plus the software record: the Xclaim 3D's
64 KB expansion ROM (image `M64_3D_1.F10`, board 109-39200-00, with board photographs) [3], and the
ATI driver-archive readmes for the Xclaim 3D (software updates 2.01, 2.1 of December 9, 1997 and
3.1 of March 19, 1998, each "for use with the XCLAIM 3D only"), the Xclaim VR (updates 2.01, 2.1,
3.0 of December 11, 1997 and 3.1, "for use with the XCLAIM VR and XCLAIM TV"), and the **ATI MPEG
Accelerator 2.0** of June 6, 1997, which is "for use with the XCLAIM VR and XCLAIM 3D" and installs
the ATI MPEG Accelerator extension [7]. The Xclaim 3D's ROM publishes the two memory upgrade
modules as separate properties — `ATY,Mem# (Upgrade to 6MB) = "100-401015"` and
`ATY,Mem# (Upgrade to 8MB) = "100-401017"` — and publishes no base-memory part number (*observed*
[3]); the base memory size of the retail card is therefore not established by its own firmware
(§6.4).

### 1.4 Host machines and the onboard fits

A Rage II card is a standard PCI display device: any slot of the PCI Power Macintosh generation
serves ([pci.md](../pci.md) §1.2), and the observed software record confirms the intended
population — the Xclaim 3D update notes an Apple 7200 interaction (a sound-corruption workaround
requiring the ATI Sound Catalyst extension [7]), and the 3D Rage II generation's retail software
sits alongside the mach64-era installers in the same archive record [7]. Everything slot-side is
[pci.md](../pci.md)'s: the configuration-space architecture (§2.4-§2.5), the command-register
policy (§2.6), byte order across the bridge (§3.4), the OR-combined slot interrupt (§3.5) and the
Open Firmware startup process that runs the card's FCode (§4.1-§4.4); the host bridges are
[bandit.md](../../../machines/tnt/bandit.md)'s, the platform [tnt.md](../../../machines/tnt/tnt.md)'s.

The **onboard fits** are where the Rage II generation matters most on the Macintosh:

- **Gazelle (Power Macintosh/Performa 5500 and 6500, 1997).** Apple's 6500 announcement credits the
  machines with "the ATI RAGE II graphics accelerator", bringing "QuickTime and QuickTime MPEG
  acceleration for full-screen animation playback ... QuickDraw and QuickDraw 3D acceleration ...
  and accelerated video capture for video editing" [5]. The Power Macintosh 5500/6500 developer
  note documents the subsystem in detail but is present in the evidence corpus only as a page-image
  PDF (§6.13) [6]. The onboard part's Open Firmware node is `/bandit/ATY,264GT-B` — the on-board
  video `output-device` of the Performa 5500/6500 [8].
- **Gossamer (Power Macintosh G3, "beige", 1997-98).** The original G3's logic boards (ROM subversion
  V4.0 F2) carry the "ATI 3D RAGE II+DVD 64-bit graphics and multimedia accelerator" as one of five
  core ICs; later boards (V4.5 F1/F2) move to the Rage Pro [4]. The onboard fit is 2 MB of 100 MHz
  SGRAM on a 64-bit bus, expandable to 4 MB or 6 MB by a 144-pin SGRAM SO-DIMM, with the extra
  memory above the display's needs available for 3D textures [4].

The beige G3's developer note also bounds the Rage II+DVD's place in that machine's video offering:
the optional "128-Bit 2D/3D Professional Graphics Card" (8 MB, 240 MHz RAMDAC) is a separate product
whose specifications state "video scaling and color space conversion [are] handled in software"
[9] — the opposite contract from the Rage II+DVD's hardware scaler, and a card whose silicon is not
identified in the evidence set.

### 1.5 Evidence base

The page is built from primary evidence, but the two halves of the evidence are uneven, and the
register-grade half is one generation off. The **vendor corpus**: the mach64 register reference for
the ATI-264VT and 3D RAGE (RRG-G02700 Rev. 0.10, July 1996) — the last ATI register reference that
documents the mach64GT register file, covering the GT-A2 3D RAGE [1]; and the RAGE PRO and
Derivatives Programmer's Guide (Rev 1.0, 2000), which supersedes the mach64 Programmer's Guide and
carries the Rage II/II+ generation's facts: the family history, the non-Intel memory map, the device
IDs, the chip-characteristics table, the front-end scaler and bus-master programming sequences, and
the BIOS query structure's ASIC-type list [2]. There is **no chip-level specification for the Rage
II (GT-B) itself** — the GCS-C02700 3D RAGE specification covers GT-A2, and a GT-B-specific document
is not in the evidence set (§6.1) — so every register statement here is made at the GT-A2 level and
annotated with the generation deltas the programmer's guide records. The **Macintosh record**: one
64 KB Xclaim 3D expansion ROM, detokenized end to end (its PCI data structure, published properties,
FCode program structure, and embedded PowerPC driver with its loader tables and function-name
table) [3]; the board photographs of the same card [3]; the ATI driver-archive readmes and installer
sets [7]; Apple's Power Macintosh G3 developer note [4]; the 6500 press release [5]; the 5500/6500
developer note as a page-image PDF [6]; and the NetBSD/macppc model-support list for the onboard
node path [8]. What is *not* in the evidence set: any configuration-space dump of a live card, any
recorded run-time observation of a Mac boot (the mach64 GX card has such a log —
[mach64.md](mach64.md) §4.8 — the Rage II cards do not), any Xclaim VR ROM, any decode of the
Apple-branded card, and the VT/GT-era CONFIG_CHIP_ID and CONFIG_STAT0 bit charts, which did not
survive text extraction of [1] (§6.2). The honest consequence is a page whose register file is dense
at the documented GT-A2 level, whose Macintosh programming model rests on one retail card's
firmware, and whose Open questions section is correspondingly long.

## 2. Register file

### 2.1 Address spaces and apertures

The programming model is fully memory-mapped; the registers are memory-mapped, I/O-mapped, or both.
In general the VGA registers are I/O-mapped only, the draw engine, 3D engine and multimedia
registers are memory-mapped only, and the rest are I/O-mapped with memory aliases; all registers
are 32 bits wide except `DAC_REGS`, which is 4x8 bits [1] §2-1.

- **Aperture modes.** The part operates in VGA aperture or linear aperture mode. The linear aperture
  mode "is optimized for PCI configurations" [1] §2-1 and is the only mode a Macintosh card uses.
- **The linear aperture is fixed at 8 MB** on this generation, and "the aperture size (CFG_MEM_AP_SIZE)
  is always set to 2x8 MB, and the location (CFG_MEM_AP_LOC) is fixed by the PCI configuration space"
  — both fields of `CONFIG_CNTL` are read-only in PCI systems [1] p. 4-16. This is the VT generation's
  break from the GX's firmware-programmed apertures ([mach64.md](mach64.md) §2.1): on a GT the
  firmware never sizes the aperture; PCI does.
- **Two register blocks at the top of the aperture.** The upper 1 KB of the aperture is **block 0**
  (byte offset $7FFC00); a second 1 KB **block 1** sits just below it at $7FF800 [1] §2-1. Register
  addresses use the notation `MM:block_offset` — `MM:1_09` is block 1, dword offset 9h, byte address
  24h within the block; the worked example resolves it to aperture base + $7FF824 [1] §2-3. The
  Xclaim 3D's FCode carries the literal $7FFC00 — the block-0 base inside its 8 MB aperture
  (*observed* [3]).

The Xclaim 3D's FCode publishes a single 32-bit memory BAR of 16 MB ($01000000) in its `reg`
property — the same envelope as the mach64 GX card ([mach64.md](mach64.md) §4.1): a 16 MB decode
covering the 8 MB aperture with the register blocks at its top (*observed* [3]).

### 2.2 The I/O views: sparse and block

On PCI there are two I/O mappings, and both are supported by the VT and 3D RAGE [1] §2-3:

- **Sparse I/O** (ISA style): `absolute I/O address = (I/O select << 10) + I/O base`, where the I/O
  base is "usually 2ECh", with 1CCh and 1C8h as strap-selected alternatives [1] §2-3. Byte lanes are
  reached by adding 0-3 to the computed address — the manual's example reaches the `DAC_MASK` byte
  of `DAC_REGS` (select 17h) at 5EEEh with base 2ECh [1] §2-4. The Xclaim 3D's FCode carries the
  literal $2EC — the sparse-I/O base it computes register addresses from (*observed* [3]).
- **Block I/O** (PCI only, "relocatable"): "registers not associated with the draw engine and
  multimedia map into a continuous block that starts at the I/O base address specified in the PCI
  configuration registers" — `absolute I/O address = (BLK select << 2) + I/O base` [1] §2-3. Only
  block 0 contains I/O-mappable registers, but *all* of block 0 is visible in block I/O, including
  registers with no sparse alias [1] §2-3, §2-4.

On the non-Intel host the register contract changes width: "I/O mapped registers may not be
available on non-Intel platforms", and while the memory-mapped registers "may be read ... in 8-bit,
16-bit and 32-bit quantities" like the GX's, "writes to the memory mapped registers must be
performed in one 32-bit write" on the mach64CT family [2] §2.3.3. The GX-era halfword register
traffic of the Apple GX card's FCode ([mach64.md](mach64.md) §4.2) has no equivalent here.

### 2.3 Register class summary

The register file by class, from the cross-reference [1] Ch. 3. "I/O" is the sparse select; "BLK"
the block-I/O dword select; "MM" the memory-mapped `block_offset`. GT-only registers are marked:

| Class | Register | R/W | I/O | MM | Detail |
|---|---|---|---|---|---|
| CRTC | CRTC_H_TOTAL_DISP | R/W | 0, 1F | 0_00 | [1] p. 4-20 |
| CRTC | CRTC_H_SYNC_STRT_WID | R/W | 1 | 0_01 | [1] p. 4-21 |
| CRTC | CRTC_V_TOTAL_DISP | R/W | 2 | 0_02 | [1] p. 4-22 |
| CRTC | CRTC_V_SYNC_STRT_WID | R/W | 3 | 0_03 | [1] p. 4-23 |
| CRTC | CRTC_VLINE_CRNT_VLINE | R/W | 4 | 0_04 | [1] p. 4-24 |
| CRTC | CRTC_OFF_PITCH | R/W | 5 | 0_05 | [1] p. 4-25 |
| CRTC | CRTC_INT_CNTL | R/W | 6 | 0_06 | [1] p. 4-26 |
| CRTC | CRTC_GEN_CNTL | R/W | 7 | 0_07 | [1] p. 4-27 |
| Overscan | OVR_CLR, OVR_WID_LEFT_RIGHT, OVR_WID_TOP_BOTTOM | R/W | 8-A | 0_10-0_12 | [1] pp. 4-31-4-32 |
| Cursor | CUR_CLR0, CUR_CLR1, CUR_OFFSET, CUR_HORZ_VERT_POSN, CUR_HORZ_VERT_OFF | R/W | B-F | 0_18-0_1C | [1] pp. 4-33-4-37 |
| General I/O | GP_IO_CNTL, GP_IO | R/W | — | 0_1F, 0_1E | [1] p. 4-3-4-4 |
| Scratch | SCRATCH_REG0, SCRATCH_REG1 | R/W | 10, 11 | 0_20, 0_21 | [1] pp. 4-6-4-7 |
| Clock | CLOCK_CNTL | R/W | 12 | 0_24 | [1] p. 4-38 |
| Bus | BUS_CNTL | R/W | 13 | 0_28 | [1] p. 4-8 |
| Memory | MEM_CNTL | R/W | 14 | 0_2C | [1] p. 4-10 |
| Memory | MEM_VGA_WP_SEL, MEM_VGA_RP_SEL | R/W | 15, 16 | 0_2D, 0_2E | [1] pp. 4-12-4-13 |
| DAC | DAC_REGS, DAC_CNTL | R/W | 17, 18 | 0_30, 0_31 | [1] pp. 4-39-4-40 |
| DAC | CRC_SIG | R | 1D | 0_3A | [1] p. 4-42 |
| General/test | GEN_TEST_CNTL | R/W | 19 | 0_34 | [1] p. 4-14 |
| Config | CONFIG_CNTL | R/W | 1A | 0_37 | [1] p. 4-16 |
| Config | CONFIG_CHIP_ID | R | 1B | 0_38 | [1] p. 4-17 (chart not extractable, §6.2) |
| Config | CONFIG_STAT0 | R/W | 1C | 0_39 | [1] p. 4-18 (chart not extractable, §6.2) |
| Destination | DST_OFF_PITCH, DST_X, DST_WIDTH, DST_HEIGHT ... DST_CNTL | R/W | — | 0_40-0_4C | [1] pp. 4-43-4-58 |
| Destination GT | LEAD_BRES_ERR/INC/DEC (aliased from DST_BRES_*), LEAD_BRES_LNTH, TRAIL_BRES_ERR/INC/DEC, Z_OFF_PITCH, Z_CNTL, DST_Y_X alias at 0_4D | R/W | — | 0_43-0_53 | [1] pp. 4-43-4-61 |
| Source | SRC_OFF_PITCH ... SRC_CNTL (15 registers) | R/W | — | 0_60-0_6D | [1] pp. 4-62-4-76 |
| Scaler pipe (GT) | SCALE_Y_OFF, SCALE_WIDTH, SCALE_HEIGHT, SCALE_Y_PITCH, SCALE_X_INC, SCALE_Y_INC, SCALE_VACC, SCALE_3D_CNTL, SCALE_HACC, SCALE_XUV_INC, SCALE_UV_HACC | R/W | — | 0_70-0_F8 | [1] pp. 6-1-6-7; aliased onto the 3D registers (§2.10) |
| Host data | HOST_DATA[15:0], HOST_CNTL | W / R/W | — | 0_80-0_8F, 0_90 | [1] pp. 4-77-4-78 |
| Pattern | PAT_REG0, PAT_REG1, PAT_CNTL | R/W | — | 0_A0-0_A2 | [1] pp. 4-79-4-81 |
| Scissor | SC_LEFT, SC_RIGHT, SC_LEFT_RIGHT, SC_TOP, SC_BOTTOM, SC_TOP_BOTTOM | R/W, W | — | 0_A8-0_AD | [1] pp. 4-82-4-87 |
| Data path | DP_BKGD_CLR, DP_FRGD_CLR (GT: also DP_FOG_CLR), DP_WRITE_MSK, DP_CHAIN_MSK, DP_PIX_WIDTH, DP_MIX, DP_SRC | R/W | — | 0_B0-0_B6 | [1] pp. 4-88-4-97 |
| Compare | CLR_CMP_CLR, CLR_CMP_MSK, CLR_CMP_CNTL | R/W | — | 0_C0-0_C2 | [1] pp. 4-98-4-100 |
| FIFO | FIFO_STAT | R | — | 0_C4 | [1] p. 4-101 |
| Context | CONTEXT_MASK, CONTEXT_LOAD_CNTL | R/W | — | 0_C8, 0_CB | [1] pp. 4-102-4-103 |
| Engine | GUI_TRAJ_CNTL | R/W | — | 0_CC | [1] p. 4-104 |
| Status | GUI_STAT | R | — | 0_CE | [1] p. 4-106 |
| Texture (GT) | TEX_0_OFF ... TEX_10_OFF, S_X_INC2 ... TEX_SIZE_PITCH | R/W | — | 0_70-0_DC | [1] pp. 6-8-6-11 |
| Interpolation (GT) | RED/GREEN/BLUE/ALPHA/FOG/Z _X_INC, _Y_INC, _START | R/W | — | 0_F0-0_FB | [1] pp. 6-12-6-14 |
| Overlay | OVERLAY_Y_X_START, OVERLAY_Y_X_END, OVERLAY_VIDEO_KEY_CLR/MSK, OVERLAY_GRAPHICS_KEY_CLR/MSK, OVERLAY_KEY_CNTL | R/W | — | 1_00-1_06 | [1] pp. 5-1-5-7 |
| Overlay scaler | OVERLAY_SCALE_INC, OVERLAY_SCALE_CNTL, SCALER_HEIGHT_WIDTH, OVERLAY_TEST, SCALER_THRESHOLD | R/W | — | 1_08-1_0C | [1] pp. 5-8-5-13 |
| Capture | CAPTURE_Y_X, CAPTURE_HEIGHT_WIDTH, CAPTURE_CONFIG, TRIG_CNTL, VIDEO_SYNC_TEST | R/W | — | 1_10-1_16 | [1] pp. 5-18-5-24 |
| CRTC | EXT_CRTC_GEN_CNTL (VT-A4 only) | R/W | — | 1_17 | [1] p. 4-29 |
| VMC | VMC_CONFIG, VMC_STATUS, VMC_CMD, VMC_ARG0, VMC_ARG1, VMC_SNOOP_ARG0/1 | R/W, R | — | 1_18-1_1E | [1] pp. 5-31-5-38 |
| Video buffers | BUF0_OFFSET, BUF0_PITCH, BUF0_CAP_ODD_OFFSET, BUF1_OFFSET, BUF1_PITCH, BUF1_CAP_ODD_OFFSET | R/W | — | 1_20-1_2C | [1] pp. 5-25-5-30 |
| VMC stream | VMC_STRM_DATA[15:0] | R/W | — | 1_30-1_3F | [1] p. 5-39 |
| Video config | VIDEO_FORMAT, VIDEO_CONFIG | R/W | — | 1_12, 1_13 | [1] pp. 5-14-5-17 |
| Debug | HW_DEBUG | R/W | — | 1_50 | [1] p. 4-19 |

All draw-engine and multimedia registers (dword offsets ≥ 40h in block 0, and block 1) are
memory-mapped only; the setup/control and CRTC/DAC registers carry both views. The GT's register
count against the VT's is the point of §2.10: the scaler pipe is not new address space — it is
*aliased onto* the 3D engine's registers.

### 2.4 Configuration registers

**CONFIG_CNTL** (I/O 1Ah, BLK 37h, MM 0_37h) — on this generation it also has a memory alias,
unlike the GX where it was I/O-only ([mach64.md](mach64.md) §2.2) [1] p. 4-16:

| Field | Bits | Meaning |
|---|---|---|
| CFG_MEM_AP_SIZE | 1:0 | read-only, always 2 ("2x8 MByte apertures"); others reserved |
| CFG_MEM_VGA_AP_EN | 2 | R/W — memory-mapped registers visible in the VGA aperture (default 0) |
| CFG_MEM_AP_LOC | 15:4 | read-only — linear aperture location on a 16 MB boundary (VT: bits 5:0 = 00) |
| CFG_VGA_DIS | 16 | R/W — 1 disables the VGA core (default 0: enable if the CONFIG_STAT0 VGA strap is set) |
| CDE_WINDOW | 29 | VT only — the GT has no CDE window field [1] p. 4-16 |

The register's own usage note bounds who touches it: "Aperture configuration should be done in the
adapter BIOS only", and "Both CFG_CARD_ID and CFG_VGA_DIS are touched only in the adapter ROM on
power-up to configure the board for multiple mach64 usage" [1] p. 4-16. The 3D RAGE note that
follows is the generation's address-range change: "all offset registers are expanded to allow
8 Meg pointers, with 64-bit granularity. Texture map pointers must have byte granularity" [1]
p. 4-16 — the GT's offset registers address 8 MB, not the GX's 4 MB ([mach64.md](mach64.md) §2.9).

**CONFIG_CHIP_ID** (I/O 1Bh, MM 0_38h, read-only) returns "the chip type, class, and revision"
[2] §3.3.4 — the letter-coded identification the GX chart shows ([mach64.md](mach64.md) §2.4).
The VT/GT-era bit chart (RRG p. 4-17) did not survive text extraction of the register reference,
so the exact field layout for this generation is unverified (§6.2); the two documented
identification routes are the ASIC ID byte at PCI configuration offset 08h (§4.1) and the BIOS
query structure's ASIC-type code, which distinguishes GT (0x4700), GT-A2/NEC (0x4701), GT-B/SGS
(0x473A) and GT-B/UMC (0x475C) [2] Table A-6.

**CONFIG_STAT0** (I/O 1Ch, MM 0_39h) returns the board's strapped configuration. The cross-reference
lists it **R/W on the VT/GT** [1] Ch. 3 — a change from the GX, where it is read-only and the Apple
GX card's firmware writes it anyway, relying on the write being discarded ([mach64.md](mach64.md)
§2.4, §5). Its VT/GT bit chart shares the OCR failure of CONFIG_CHIP_ID (§6.2). **HW_DEBUG**
(MM 1_50h) is "reserved for debugging hardware on engineering samples" [1] §1-2, p. 4-19.

### 2.5 CRTC registers

The accelerator CRTC file, with the fields a mode set touches ([1] pp. 4-20-4-30; the horizontal
parameters are in characters, pixels-times-8, the vertical in lines):

| Register | Fields |
|---|---|
| CRTC_H_TOTAL_DISP (MM 0_00) | CRTC_H_TOTAL bits 11:0, CRTC_H_DISP bits 23:16 — stored as count-1 |
| CRTC_H_SYNC_STRT_WID (MM 0_01) | CRTC_H_SYNC_STRT (11:0), CRTC_H_SYNC_DLY (13:12), CRTC_H_SYNC_STRT_HI (14), CRTC_H_SYNC_WID (23:16), CRTC_H_SYNC_POL (29; 1 = active low) |
| CRTC_V_TOTAL_DISP (MM 0_02) | CRTC_V_TOTAL bits 11:0, CRTC_V_DISP bits 26:16 — count-1 |
| CRTC_V_SYNC_STRT_WID (MM 0_03) | CRTC_V_SYNC_STRT (11:0), CRTC_V_SYNC_WID (23:16), CRTC_V_SYNC_POL (29; 1 = active low) |
| CRTC_VLINE_CRNT_VLINE (MM 0_04) | CRTC_VLINE (the vertical-line interrupt line, 11:0), read-only CRTC_CRNT_VLINE (26:16) |
| CRTC_OFF_PITCH (MM 0_05) | CRTC_OFFSET bits 19:0 in 64-bit words, CRTC_PITCH bits 38:20 in pixels-times-8; the pitch "must correspond exactly to the destination draw engine pitch for visible screen memory" [1] p. 4-25 |
| CRTC_INT_CNTL (MM 0_06) | interrupt control and status — §3.9 |
| CRTC_GEN_CNTL (MM 0_07) | master control, below |
| EXT_CRTC_GEN_CNTL (MM 1_17) | VT-A4 only — additional CRTC control, out of the GT's evidence path [1] p. 4-29 |

**CRTC_GEN_CNTL**, the VT/GT bit file [1] p. 4-27:

| Field | Meaning |
|---|---|
| CRTC_DBL_SCAN_EN / CRTC_INTERLACE_EN | double scan; interlace |
| CRTC_HSYNC_DIS / CRTC_VSYNC_DIS | sync output disables |
| CRTC_CSYNC_EN | composite sync on the horizontal sync output |
| CRTC_DISPLAY_DIS | forces blanking active |
| CRTC_VGA_XOVERSCAN | overscan enable in VGA mode |
| CRTC_PIX_WIDTH (bits 11:8) | 1 = 4 bpp, 2 = 8 bpp, 3 = 15 bpp (5,5,5), 4 = 16 bpp (5,6,5), 5 = 24 bpp, 6 = 32 bpp |
| CRTC_BYTE_PIX_ORDER | 4-bpp pixel order within each byte |
| CRTC_FIFO_OVERFILL (bits 15:14) | double quadwords to overfill the display FIFO (0-3) |
| CRTC_FIFO_LWM (bits 20:16) | display FIFO low water mark; "the display FIFO is 16 entries deep" |
| VGA_128KAP_PAGING | paging through the 128K VGA aperture |
| CRTC_DISPREQ_ONLY | 1 = only display requests are serviced |
| CRTC_LOCK_REGS | locks the extended CRTC registers read-only |
| CRTC_SYNC_TRISTATE | tri-states HSYNC and VSYNC |
| CRTC_DISP_REQ_ENB (bit 22) | 1 = **disable** display requests — **default 1** (§5) |
| VGA_ATI_LINEAR | linear addressing through the VGA aperture |
| CRTC_VSYNC_FALL_EDGE | frame sequence starts on the falling VSYNC edge |
| VGA_TEXT_132 / VGA_XCRT_CNT_EN / VGA_CUR_B_TEST | extended text mode; extended display address counter; cursor blink test |
| CRTC_EXT_DISP_EN (bit 24) | 0 = VGA display, 1 = extended (accelerator) display — default 0 |
| CRTC_ENABLE (bit 25) | 0 = CRTC reset, 1 = CRTC enable — default 0 |

### 2.6 DAC and general-purpose I/O registers

**DAC_CNTL** (I/O 18h, MM 0_31h) configures the integrated true-colour palette DAC [1] p. 4-40:

| Field | Meaning |
|---|---|
| DAC_BLANKING | 7.5 IRE blanking pedestal enable |
| DAC_CMP_DISABLE | power down the DAC comparators |
| DAC_CMP_OUTPUT (R) | comparator result: 0 = at least one comparator > 0.42 V; 1 = all three < 0.28 V — the monitor-attached test (§3.7) |
| DAC_8BIT_EN | 8-bit versus 6-bit DAC operation |
| DAC_VGA_ADR_EN | VGA DAC I/O addresses active in extended display mode |
| DAC_FEA_CON_EN | feature-connector signal outputs; "should be disabled in 24 bpp and 32 bpp modes and high resolution modes" |
| DAC_PDWN | powers down the DAC macro only |
| DAC_TYPE (R) | 0 = internal DAC, 18-bit palette, no gamma correction; 1 = internal DAC, 24-bit palette, **with gamma correction** — "Always 1 for VT" |
| DAC_GIO_STATE_0/1/4, DAC_GIO_DIR_0/1/4 | the three general-purpose I/O pins' state and direction (default input) — the monitor-sense pins (§3.7) |
| DAC_RW_WS | DAC read/write wait states |

**DAC_REGS** (I/O 17h, MM 0_30h) is the four 8-bit palette cells (`DAC_W_INDEX`, `DAC_DATA`,
`DAC_MASK`, `DAC_R_INDEX`), also aliased at the VGA DAC I/O addresses 3C6h-3C9h; "the same
palette data and DAC_MASK are used whether in VGA or extended modes" [1] p. 4-39. **CRC_SIG**
(I/O 1Dh, MM 0_3A) is the display-CRC signature result [1] p. 4-42.

The **GP_IO** pair (GP_IO_CNTL at MM 0_1F, GP_IO at MM 0_1E) multiplexes a 16-bit general-purpose
bus whose *target* is selectable — 0 = VIDEO_IO, 1 = VMC, 2 = VFC, 3 = ENG_DIAG, 4 = GP_IO [1]
p. 4-3. The pin assignments in VIDEO_IO mode are the video port's: DATA(0..7), ESYNC, EVIDEO,
BLANKB, SNRDYB, SAB, EDCLK, VMCMASK and DCLK, each with its own direction bit [1] p. 4-4. This is
the register set behind the card-level video input/output (§3.6).

### 2.7 Clock control and the PLL

**CLOCK_CNTL** (I/O 12h, MM 0_24h) selects the pixel clock and addresses the internal synthesizer:
"The internal clock synthesizer in the VT and 3D RAGE has only 4 programmable pixel clock
settings. Therefore, only bits 0 and 1 of CLOCK_SEL are used" [1] p. 4-38. The PLL registers behind
it are accessed through the register's bytes — CLOCK_CNTL1 (bits 15:8) carries the PLL address and
the `PLL_WR_EN` bit, CLOCK_CNTL2 (bits 23:16) the data — with the manual's read/write idioms
`iow8 CLOCK_CNTL1 PLL_ADDR` then `ior8 CLOCK_CNTL2 PLL_DATA`, or a single 32-bit
`CLOCK_SEL | PLL_ADDR | PLL_WR_EN | PLL_DATA` write [1] App. B.

All clocks derive from three masters — Bus Clock (CPUCLK), MCLK and VCLK — and MCLK and VCLK each
have four source choices, including the internal PLLs (PLLMCLK and PLLVCLK) and external pins [1]
App. B "Clock Sources". The PLL register file, addressed indirectly through CLOCK_CNTL [1] App. B:

| Addr | Register | Fields (default) |
|---|---|---|
| 1 | PLL_MACRO_CNTL | charge-pump gain (2:0), VCGEN gain (4:3), duty-cycle control (7:5) — default D4h |
| 2 | PLL_REF_DIV | reference divider, 7 bits — default 36h |
| 3 | PLL_GEN_CNTL | PLL_OVERRIDE (power down), PLL_MRESET, OSC_EN, EXT_CLK_EN, MCLK_SRC_SEL (6:4: 000 PLLMCLK, halves and quarters, 100 CPUCLK, 101 DCLK, 110 PLLREFCLK, 111 XTALIN) — default 4Fh |
| 4 | MCLK_FB_DIV | MCLK feedback divider — default 97h, 40 MHz |
| 5 | PLL_VCLK_CNTL | VCLK_SRC_SEL (1:0: CPUCLK, DCLK, XTALIN, PLLVCLK/post-divider), PLL_PRESET, VCLK_INVERT, ECP_DIV (5:4), ERATE_GT_XRATE, **SCALER_LOCK_EN** (lock out lower-priority memory requesters when the scaler needs exclusive access) — default 04h |
| 6 | VCLK_POST_DIV | four 2-bit post dividers, one per VCLK setting — default 6Ah |
| 7-10 | VCLK0_FB_DIV ... VCLK3_FB_DIV | four feedback dividers — defaults BEh, D6h, EEh, 88h |

Four pre-staged VCLK settings are therefore live at once, switchable by CLOCK_SEL bits 1:0 — the
mechanism a mode-switching driver uses to change pixel clocks without reprogramming the loop.

### 2.8 Memory control

**MEM_CNTL** (I/O 14h, MM 0_2C) configures the memory interface [1] p. 4-10. On this generation the
documented fields are DRAM/SDRAM-oriented: MEM_SIZE (bits 2:0: 000 = 512 KB, 001 = 1 MB, 010 = 2 MB,
011 = 4 MB, 1XX reserved — default 0), MEM_REFRESH (refresh cycle length, DRAM and SDRAM),
MEM_CYC_LNTH_AUX (SDRAM activate-to-command length) and MEM_CYC_LNTH (DRAM non-page cycle length or
SDRAM precharge-to-activate length), plus the latch and delay tuning fields written once at boot.
The generation's *Apple* memory story is SGRAM — the beige G3's onboard part has an "architecture
optimized to support high-speed SGRAM video memory", 100 MHz/10ns devices on a 64-bit bus [4] — and
SGRAM block write is a Rage II+ feature [2] Table 7-1; but the SGRAM programming fields are *not*
documented in the VT/GT register chart, which predates that memory type (§6.6). The driver
evidence for the SGRAM path is a name only: the Xclaim 3D's embedded driver carries a
`VTB_ResetSDRAM` routine [3].

**MEM_VGA_WP_SEL / MEM_VGA_RP_SEL** (I/O 15h/16h, MM 0_2D/0_2E) are the VGA-aperture page pointers;
on a PCI card with the linear aperture they are unused ([mach64.md](mach64.md) §2.7).

### 2.9 Draw-engine registers: the mach64 core and the GT deltas

The 2D engine is the mach64 core, unchanged in address and semantics from the GX generation: the
data-path group (`DP_BKGD_CLR` 0_B0, `DP_FRGD_CLR` 0_B1, `DP_WRITE_MSK` 0_B2, `DP_CHAIN_MSK` 0_B3,
`DP_PIX_WIDTH` 0_B4, `DP_MIX` 0_B5, `DP_SRC` 0_B6), the compare, scissor, pattern and host-data
groups, the context block and its load control, and `GUI_TRAJ_CNTL`'s composite of DST_CNTL and
SRC_CNTL — all as [mach64.md](mach64.md) §2.8-§2.10 documents them. Three GT deltas matter:

- **The offset and pointer registers address 8 MB** at 64-bit granularity, with texture pointers
  needing byte granularity [1] p. 4-16.
- **The Bresenham registers are renamed and doubled**: the GX's `DST_BRES_ERR/INC/DEC/LNTH` become
  `LEAD_BRES_ERR/INC/DEC/LNTH` (with `DST_BRES_LNTH` at 0_48h aliased to `LEAD_BRES_LNTH` at 0_51h),
  and a second accumulator — `TRAIL_BRES_ERR/INC/DEC` at 0_4E-0_50h — joins them for the trapezoid
  trajectory [1] Ch. 3, pp. 4-43-4-59. A second composite `DST_Y_X` alias appears at 0_4Dh [1] Ch. 3.
- **The Z plane arrives**: `Z_OFF_PITCH` (0_52h) and `Z_CNTL` (0_53h) define the depth buffer the
  3D pipeline tests against [1] pp. 4-60-4-61 — the register pair behind Apple's "includes 16-bit
  Z buffer for hidden texture surface removal" [4].

`DP_PIX_WIDTH` gains the **DP_SCALE_PIX_WIDTH** field (bits 18:16) — the scaler/3D data path's own
width select: 2 = 8 bpp pseudocolor, 3 = 15 bpp aRGB 1555, 4 = 16 bpp RGB 565, 6 = 32 bpp aRGB 8888,
7 = 8 bpp RGB 332, 8 = Y8 greyscale, 11 = YUV 422 packed, 14 = aYUV 444, 15 = 16 bpp aRGB 4444 [1]
p. 4-92. `DP_SRC`'s colour source select gains value 5 — "Scaler/3D data (3D RAGE); (reserved in
VT)" [1] p. 4-97 — the single setting that turns a blit into a scaled, colour-converted blit
(§3.5). `DP_FRGD_CLR` doubles as `DP_FOG_CLR` for fog [1] Ch. 3. Host-data traffic through the 3D
pipe is restricted: "only a single pixel at a time is sent to the host data register. The pixel
will be assumed to be aligned to bit 0. The DP_SCALE_PIX_WIDTH rather than the DP_HOST_PIX_WIDTH
field will determine the size of the data" [1] §4 "Host Data".

### 2.10 The scaler pipe and the 3D registers

The GT's new address space is mostly *aliases*: "All of the Scaler registers, except SCALE_3D_CNTL
and SCALE_VACC, are aliased with certain 3D and Texture Mapping registers" [1] §6-1. The layout
[1] Ch. 3:

| Scaler register | Aliased 3D register | MM |
|---|---|---|
| SCALE_Y_OFF | TEX_0_OFF | 0_70 |
| SCALE_WIDTH / SCALE_HEIGHT | — | 0_77 / 0_78 |
| SCALE_Y_PITCH | S_Y_INC | 0_7B (alias 0_D4) |
| SCALE_X_INC | RED_X_INC | 0_7C (alias 0_F0) |
| SCALE_Y_INC | GREEN_X_INC | 0_7D (alias 0_F3) |
| SCALE_VACC | — | 0_7E |
| SCALE_3D_CNTL | — | 0_7F |
| SCALE_HACC | RED_START | 0_F2 |
| SCALE_XUV_INC | BLUE_X_INC | 0_F6 |
| SCALE_UV_HACC | BLUE_START | 0_F8 |

The texture-mapping group proper is `TEX_0_OFF`...`TEX_10_OFF` (aliased over the scaler offsets),
the S_/T_ increment and start registers at 0_D0-0_DB, and `TEX_SIZE_PITCH` at 0_DC [1] pp. 6-8-6-11;
the colour, Z and alpha interpolation group is the RED/GREEN/BLUE/ALPHA/FOG/Z `_X_INC`, `_Y_INC`,
`_START` family at 0_F0-0_FB [1] pp. 6-12-6-14. The scalers reuse this interpolator hardware: "For
both horizontal and vertical scaling, the color interpolator DDAs are reused to function as a
counter/accumulator for the scaler source memory address generator" [1] §6-1.

**SCALE_3D_CNTL** (MM 0_7F) is the pipeline's master control [1] pp. 6-4-6-6:

| Bit | Field | Meaning |
|---|---|---|
| 0 | SCALE_PIX_EXPAND | pixel expansion: 0 = zero extend, 1 = dynamic range correct |
| 1 | SCALE_DITHER | 0 = X error diffusion, 1 = two-dimensional table-lookup dither |
| 2 | DITHER_EN | dither the scaled/3D image (8 and 16 bpp destinations) |
| 3 | DITHER_INIT | reset the dither error register at each line start |
| 4 | ROUND_EN | rounding instead of dithering for 24-bit reduction; both off = truncation |
| 5 | TEX_CACHE_DIS | disable the texel cache (latency-bound modes) |
| 6:8 | SCALE_3D_FCN | 0 = none, 1 = scaling, 2 = texture mapping, 3 = shading |
| 9 | SCALE_PIX_REP | replicate pixels instead of blending (stretch blit) |
| 10 | NEAREST_TEX_VIS | nearest-texel visibility test during filtering |
| 11 | APPLE_YUV_MODE | interpret YUV as signed — "UV will have an offset of 0, rather than 128" |
| 12:13 | ALPHA_FOG_EN | 0 = none, 1 = alpha blending, 2 = fog (destination from DP_FRGD_CLR) |
| 14 | COLOR_OVERRIDE | force interpolator colour in place of texel colour (multipass) |
| 15 | RED_DITHER_MAX | limit the RED dither range away from reserved DAC LUT areas |
| 16 | SIGNED_DST_CLAMP | assume signed destination while blending — "used for MPEG motion compensation" |
| 17:19 | ALPHA_BLND_SRC | source blend factor: 0, 1, (RD,GD,BD), (1-RD,1-GD,1-BD), (As,As,As), ... |
| 20:22 | ALPHA_BLND_DST | destination blend factor: 0, 1, (Rs,Gs,Bs), (1-Rs,1-Gs,1-Bs), (As,As,As), ... |
| 23:25 | TEX_LIGHT_FCN | 0 = no lighting, 1 = modulate (texel x interpolator), 2 = alpha decal |
| 26 | MIP_MAP_DISABLE | use only the largest present texture map |
| 27 | BILINEAR_TEX_EN | 2x2 blend under magnification |
| 28:29 | TEX_BLEND_FCN | texel blending under minification |
| 30 | TEX_AMASK_AEN | use the texture alpha's low bit as a mask |
| 31 | SRC_3D_SEL | 3D source: 0 = interpolators or texture/line FIFOs, 1 = host FIFO |

Two further scaler registers are unaliased: `SCALE_VACC` (vertical accumulator start: "Sign, 12
bits fractional, 4 bits integer", shiftable for sub-pixel phase control) and, at 0_F6/0_F8, the
**independent UV accumulators** `SCALE_XUV_INC` and `SCALE_UV_HACC` for subsampled YUV — a
register pair the later programmer's guide calls out as "*for the 3D RAGE II/II+ only*" [2]
§8.9.2, the one generation-delta visible inside the scaler's own register set.

### 2.11 The multimedia registers (block 1)

Block 1 is the VT-generation multimedia file, carried whole into the GT [1] Ch. 5. Four groups
matter to a re-implementation:

- **Overlay window and keying** — `OVERLAY_Y_X_START` (1_00), `OVERLAY_Y_X_END` (1_01, with VT-A4's
  `OVERLAY_Y_X_LOCK` coordinate-latch bit), the video and graphics key colour and mask registers
  (1_02-1_05) and `OVERLAY_KEY_CNTL` (1_06), whose three fields choose the video comparison
  function, the graphics comparison function, and the `OVERLAY_CMP_MIX` of the two results
  (16 codes, from "Always Graphics" to "VID_CMP"), plus `OVERLAY_EXCLUSIVE_EN` — "Video stream only
  (suppress graphics)" [1] pp. 5-1-5-7.
- **Overlay scaler and thresholds** — `OVERLAY_SCALE_INC` (1_08), `OVERLAY_SCALE_CNTL` (1_09),
  `SCALER_HEIGHT_WIDTH` (1_0A), `OVERLAY_TEST` (1_0B) and `SCALER_THRESHOLD` (1_0C) with its
  read-back `SCALER_SOURCE_LINE` status [1] pp. 5-8-5-13.
- **General video, capture and buffers** — `VIDEO_CONFIG` (1_13) and `VIDEO_FORMAT` (1_12) (the
  bit fields of §3.6), `CAPTURE_Y_X` (1_10), `CAPTURE_HEIGHT_WIDTH` (1_11), `CAPTURE_CONFIG`
  (1_14), `TRIG_CNTL` (1_15), `VIDEO_SYNC_TEST` (1_16), and the double-buffered capture pointers
  `BUF0_OFFSET`/`BUF0_PITCH`/`BUF0_CAP_ODD_OFFSET` and `BUF1_*` (1_20-1_2C) [1] pp. 5-14-5-30.
- **The VMC port** — `VMC_CONFIG` (1_18, with the `VMC_EN` master set/reset bit), `VMC_STATUS`
  (1_19, with normal/schedule/debug read-back modes), `VMC_CMD` (1_1A), `VMC_ARG0/1` (1_1B/1_1C),
  the read-only `VMC_SNOOP_ARG0/1` (1_1D/1_1E) and the sixteen `VMC_STRM_DATA` registers
  (1_30-1_3F) [1] pp. 5-31-5-39.

The VMC stream registers are the chip's compressed-video outlet: they "can be used to send
unformatted stream data out of the VMC port, in streams of up to 16 DWORDs. The data registers are
in 16 consecutively mapped addresses to allow for block data moves, 16 DWORDs deep. The data will
typically be MPEG compressed data" [1] p. 5-39.

### 2.12 Bus-master registers

Bus mastering is a **Rage II/II+/IIC/Pro capability**, absent from the VT and the first 3D RAGE:
"The 3D RAGE II/II+/C and 3D RAGE PRO chips all have the ability to act as a bus master" [2]
§8.10.1 — a list that names the II but not the GT-A2. (The register reference's PCI chapter
documents the VT's command register with Bus Master Enable "Always 0, disabled" [1] §7-1.) The
transfers are descriptor-driven; a descriptor entry
is four dwords [2] Table 8-3:

| DWORD | Field | Bits | Function |
|---|---|---|---|
| 0 | BM_FRAME_BUF_OFFSET | 23:0 | frame buffer offset for the transfer |
| 1 | BM_SYSTEM_MEM_ADDR | 31:0 | physical system memory address |
| 2 | BM_COMMAND | 11:0 | byte count — **4096 maximum** per descriptor |
| 2 | BM_COMMAND | 30 | disable frame-buffer offset increment |
| 2 | BM_COMMAND | 31 | end-of-descriptor-list |
| 3 | Reserved | 31:0 | — |

The descriptor table "must be in contiguous memory" and the physical address of its head must be
known [2] §8.10.2. The register set named by the setup sequences: `BM_SYSTEM_TABLE` (with the
`SYSTEM_TRIGGER` method field and `SYSTEM_TABLE_ADDR`), `BM_GUI_TABLE` (with
`CIRCULAR_BUF_SIZE`), and `BM_ADDR` — the GUI-transfer target, "in the MM offset format, which
is 0x92" [2] §8.10.3-8.10.4, i.e. the first undocumented offset of the GT generation. The enable
and completion path runs through the older registers: `BUS_EXT_REG_EN` and `BUS_MASTER_DIS` in
`BUS_CNTL`, the `BUSMASTER_EOL_INT` enable/acknowledge in `CRTC_INT_CNTL` (§3.9), and
`SRC_BM_ENABLE`, `SRC_BM_SYNC` and `BUS_MASTER_OP` in `SRC_CNTL` — "BUS_MASTER_OP = 3 for a
system memory to bus master host data register transfer" [2] §8.10.4. The MM offsets of
`BM_SYSTEM_TABLE` and `BM_GUI_TABLE` are not in the evidence set (§6.5).

### 2.13 Reset state

The documented power-up and default values [1] Ch. 4-7: `CFG_MEM_AP_SIZE` defaults to 2 (the 2x8 MB
apertures) and `CFG_MEM_VGA_AP_EN`/`CFG_VGA_DIS` to 0; `CRTC_EXT_DISP_EN` and `CRTC_ENABLE` default
to 0 (VGA display, CRTC in reset) and `CRTC_DISP_REQ_ENB` to 1 (display requests *disabled*);
`CRTC_PIX_WIDTH` defaults to 0 (reserved — a mode set must write it); `GEN_CUR_ENABLE` defaults to 0,
and `GEN_GUI_RESETB` (bit 8) is edge-sensitive — "Resets GUI Engine on high to low transition
(Default = 0)" — so a driver resets the engine by pulsing the bit rather than by holding a level [1]
p. 4-14; the DAC GIO directions default to input;
`MEM_SIZE` defaults to 0 (512 KB) on the documented chart; the interrupt enables of `CRTC_INT_CNTL`
and `BUS_CNTL` default to 0; and the PLL register file carries the defaults of §2.7's table. The
power-on state of the draw-engine latches (mix, source selects, trajectories), of the scaler pipe
and of the 3D registers is not documented anywhere in the evidence set — the same gap the GX
generation has ([mach64.md](mach64.md) §2.11) — and is reachable by any driver that draws before
its first full setup (§6.14).

## 3. Behaviour

### 3.1 Endianness

The part is a little-endian PCI device on a big-endian host; the platform seam — the bridge
swaps between the processor's big-endian bus and the PCI bus's little-endian order — is
[pci.md](../pci.md) §3.4's, and the card sits on the little-endian side of it. The observed
Macintosh evidence for the consequence is the Xclaim 3D's embedded driver, whose register access
layer is a set of byte-swap helpers — `WriteSwap16`, `WriteSwap32`, `ReadSwap16`, `ReadSwap32`
alongside the plain `regw8`/`regr8`/`regw16`/`regr16` accessors — every 16- and 32-bit register
access goes through them (*observed* in the driver's function-name table [3]). The in-chip
endianness control is the mach64 core's `HOST_BIG_ENDIAN_EN` ([mach64.md](mach64.md) §3.1): it
governs only the HOST_DATA path's byte order within pixels, not the register file. The family's
big-endian "extended linear aperture" of the programmer's guide's product table [2] Table 1-1 is
a host-order framebuffer option whose GT-column marking did not survive the table's extraction
(§6.12); no Macintosh evidence exercises it.

### 3.2 The command FIFO and engine status

The FIFO contract is the mach64 core's ([mach64.md](mach64.md) §3.2) with two GT-era changes:

1. **The parameter FIFO is 48 entries deep.** "For the 3D RAGE, the parameter FIFO is expanded to
   48 entries. A new field (FIFO_CNT) is added to GUI_STAT to contain the number of available
   (empty) FIFO entries" — and FIFO_CNT is "less than or equal to 32" [1] p. 4-106. FIFO_STAT
   changes meaning with it: on the 3D RAGE it "indicates the number of full command FIFO entries
   in the top half of the FIFO entries. The number is represented as a single bit per entry. Thus
   if there are 23 full entries, then bits 6:0 will be set" [1] p. 4-101 — two different counters
   of one FIFO, not to be equated.
2. **GUI_ACTIVE covers the 3D engine.** On the GT the busy bit "Indicates that the GUI engine is
   busy OR the 3D engine is busy OR the command FIFO is not empty OR context loading is occurring"
   [1] p. 4-106 — idleness is still tested only through this bit, and "All status bits in this
   register should be read-only when the draw engine is idle" [1] p. 4-106.

The discipline is unchanged: only registers at dword offsets ≥ 40h go through the command FIFO,
all others bypass it; "Each grouping of register writes through the command FIFO must be preceded
by a FIFO check to ensure that sufficient entries are available"; and overflow sets `FIFO_ERR` and
locks the draw engine, recoverable only through the `GEN_TEST_CNTL` reset, with the error
interrupt wireable for debugging through `BUS_CNTL` [1] p. 4-101.

### 3.3 The pixel data path

The pixel data path — monochrome source select, foreground/background colour source and mix, the
17-function ALU, the colour compare veto, the 64-bit-wide physical data path, and the host-data
panic contract — is the mach64 core's, as [mach64.md](mach64.md) §3.3 documents it. The GT-era
additions at the data path's edges:

- The **scaler/3D pipe is a fifth colour source** (DP_BKGD_SRC/DP_FRGD_SRC = 5) whose pixel width
  is selected by DP_SCALE_PIX_WIDTH rather than the host or destination width fields (§2.9) [1]
  pp. 4-92-4-97.
- **Three of the new pixel formats pass through the standard engine as raw data**: "8 bpp
  pseudocolor, Y8, and 8 bpp RGB332 are treated as raw 8 bpp data by the standard draw engine ...
  YUV422 is treated as raw 32 bpp data ... and [is] differentiated by the Scaler/3D block" [1]
  p. 4-93 — a blit of YUV422 through the 2D path moves dwords; only the scaler block interprets
  them.
- The scaler's **source-to-destination format matrix** [1] p. 4-93: pseudo-8 to pseudo-8; Y8 to
  RGB8/15/16/32, Y8, YUV422 and YUV444; RGB 8/12/15/16/32 to RGB 8/15/16/32; YUV422 and YUV444 to
  RGB8/15/16/32, Y8, YUV422 and YUV444 — the colour-space-conversion surface of §3.5.

### 3.4 Trajectories

The rectangle and Bresenham-line trajectories, the source trajectories, the direction contract
("If the destination trajectory is rectangular, SRC_X_DIR and SRC_Y_DIR track DST_X_DIR and
DST_Y_DIR") and the side effects are the mach64 core's ([mach64.md](mach64.md) §3.4). The GT
adds the **trapezoid**: "In the 3D RAGE, a new trajectory is provided — the trapezoid. This new
trajectory may be used for the destination, the texture map sources, the 2D source, and the Z
source" [1] §1-2 — the shape behind the "six perspectively correct texture mapping functions" of
Apple's feature list [4], walked by the LEAD and TRAIL Bresenham accumulator pairs of §2.9.

### 3.5 The front-end scaler

The front-end scaler is the generation's video centrepiece. Its operation, as the programmer's
guide states it: "The 3D RAGE II/II+/IIC and the 3D RAGE PRO chips all have a front end scaler /
3D engine pipeline that provides support for horizontal and vertical scaling, interpolation and
color space conversion of source images. ... Horizontal and vertical scaling are done in a single
pass. For each destination line, two lines of source data are read, and then color expanded to
24 bpp for vertical blending. The resultant data is then blended horizontally, converted to RGB if
necessary and then packed to the destination pixel type and dithered" [2] §8.9.1. (The filter is
"a 2 tap, 4 bit co-efficient fixed linear filter" in that account [2] §8.9.1, but a "2-tap 5-bit
coefficient fixed linear filter" in the 1996 register reference [1] §6-1 — a discrepancy left
honest here; the same register file is describing the same filter.) The DDA accumulators run at
8.12 unsigned precision, with the blend coefficients taken "from the high 5 fractional bits of the
DDA coordinate registers" [1] §6-1.

The programming sequence for a scaled blt [2] §8.9.2, with the register names of §2.10:

1. Activate the pipe: `SCALE_3D_FCN = 1` (scaling) in `SCALE_3D_CNTL`.
2. Point it at memory: `SCALE_OFF` (byte offset, bits 2:0 required zero), `SCALE_PITCH`,
   `SCALE_WIDTH`, `SCALE_HEIGHT` [1] p. 6-1.
3. Set the increments — `SCALE_X_INC` and `SCALE_Y_INC`, "12 bit fractional, 8 bits unsigned
   integer" — and, on the II/II+ only, `SCALE_UV_HACC` "should you wish to scale U and V
   independent of Y" [2] §8.9.2; the accumulators `SCALE_HACC`/`SCALE_VACC` may start non-zero
   "for fine control", shifting the input pixel relative to the output [1] pp. 6-3, 6-7.
4. Route it into the engine: `DP_FRGD_SRC = 5`, `DP_SCALE_PIX_WIDTH`, the write mask, the mixes,
   and a rectangular trajectory — `DST_WIDTH` initiates the blt [2] §8.9.2.

The YUV handling has two Macintosh-relevant corners. `APPLE_YUV_MODE` (SCALE_3D_CNTL bit 11)
denotes "that YUV data in the Scale/3D pipe will be interpreted as signed (UV will have an offset
of 0, rather than 128)" [1] p. 6-4 — the register named for the platform it was added for.
`SIGNED_DST_CLAMP` (bit 16) is the MPEG motion-compensation mode of §1.2. And the scaler's source
can be ordinary host-written memory: "SCALER_IN can be specified independently of VIDEO_IN when
the scaler is used to scale software MPEG or AVI video data written from the host" [1] §5
"Video Input and Output Formats" — the software-MPEG playback path the ATI MPEG Accelerator
software drives [7]. Scaled, converted data can also be a texture — "video textures and video
lighting" in Apple's feature list [4].

### 3.6 Overlay, capture and the VMC port

The **overlay** is a screen-positioned window that selects between the graphics stream and the
scaler's video on a per-pixel basis. The window is defined by `OVERLAY_Y_X_START`/`END`
(inclusive coordinates, "relative to the start of the active display", and not to be programmed
beyond it); within the window "data is not displayed until the overlay key colour is matched in
the destination" [1] §5-1. The two keyers compare masked pixels against the key colours —
video is "always 24 bits in the ordered 'RGB'" while the graphics key registers "must take into
account the current graphics pixel depth" [1] pp. 5-1-5-5 — and `OVERLAY_KEY_CNTL` combines the
two comparison results through `OVERLAY_CMP_MIX`'s sixteen codes; "A result of '1' means that
video will be displayed within the region defined by the overlay coordinates" [1] p. 5-3.
`OVERLAY_EXCLUSIVE_EN` suppresses the graphics fetch entirely inside the window — a bandwidth
economy a Macintosh driver doing full-screen video would want.

**Video input** is configured through `VIDEO_CONFIG` and `VIDEO_FORMAT` [1] pp. 5-14-5-17:

| Register | Field | Meaning |
|---|---|---|
| VIDEO_CONFIG | VIDEO_MODE | 0 = video in; 1 reserved |
| VIDEO_CONFIG | VIDEO_DEVICE | which device inputs video: 0 = VMC, 1 = the video port |
| VIDEO_CONFIG | VIDEO_HSYNC_POL / VIDEO_VSYNC_POL | sync polarities for the parallel decoder mode |
| VIDEO_CONFIG | VIDEO_IO_SIZE | 0 = 8 bit; 1 reserved |
| VIDEO_CONFIG | VIDEO_FIELD_FLIP | flips the field sense |
| VIDEO_CONFIG | VIDEO_INPUT_TYPE | 0 = parallel input, CCIR-style, Philips-compatible pin mode; 3 = embedded synchronization, Brooktree-compatible |
| VIDEO_CONFIG | VIDEO_HORZ_DOWN / VIDEO_VERT_DOWN | decimation: 1:1, 2:1 or 4:1 pixel/line skipping |
| VIDEO_FORMAT | VIDEO_IN | capture format: 2 = 8 bpp, 4 = 16 bpp, 6 = 32 bpp, 11 = VYUY422, 12 = YVYU422 |
| VIDEO_FORMAT | SCALER_IN | scaler source format from memory: 3 = 15 bpp aRGB 1555, 4 = 16 bpp RGB 565, 6 = 32 bpp aRGB 8888, 9 = YUV 9, 10 = YUV 12, 11 = VYUY422, 12 = YVYU422 |
| VIDEO_FORMAT | HOST_MEM_MODE | aperture memory mode: 0 = normal, 1 = Y, 2 = U, 3 = V |
| VIDEO_FORMAT | HOST_YUV_APER / HOST_BYTE_SHIFT_EN | which aperture half the YUV host mode uses; the 1-bit-shift packing mode |

The pin mode named by VIDEO_INPUT_TYPE — the CCIR-style parallel decoder interface, with the
`GP_IO` register's DATA/ESYNC/EVIDEO/BLANKB/SNRDYB/SAB/EDCLK/DCLK pins (§2.6) — is the interface
a video-capture card hangs a decoder chip on; the Xclaim VR's video input/output is that
board-level hardware, whose companion chips are not identified in the evidence set (§6.8).
Capture runs double-buffered through `BUF0_*`/`BUF1_*` with separate odd-field offsets, triggered
by `TRIG_CNTL`'s `CAPTURE_EN` (with `VMC_EN` in VMC-input mode) [1] pp. 5-18-5-38. The beige G3's
storage rule for captured data: "Data from the video input module is always stored and
transferred at 16 bits per pixel" [4].

### 3.7 Monitor sensing: AppleSense and DDC

The part carries both monitor-identification disciplines, one generation before the Rage 128
consolidated them ([rage-128.md](rage-128.md) §3.6). The three **DAC GIO pins** (GIO0, GIO1,
GIO4) are the general-purpose sense pins — direction and state per pin, all inputs by default
([1] p. 4-40) — and the **DAC comparators** provide the load test: `DAC_CMP_OUTPUT` reads 1 when
all three RGB comparators are below 0.28 V, 0 when at least one exceeds 0.42 V [1] p. 4-40. The
beige G3's developer note names the protocols: "DDC1 and DDC2B+ for plug-and-play monitor
support" [4], and the external connector's pin assignments are "the same as those for the Power
Macintosh 5500 and 6500 computers" [4] — the DB-15 AppleSense set ([mach64.md](mach64.md) §3.6
for the GX-era four-step sense the same lines carry).

The Xclaim 3D's firmware evidence shows both disciplines in use at different layers (*observed*
[3]): the FCode publishes an **`EDID` property** — a 128-byte buffer, the size of a DDC EDID 1.x
block — so the boot-time driver path reads EDID over DDC; and the embedded Mac OS driver carries
both a sense-line family (`ReadSenseLines`, `GetSenseCode`, `ValidSenseCode`, `GetMonitorIdx`,
`VTB_SetSense`, `VTB_GetSense`) and a complete DDC bit-bang family (`DDCSetClock`, `DDCSetData`,
`DDCSetStart`, `DDCSetStop`, `DDCSendBit`, `DDCReceiveBit`, `DDCSendByte`, `DDCReceiveByte`,
`DDCCheck`, `DDCReadEDID`, `DDCGetEDID`, `DDCCheckEDID`, `DoDDCForceRead`). Which pins the DDC
pair shares with the AppleSense lines — the Rage 128's MONID pins answer this explicitly, the
GT's documentation does not — is open (§6.11).

### 3.8 Clocking

The clock tree is the internal dual synthesizer of §2.7: PLLMCLK generates the memory clock,
PLLVCLK the pixel clocks (four pre-staged settings), each with external-pin and divided source
alternatives, all programmed through `CLOCK_CNTL`'s indirect PLL window [1] App. B. The Apple
description of the on-chip timing matches: the controller's "transactions are synchronized with
the PCI bus", "data transfers from the frame-buffer RAM are clocked by the MEM_CLK signal", and
"data transfers to the CLUT and the video output are clocked by the dot clock" [4]. The observed
driver surface for the same tree is the Xclaim 3D's embedded driver, whose clock routines are
`VTB_SetPll`/`VTB_GetPll`, `VTB_ProgramVclk`/`VTB_ProgramMclk`, `VTB_InitClk`, `VTB_InitDLL`,
`VTB_VCLKmaxPN`, `VTB_Adj_dotclock`, `VTB_Adj_VPll`, `Mclk_Post` — and, separately,
`VTB_XCLKvsMCLK`, `VTB_GetXClk` and `VTB_GetMClk` (*observed* [3]), evidence that the driver
distinguishes an XCLK from MCLK although the documented PLL file of [1] App. B programs only
MCLK and VCLK (§6.7).

### 3.9 Interrupts

`CRTC_INT_CNTL` (I/O 6h, MM 0_06) is "used for enabling and acknowledging interrupts generated by
the accelerator CRTC, video capture, overlay display, and VMC port, and reading the status of the
CRTC" [1] p. 4-26. The VT/GT bit file:

| Bits | Field | Meaning |
|---|---|---|
| 0 | CRTC_VBLANK (R) | vertical blank status |
| 1 | CRTC_VBLANK_INT_EN / 2 CRTC_VBLANK_INT (+_AK) | enable; latched VBL interrupt, write-1-to-clear acknowledge |
| 3 | CRTC_VLINE_INT_EN / 4 CRTC_VLINE_INT (+_AK) | the vertical-line interrupt at CRTC_VLINE |
| 5 | CRTC_VLINE_SYNC (R) | even/odd scan line |
| 6 | CRTC_FRAME (R) | interlaced odd/even frame |
| 7-8 | VIDEOIN_EVEN_INT_EN / VIDEOIN_EVEN_INT (+_AK) | end-of-even-field capture interrupt |
| 9-10 | VIDEOIN_ODD_INT_EN / VIDEOIN_ODD_INT (+_AK) | end-of-odd-field capture interrupt |
| 11-12 | OVERLAY_EOF_INT_EN / OVERLAY_EOF_INT (+_AK) | overlay end-of-frame interrupt |
| 13-14 | VMC_EC_INT_EN / VMC_EC_INT (+_AK) | VMC exception-code interrupt |
| 15+ | — | (the Rage II/II+ bus-master end-of-list enable and acknowledge, [2] §8.10.3; bit positions not in the evidence set, §6.5) |

`BUS_CNTL` carries the debug interrupt enables for the command FIFO error and host data error
([1] p. 4-8; [mach64.md](mach64.md) §3.8). The single INTA# pin's Macintosh wiring is the
platform's: the slot's four pins OR-combine into one slot line ([pci.md](../pci.md) §3.5), the
driver installs on the interrupt source tree ([pci.md](../pci.md) §4.5), and dispatch among the
chip's sources is by register reads. The observed Macintosh driver surface is the VBL path: the
Xclaim 3D's embedded driver imports `InstallInterruptFunctions`/`GetInterruptFunctions`/
`VSLNewInterruptService`/`VSLDoInterruptService` and carries `DoVBLService` and a
`MyInterruptHandler` [3].

### 3.10 The hardware cursor

The cursor is a 64x64x2 sprite — "Hardware cursor up to 64 x 64 x 2" in Apple's description [4]
— with the mach64 core's encoding ([mach64.md](mach64.md) §3.9). The GT-era register detail [1]
pp. 4-33-4-37: "The hardware cursor may be any size up to 64x64 pixels. The cursor pitch is always
64 pixels meaning the cursor definition is always 64 pixels wide although pixels outside of the
visible cursor area are ignored"; the block lives at `CUR_OFFSET` (in 64-bit words); position is
`CUR_HORZ_VERT_POSN` with `CUR_HORZ_VERT_OFF` offsetting into the block for clipped cursors; and
the two colours are 24-bit RGB triples to the internal DAC — `CUR_CLR0_R/G/B` and `CUR_CLR1_R/G/B`
— with the palette *index* form (`CUR_CLR0_8`) used only on the feature connector in 4/8 bpp
[1] p. 4-33. The enable is `GEN_CUR_ENABLE` at `GEN_TEST_CNTL` bit 7 [1] p. 4-14.

Unlike the Apple GX card, where Mac OS draws the cursor in software and the firmware never enables
it ([mach64.md](mach64.md) §3.9), the Rage II generation's Macintosh driver uses the hardware
cursor: the Xclaim 3D's embedded driver imports
`VSLPrepareCursorForHardwareCursor` from VideoServicesLib and carries `SetCrsrImage`,
`SetCrsrState`, `CrsrDACBlast`, `DoSetHardwareCursor`, `DoSupportsHardwareCursor` and
`DoGetHardwareCursorState` [3] (*observed*).

## 4. Programming model

### 4.1 PCI identity and configuration space

| Field | Value | Evidence |
|---|---|---|
| Vendor ID | $1002 | default, "ATI's assigned PCI vendor ID" [1] §7-1; *observed* [3] |
| Device ID | $4754 ('GT') | default "or 4754h (ASCII characters 'GT')" [1] §7-1; *observed* [3] |
| Class code | $030000 display controller | *observed* [3] |
| ASIC ID (offset 08h) | major ASIC version (A=0), foundry (000 = SGS, 001 = NEC, 010 = KSC), minor revision — "also appears in the CONFIG_CHIP_ID register" [1] §7-2 | [1] §7-2 |
| Command | I/O Access Enable and Memory Access Enable R/W (default 0); the VT's Bus Master Enable is "Always 0, disabled" [1] §7-1 — see §2.12 for the Rage II/II+ | [1] §7-1 |
| BAR0 (10h) | memory aperture base, 32-bit, default $00000000 [1] §7-3; the card publishes a 16 MB BAR (*observed* [3]) | [1] §7-3; [3] |
| BAR1 (14h) | block-decoded I/O base, low byte "Always 01h" [1] §7-3 | [1] §7-3 |
| Expansion ROM BAR (30h) | ROM base address [1] §7-3 | [1] §7-3 |

The card's own FCode touches configuration space through byte and word accesses — `config-b@`,
`config-b!`, `config-w@`, `config-w!` — interleaved with `map-in`/`map-out` calls to the parent
bridge, five windows mapped and unmapped over the bring-up (*observed* [3]). No live
configuration-space dump of the card exists, so the command register's runtime value, the
assigned BAR addresses and the interrupt pin readback are unobserved (§6.10).

### 4.2 The expansion ROM and the FCode bring-up

The Xclaim 3D's expansion ROM [3] is a 64 KB image with the standard PCI ROM header and a PCI data
structure at offset $20: vendor $1002, device $4754, class $030000, code type $01 (Open Firmware),
indicator $80 (last image), declared image length 52,736 bytes (*observed* [3]). The FCode program
begins at image offset $40: format $08, declared length 52,477 bytes, and the detokenized walk
consumes exactly the declared byte count and terminates cleanly [3].

The bring-up's observed shape (*observed* [3]): the program opens by defining 293 FCode words, the
first of which are constants — the register dword selects (0, 2, 4, 8, 0A, 0C, 0E, 14, 18, 1C, 1F,
20, 24, 2C, 34, 40, 44, 48 ...) that later register writes are computed from; it carries the
sparse-I/O base $2EC and the register-block base $7FFC00 as literals; it reads the CPU identity
(`AAPL,cpu-id`); and it bails with "Display not installed" when no monitor answers. The program
also carries a self-test battery — "Test hardware registers - ", "Test RamDAC - ", "Test Frame
buffer - " with per-address " failed at address:" / " passed Ok" reports — and the mode words
`show-modes`, `set-mode` and `mode#` with "Not enough memory to support" / "Mode not supported"
rejections. What is *not* yet established is the register-init program itself: unlike the Apple GX
card, whose FCode walks a literal (address, value) table ([mach64.md](mach64.md) §4.3), this
ROM's register writes are computed, so no init table can be transcribed without a full annotated
decode — a gap recorded in §6.10.

### 4.3 The published properties

The FCode's Open Firmware node, as detokenized [3] (*observed*):

| Property | Value |
|---|---|
| name | "ATY,mach64_3D" |
| model | "ATY,GT-B" |
| device_type | "display" |
| character-set | "ISO8859-1" |
| iso6429-1983-colors | (built at run time) |
| width / height / linebytes | computed at run time |
| depth | the literal 8 |
| reg | one 32-bit memory BAR, size $01000000 (16 MB) |
| EDID | a 128-byte buffer |
| ATY,Rom# | "113-39203-124" |
| ATY,Mem# (Upgrade to 6MB) | "100-401015" |
| ATY,Mem# (Upgrade to 8MB) | "100-401017" |
| ATY,Card# | "109-39200-00" |
| ATY,Fcode | "APL-1.0b42" |
| ATY,Status / ATY,Flags / address | run-time values |
| driver,AAPL,MacOS,PowerPC | the embedded PEF driver |

The display method set is the standard one: `dimensions`, `color@`, `color!`, `set-colors`,
`get-colors`, `fill-rectangle`, `draw-rectangle`, `read-rectangle`, `show-modes`, `set-mode`,
`self-test` [3]. Two properties deserve their quirks' entries: `depth` is again published as the
literal 8 (the GX card behaves identically, [mach64.md](mach64.md) §5), and the two `ATY,Mem#`
properties carry the upgrade qualifier inside the property *name* (§5).

### 4.4 The embedded Mac OS driver

The `driver,AAPL,MacOS,PowerPC` property is a PowerPC PEF container (magic "Joy!peff" with the
"pwpc" architecture) embedded in the FCode as a chain of encode-bytes segments [3]. Its loader
section imports exactly four libraries — `NameRegistryLib` (11 symbols), `DriverServicesLib`
(14), `PCILib` (2) and `VideoServicesLib` (4) — and exports `TheDriverDescription` and
`DoDriverIO` (*observed* [3]). The imports name the driver's whole contract: Name Registry
property management, `ExpMgrConfigReadLong`/`ExpMgrConfigWriteLong` for PCI configuration
access, `VSLNewInterruptService`/`VSLDoInterruptService` for the VBL interrupt,
`VSLPrepareCursorForHardwareCursor` for the hardware cursor, and the driver-services block
(`DelayForHardware`, `PoolAllocateResident`, `BlockCopy`, `SetProcessorCacheMode`,
`DurationToAbsolute`, `IOCommandIsComplete`) [3].

The driver's own function-name table — the most direct statement of what the shipped Macintosh
software does with this silicon (*observed* [3]) — dispatches the standard ndrv command set
(`DoOpenCmd`, `DoCloseCmd`, `DoControlCmd`, `DoStatusCmd`, `DoInitializeCmd`, `DoReplaceCmd`,
`DoFinalizeCmd`, `DoKillIOCmd`, `DoSupersededCmd`), then divides by subsystem: monitor
identification through both sense lines and DDC (§3.7); mode and CRTC programming
(`SetCRTMode`, `SetMode`, `InternalSetVideoMode`, `DoSwitchMode`, `HALSetupMyDisplayT`,
`HALGetTimingFlags`, `HALGetMonitorInfo`, `HALGetCrtcValues`); the clock tree (§3.8); memory
sizing (`TestMemSize`, `GTB_TestMemSize`, `MemSizing`, `GetFrameMemOffset`) — with the GT-B
specific routine named as such; the draw engine (`resetengine`, `initengine`, `mWaitforidle`,
`mWaitforemptyfifo`); the DAC and gamma path (`M64DacRIdx`, `M64DacWIdx`, `M64SetDac`,
`M64GetDac`, `HALSetDACWithTable`, `HALSetDACWithAllGray`, `HALSetDACWithLinearRamp`,
`HALSetDACGamma`, `DoSetGamma`, `DoGammaCorrectCLUT`, with four built-in tables — "Mac Std
Gamma", "Page-White Gamma", "Mac RGB Gamma", "Mac Gray Gamma"); the hardware cursor (§3.10);
DDC/EDID (§3.7); and a QuickDraw 3D hook, `DoATISet3DCallback`. The byte-swap accessors of §3.1
sit underneath all of it.

### 4.5 Mode setting and monitor identification

The onboard G3 target's mode set is documented in full by Apple's developer note [4]: at power-on
"the monitor is initially set for a display size of 640 by 480 pixels", with on-the-fly switching
from the Control Strip or Monitors & Sound panel; the supported pixel sizes run 512x384 through
1600x1200 (the Rage Pro adds 1920x1080), with maximum depths per memory size — 32-bit at
1152x870 on 2 MB, 32-bit to 1280x1024 on 4 MB, 32-bit at 1280x1024 on 6 MB, and 16-bit at
1600x1200 (8-bit on 2 MB) [4] Tables 3-1, 3-2. The video-input/acceleration mode table adds the
3D constraint: QuickDraw 3D acceleration depends on depth and window size (8-bpp modes get 2D
acceleration only; 640x480 at 32 bpp gets 3D "depending on application window size") [4]
Table 3-3. The card-side mode contract is the FCode's `show-modes`/`set-mode` pair with its
"Mode not supported" and "Not enough memory to support" rejections [3] — the same rejection
discipline the GX card's FCode carries ([mach64.md](mach64.md) §4.2).

### 4.6 The Mac OS software stack

The retail software record [7]: the Xclaim 3D driver updates (2.01, 2.1 of December 9, 1997 and
3.1 of March 19, 1998, each "for use with the XCLAIM 3D only"); the Xclaim VR updates (2.01, 2.1,
3.0 of December 11, 1997 and 3.1, "for use with the XCLAIM VR and XCLAIM TV"); the **ATI MPEG
Accelerator 2.0** (June 6, 1997), which is "for use with the XCLAIM VR and XCLAIM 3D" and whose
installer drops the ATI MPEG Accelerator extension into the Extensions folder; the Xclaim VR
installer disk image; and the universal installers and OS-bundled ATI extension sets that
carry the same driver family to the mach64-era cards. One system-interaction note is on record:
Xclaim 3D users on an Apple 7200 with sound corruption must additionally install the ATI Sound
Catal extension [7] — a driver-level workaround, not silicon behaviour, and the one observed
trace of the Xclaim 3D population inside the Catalyst-generation machines.

### 4.7 The 3D and video paths under Mac OS

Apple's account of the part under Mac OS [4]: "Software support through Macintosh QuickDraw 3D
and QuickDraw 3D RAVE (rendering acceleration virtual engine) APIs"; the accelerator
"Accelerates 3D QuickDraw rendering up to 6 times that of software-only rendering" with
"real-time 3D shaded object manipulation, animation, and virtual world navigation"; a 16-bit Z
buffer; "six perspectively correct texture mapping functions"; "alpha blending, transparency,
and fog effects"; "flat and Gouraud shading"; and "video textures and video lighting" —
the SCALE_3D_CNTL feature bits of §2.10 rendered into marketing English. The QuickTime path is
"hardware acceleration of video for full screen, full motion, TV-quality playback of Cinepak and
MPEG1 QuickTime movies" with "bilinear hardware interpolation and scaling" [4] — and the MPEG
Accelerator extension is the software that drives it [7], with the scaler's host-memory source
mode (§3.5) as the register-level mechanism. The 6500 launch material claims the same
capabilities for the onboard Rage II [5]. No recorded run-time observation of any of this exists
(§6.9).

## 5. Quirks & errata

- **The scaler pipe is a set of aliases onto the 3D engine.** `SCALE_X_INC` *is* `RED_X_INC`
  (0_7C and 0_F0), `SCALE_Y_INC` *is* `GREEN_X_INC` (0_7D/0_F3), `SCALE_Y_PITCH` *is* `S_Y_INC`
  (0_7B/0_D4), and the texture offsets sit on the scaler offsets (§2.10) [1] Ch. 3, §6-1. A model
  that implements them as separate registers breaks both scaling and texturing.
- **The Bresenham registers moved and doubled.** The GX's `DST_BRES_LNTH` at 0_48h is aliased to
  `LEAD_BRES_LNTH` at 0_51h, the error/inc/dec trio is renamed LEAD_*, and a TRAIL_* trio exists
  for the trapezoid (§2.9) [1] Ch. 3 — a GX-derived driver's line code addresses the wrong
  flip-flops if the aliases are missed.
- **Register writes must be dwords.** "Writes to the memory mapped registers must be performed in
  one 32-bit write" on the mach64CT family; reads may be 8/16/32-bit [2] §2.3.3 — stricter than
  the GX ([mach64.md](mach64.md) §5).
- **Two FIFO counters, one FIFO.** FIFO_STAT reports "full entries in the top half" as one bit
  per entry; GUI_STAT's FIFO_CNT reports the empty count, at most 32 of the 48-entry parameter
  FIFO (§3.2) [1] pp. 4-101, 4-106. Equating them miscomputes the free space.
- **CRTC_DISP_REQ_ENB defaults to 1 — display fetch disabled.** A mode set that enables the CRTC
  without clearing bit 22 of CRTC_GEN_CNTL gets a blank display with a running CRTC [1] p. 4-27.
- **CONFIG_STAT0 became writable.** The cross-reference lists it R/W on the VT/GT [1] Ch. 3,
  where the GX's is read-only and the Apple GX firmware's discard-write is a documented oddity
  ([mach64.md](mach64.md) §5). Code carrying the GX assumption can corrupt this generation's
  strap state.
- **The aperture is PCI's business, not the firmware's.** CFG_MEM_AP_SIZE is read-only "2x8 MB"
  and CFG_MEM_AP_LOC is fixed by the BARs [1] p. 4-16 — the GX card's three CONFIG_CNTL
  aperture writes ([mach64.md](mach64.md) §4.2) have no equivalent, and a ROM that performed them
  would be writing read-only fields.
- **Bus mastering is generation-gated.** The VT's command register hard-documents Bus Master
  Enable as "Always 0" [1] §7-1, and the Rage II/II+ documents the capability and its registers
  [2] §8.10 — a model that hard-wires the bit for the whole family breaks Rage II features, and
  one that enables it for a VT is out of spec.
- **Only two bits of CLOCK_SEL mean anything.** The internal synthesizer has four pixel-clock
  settings, so "only bits 0 and 1 of CLOCK_SEL are used" [1] p. 4-38 — the upper bits of the
  field are not a clock select.
- **YUV422 is raw dword data to the 2D engine.** Only the Scaler/3D block differentiates the
  format (§3.3) [1] p. 4-93 — a plain blit of a YUV422 surface moves data correctly but never
  converts it.
- **The feature connector is not for deep modes.** DAC_FEA_CON_EN "should be disabled in 24 bpp
  and 32 bpp modes and high resolution modes" [1] p. 4-40.
- **The card publishes depth 8 literally** — the FCode's `depth` property is the constant 8, as
  on the GX card ([mach64.md](mach64.md) §5) (*observed* [3]).
- **Property names can carry qualifiers**: the upgrade module part numbers are published as
  properties *named* `ATY,Mem# (Upgrade to 6MB)` and `ATY,Mem# (Upgrade to 8MB)` (*observed*
  [3]) — Name Registry code that matches `ATY,Mem#` by exact string finds neither.
- **Every 16/32-bit register access is byte-swapped by the Macintosh driver** (§3.1) — the
  registers are little-endian and the host is not ([pci.md](../pci.md) §3.4) (*observed* [3]).

## 6. Open questions

1. **The GT-B's own register documentation.** No chip-level specification or register reference
   for the Rage II (GT-B) is in the evidence set — the 1996 register reference covers the GT-A2
   3D RAGE [1] and the programmer's guide carries only feature-level deltas [2]. Whether GT-B
   adds or changes registers beyond those deltas (the source-FIFO depth 16x32 and 83 MHz memory
   are stated as characteristics, not as register contents) is unestablished.
2. **CONFIG_CHIP_ID and CONFIG_STAT0 for the VT/GT.** The bit charts at [1] pp. 4-17-4-18 did
   not survive text extraction; the identification fields of this generation are known only
   through the PCI ASIC ID byte [1] §7-2 and the query-structure ASIC-type list [2] Table A-6.
3. **Which silicon generation the Macintosh parts carry.** The Xclaim 3D's ROM reports device
   $4754 with model `ATY,GT-B` (*observed* [3]); Apple calls the onboard part "3D RAGE II+DVD"
   [4]; the programmer's guide assigns $4755 to the II+ (GTB) [2] Table 3-1. Whether the retail
   cards and the onboard parts are Rage II or Rage II+ silicon — and whether the card's two
   upgrade modules imply the II+'s SGRAM block write — is not established.
4. **The Apple-branded Rage II card.** The Apple-labelled member of the card family (board and
   ROM part numbers, bundled machines, memory configuration) has no primary document in the
   evidence set; the page's card table therefore carries only the retail cards.
5. **The bus-master register offsets.** `BM_ADDR` is documented at MM offset 0x92 [2] §8.10.4,
   but the offsets of `BM_SYSTEM_TABLE`, `BM_GUI_TABLE` and the completion-status fields, and
   the CRTC_INT_CNTL bit positions of the bus-master end-of-list interrupt [2] §8.10.3, are not
   in the evidence set.
6. **SGRAM programming.** The documented MEM_CNTL chart covers DRAM and SDRAM [1] p. 4-10;
   the Apple parts run SGRAM [4] and the II+ adds SGRAM block write [2] Table 7-1, but the SGRAM
   mode, block-write-enable bit and timing fields of this generation are not register-documented
   here — the driver's `VTB_ResetSDRAM` is observed as a name only [3].
7. **XCLK.** The embedded driver distinguishes XCLK from MCLK (`VTB_XCLKvsMCLK`, `VTB_GetXClk`)
   [3], but the documented PLL file programs only MCLK and VCLK [1] App. B; the XCLK register
   surface and its relationship to MEM_CLK are unknown.
8. **The Xclaim VR board.** Its ROM, its video decoder/encoder companion chips, connectors and
   capture capabilities are not in the evidence set; the card is known from its software record
   [7] and by inference from the silicon's video port.
9. **Run-time behaviour.** No configuration-space dump, no recorded Mac OS boot, and no engine
   traffic log exists for any Rage II card — everything about what the shipped Macintosh driver
   actually writes (context loads, engine patterns, overlay programming, bus-master use, the VMC
   port's use on any Mac card) is unobserved, where the mach64 GX card has a full log
   ([mach64.md](mach64.md) §4.8).
10. **The FCode register-init program.** The Xclaim 3D's register writes are computed rather
    than literal, so no init table can be transcribed without an annotated decode of the
    293-word FCode program; the bring-up sequence beyond its observed shape (§4.2) — command
    register value, I/O window mapping, sense/DDC order, mode selection — is undecoded.
11. **The DDC pin pair.** The three DAC GIO pins are the sense pins and DDC1/DDC2B+ is claimed
    [4], but which pins carry the DDC clock and data, and how they are muxed against the
    AppleSense lines, is not documented for this generation (the Rage 128 answers explicitly —
    [rage-128.md](rage-128.md) §3.6).
12. **The big-endian extended linear aperture.** The programmer's guide's product table lists
    an "Extended Linear Aperture (big endian)" for the CT/VT generations [2] Table 1-1, but the
    GT column's marking did not survive the table's extraction; whether the GT has the mode, and
    whether any Apple board uses it, is untested.
13. **The Gazelle onboard wiring.** The 5500/6500 developer note is present only as a page-image
    PDF [6]; the onboard Rage II's memory size, connector, sense wiring and node properties are
    known only from the node path [8], the press-release feature list [5] and the connector
    pin-assignment continuity stated by the G3 note [4].
14. **Draw-engine, scaler and 3D power-on values.** Undocumented for the GT as for the GX
    ([mach64.md](mach64.md) §2.11); reachable by any driver that draws before full setup.
15. **"II+DVD" as a name.** No ATI document defines a II+DVD silicon variant (§1.2); whether the
    name denotes a stepping of the Rage II+ or a board feature set is unresolved by the
    evidence.

## References

1. ATI Technologies Inc., *mach64 Register Reference Guide: ATI-264VT and 3D RAGE*, P/N
   RRG-G02700 Rev. 0.10, preliminary draft, July 1996 — Chapter 1 (VT and 3D RAGE features and
   register classes, the trapezoid trajectory); Chapter 2 (aperture modes, register blocks 0/1,
   sparse and block I/O mapping, the non-Intel note); Chapter 3 (the cross-reference tables:
   class, mnemonic, R/W, I/O select, dword offset, page); Chapter 4 (accelerator register
   reference, cited by page); Chapter 5 (multimedia registers: overlay, video input, capture,
   buffers, VMC); Chapter 6 (the 3D RAGE scaler and 3D operations registers); Chapter 7 (PCI
   configuration space registers); Appendix B (programming the PLL registers, with the PLL
   register table and defaults).
2. ATI Technologies Inc., *RAGE PRO and Derivatives Programmer's Guide*, P/N PRG-215R3-00-10
   Rev 1.0, March 2000 — §1.2.4 and Table 1-1 (mach64 product families and their feature
   variations); §1.3 and §1.3.2 (the mach64CT family and the mach64GT generation, motion
   compensation's Rage Pro scope); §1.3.5 (RAGE MOBILITY's iDCT); §2.3 (non-Intel architecture:
   memory map, register access widths); §3.3.4 (CONFIG_CHIP_ID); Table 3-1 (PCI device ID
   codes); §7.9.6-§7.9.7 and Table 7-1 (block write; chip characteristics: FIFO sizes, page
   costs, memory speeds, block write by generation); §8.9 (front-end scaler programming);
   §8.10 (bus master programming: descriptor entries, system and GUI transfers); Table A-6
   (the BIOS query structure and the ASIC identification codes).
3. The Macintosh 3D Rage II card ROM record — the ATI Xclaim 3D expansion ROM `M64_3D_1.F10`
   (board 109-39200-00, bitsavers archive 109-39200-00, with front and back PCB photographs):
   the 64 KB image's PCI data structure (vendor $1002, device $4754, class $030000, code type
   $01, indicator $80, image 52,736 bytes), the detokenized FCode program (format $08, 52,477
   bytes, walk-verified; the published property list including `name`/`model`/`ATY,Rom#`/the
   two `ATY,Mem#` upgrade properties/`ATY,Card#`/`ATY,Fcode`/`reg`/`EDID`; the register-select
   constants and the $2EC and $7FFC00 literals; the self-test and mode-word battery), and the
   embedded `driver,AAPL,MacOS,PowerPC` PEF with its four-library import list and the
   function-name table of the driver (sense, DDC/EDID, PLL and MCLK/XCLK, SDRAM reset and
   memory sizing, engine, cursor, gamma and QuickDraw 3D routines).
4. Apple Computer, Inc., *Developer Note: Power Macintosh G3* (beige, "Power Macintosh G3
   Computers"), 1998 — §"Built-in Video and Graphics Features" (the 3D RAGE II+DVD / PRO
   alternation by ROM subversion, the acceleration, QuickDraw 3D, Z-buffer and video feature
   lists); §"Custom ICs" and §"Graphics Controller" (the part as a core IC, its SGRAM
   architecture, memory controller, video scaler, colour space converter, clock generator, DAC,
   cursor and DDC support, the MEM_CLK and dot clock description, the 64-bit display memory
   bus); §"Video RAM Expansion" (the 2 MB base and 4/6 MB SO-DIMM); §"Built-in Display Video"
   and Tables 3-1 to 3-3 (the display mode, depth and acceleration tables); §"Apple DVD-Video
   and Audio/Video Card" (the DVD playback path and its requirement).
5. Apple Computer, Inc., press release, "Apple lance le premier micro-ordinateur a 300 MHz"
   (Apple Canada, Markham, Ontario, April 4, 1997) — the Power Macintosh 6500 announcement: the
   onboard "accelerateur graphique RAGE II d'ATI" with QuickTime and QuickTime MPEG, QuickDraw
   and QuickDraw 3D acceleration, and accelerated video capture.
6. Apple Computer, Inc., *Developer Note: Power Macintosh 5500/6500*, 1997 — the onboard ATI
   3D Rage II video subsystem of the Gazelle platform (present in the evidence corpus as a
   page-image PDF; cited for the subsystem's presence, not page-level detail).
7. The ATI Macintosh driver-archive record — the Xclaim 3D software updates 2.01, 2.1 (readme
   dated December 9, 1997) and 3.1 (readme dated March 19, 1998); the Xclaim VR software updates
   2.01, 2.1, 3.0 (readme dated December 11, 1997) and 3.1; the ATI MPEG Accelerator 2.0 readme
   (June 6, 1997; "for use with the XCLAIM VR and XCLAIM 3D"); the Xclaim VR installer disk
   image; and the universal installer and OS-bundled ATI extension sets sharing the archive.
8. The NetBSD/macppc Model Support list — the "Apple Performa 5500 and 6500" entry: the
   on-board video `output-device` `/bandit/ATY,264GT-B`.
9. Apple Computer, Inc., AppleCare Tech Info Library article 30355, "Power Macintosh G3:
   128-Bit 2D/3D Professional Graphics Card", created January 23, 1998, modified June 1, 2000 —
   the optional card's specification and display mode table.
