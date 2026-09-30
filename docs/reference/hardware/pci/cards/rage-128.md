# ATI Rage 128 VR and GL

The **ATI Rage 128** is the 1998 generation of ATI's 128-bit graphics and multimedia accelerators: a
single-chip, 0.25 µm controller integrating a 2D engine, a 3D rendering pipeline with a floating-point
setup engine, a video overlay scaler, an integrated triple-palette DAC, and a programmable PLL set,
wired to a PCI 2.1 or AGP bus interface [1] §1.1. On the Power Macintosh it is a **PCI card
subject**: the machines this page's hardware runs in are the Old World PCI Power Macintoshes and the
beige Power Macintosh G3 ([pci.md](../pci.md) §1.2), whose platform contract — the three address
spaces, the configuration header, the big-endian host to little-endian bus seam, the per-slot
OR-combined interrupt — is [pci.md](../pci.md)'s and is cited here, never restated. The cards that
carry the chip are ATI's retail Macintosh boards: the **Rage Orion** and **Nexus 128** (Rage 128 GL,
the Mac-specific gaming and workstation cards), the **Xclaim VR 128** (the same silicon plus capture
and TV hardware), and the Apple OEM card of the Blue and White Power Macintosh G3 (§1.2); the later
Rage 128 **Pro** appears in Apple hardware only on AGP machines and is covered here as the family's
superset member, not as a PCI card.

The Rage 128 is a *new* register architecture, not an extension of the mach64 line that preceded it
in Apple hardware ([mach64.md](mach64.md) §1.1): nothing in its register file is
mach64-compatible, and its own documentation reaches back to the mach64 Programmer's Guide only for
the general subject of non-Intel bus configuration [2] §2.2.3. What the two generations share is the
Macintosh-facing problem they were both built to solve — a little-endian PCI device on a big-endian
host — and the Rage 128's answer, two identical copies of every aperture so the Power Macintosh can
map each independently, is the fact that shapes this whole page (§3.1).

**Contents:**

1. [Overview](#1-overview) — what the parts are, the cards on the Power Macintosh, host machines and
   the on-board fits, the evidence base
2. [Register file](#2-register-file) — the four apertures and the BAR set; the memory-mapped register
   map; PCI configuration space; configuration, CRTC/DAC, clock/PLL, memory-control, 2D-engine,
   surface, CCE, bus-master and interrupt registers; reset state
3. [Behaviour](#3-behaviour) — endianness, aperture semantics, the command FIFO, the 2D engine, the
   CCE and bus mastering, monitor sensing, clocking, interrupts, the hardware cursor
4. [Programming model](#4-programming-model) — PCI identity and the expansion ROM, the FCode bring-up,
   mode setting, engine discipline, the Mac OS driver stack, silicon revisions and the errata ladder
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the parts are

The Rage 128 family splits by memory interface width and package [1] Table 2-1:

| Chip ID | ASCII | Package | Bus | Memory interface |
|---|---|---|---|---|
| 0x5245 | 'RE' | 328-pin BGA | PCI 33 MHz | 128-bit — **Rage 128 GL** |
| 0x5246 | 'RF' | 328-pin BGA | AGP 1x/2x | 128-bit — Rage 128 GL |
| 0x524B | 'RK' | 272-pin BGA | PCI 33 MHz | 64-bit — **Rage 128 VR** |
| 0x524C | 'RL' | 272-pin BGA | AGP 1x/2x | 64-bit — Rage 128 VR |
| — | — | 329-pin BGA | AGP | 64-bit — Rage 128 VR-O (upgrade-path package) |

The vendor ID is 0x1002 (ATI) for all [2] §4.1. Every Macintosh PCI card of the family is the
**GL, device 0x5245** — the 328-pin, 128-bit-memory part; the VR members are the motherboard and
mainstream-PC packages, and the register reference describes 0x5245 as "312+16 BGA, PCI" [2] §4.1,
counting substrate and die balls separately for the same 328-ball package. The Software Development
Guide's own ID table notes the PCI parts are "PCI 33 only" [3] Table 3-1.

The generation's feature set, as its controller specification states it [1] §2.1:

- **2D** — a 128-bit engine accelerating BitBlt, line draw, polygon/rectangle fill, bit masking,
  monochrome expansion, panning/scrolling, scissoring, "full ROP support (including ROP3)", font
  and text handling, DirectDraw-style double buffering, transparent and masked blits, and
  8/16/24/32 bpp operation [1] §2.1.2; a hardware cursor up to 64x64x2.
- **3D** — a setup engine "capable of processing 3 million unculled triangles/s and 1.5 million
  culled triangles/s" that "accepts all relevant parameters in IEEE floating point format"; an 8 KB
  two-way-associative on-chip texture cache; a superscalar (2 pixel/clock) rendering engine;
  16-, 24- or 32-bit Z (24-bit maximum when the 8-bit stencil buffer is enabled); 4 bits of
  subpixel and subtexel accuracy; per-pixel mip-mapped perspectively-correct texturing with
  bilinear and trilinear filtering, chroma key, Gouraud and specular shading, alpha blending with
  simultaneous fog, line and edge anti-aliasing, and 16-bpp dithering [1] §2.1.3. Draw formats are
  RGBA32, RGBA16 and RGB16; texture formats add RGB8, ARGB4444, YCrCb444 and the compressed
  YCrCb422, CLUT4, CLUT8 and VQ modes [1] §2.1.3.
- **Video** — a front-end scaler and overlay with 4-tap horizontal and 2-tap vertical filtering for
  RGB, 4-tap both ways for YCrCb upscaling, an enhanced line buffer covering the ATSC resolutions,
  colour-space conversion, and hardware MPEG-2 assistance: motion compensation, iDCT, and a DVD
  subpicture decoder [1] §2.1.4. A 16-bit video port (VIP 1.1 or ATI's AMC) connects TV-out and
  capture companion chips [1] §2.1.5 — this is board territory: only the Xclaim VR 128 carries it
  in the Macintosh card population (§1.2).
- **Memory** — 2 MB to 32 MB of SGRAM or SDRAM, single-data-rate or (on the VR) DDR, "at up to
  125 MHz memory clock providing bandwidths up to 2 GB/s across a 128-bit interface" [1] §2.1.1;
  the supported geometries and the address-mapping encodings are §2.6's.
- **Display** — a "triple 8-bit palette DAC with gamma correction for true WYSIWYG color. Pixel
  rates up to 250 MHz standard, optional 230 MHz", 4/8/15/16/24/32 bpp display depths, "DDC1 and
  DDC2B+ for Plug-and-Play monitors, and AppleSense monitor detection support" [1] §2.1.6 — both
  monitor-identification disciplines, the Mac's and the PC's, in one pin set (§3.6).
- **Host interface** — "PCI version 2.1 with full bus mastering and scatter/gather support" and
  "bi-endian support for compliance on a variety of processor platforms" [1] §2.1.1; a 128-entry
  command FIFO feeding 32-bit memory-mapped registers.

The 3D pipeline is fed by the **Concurrent Command Engine (CCE)**, a microcoded parser that reads
command packets from a ring buffer in system memory over PCI bus mastering and drives the 2D, 3D and
multimedia engines from them (§3.5). The marketing overview adds the trademarked names — SuperScalar
Rendering, Twin Cache Architecture, Single-Pass Multi Texturing — and the DirectX 6 feature
alignment (multitexturing, stencil planes, bump mapping, vertex buffers) [4].

The **Rage 128 Pro** (1999) is the superset successor: same register architecture, "based on the
RAGE 128 VR/GL Registers specification" with an appended change list [5] Appendix B, new device IDs
('PF' = 0x5046, 385-ball AGP 4x with TMDS; 'PR' = 0x5052) [5] §4.1, a faster setup engine, the flat
panel and TMDS register blocks, and the auxiliary-window and pad-control registers of its Appendix
B. In Apple hardware it is the AGP generation (Power Macintosh G4 "Sawtooth", Cube, iMac DV); no
Pro-based PCI card for the Old World machines is in the evidence set, and the Pro's own silicon is
out of this page's scope beyond the corrections it applies to the GL's register definitions (§4.6).

### 1.2 The cards on the Power Macintosh

| Card | Chip / PCI ID | Memory | Video connector | Expansion ROM | Notes |
|---|---|---|---|---|---|
| **ATI Rage Orion** | Rage 128 GL, 1002:5245 | 16 MB, 128-bit interface [14] p. 32 | VGA (DB-15 via adapter) [14] p. 33 | not held; board family 109-57400 (*inferred — unverified*, §6.1) | retail Mac gaming card; 2D to 1600x1200 in 32-bit colour [14] p. 1 |
| **ATI Nexus 128** | Rage 128 GL, 1002:5245 (*observed* in the ROM's PCI data structure [15]) | 32 MB SDRAM, 128-bit interface [13] p. 32 | VGA [13] p. 33 | 113-57502-103, FCode 1.68, board 109-57500-00 (*observed* in the FCode [15]) | 2D to 1920x1200 in millions of colours [13] p. 1 |
| **ATI Xclaim VR 128** | Rage 128 GL, 1002:5245 (*observed* [15]) | 16 MB (*inferred — unverified*, secondary card tables) | VGA + video in/out | 113-57406-108, FCode 1.68, board 109-57400-00 (*observed* [15]) | adds the capture/tuner/TV-out board hardware of §1.1's video port |
| Apple accelerated graphics card (Blue & White Power Macintosh G3) | "ATI 3D RAGE 128 GL" [16] §"Graphics Card" | 16 MB SDRAM soldered [16] | — | not in the evidence set (§6.9) | the New World machine's bundled card |

Both retail user's guides state the same board-level envelope: a 32-bit PCI local bus "compliant
with PCI version 2.1 specification", separate and composite sync at TTL levels, a "relocatable
memory aperture" (16 MB on the Orion, 32 MB on the Nexus 128), a standard VGA connector with an
Apple-to-VGA adapter supplied, one PCI interrupt request, +5 V at 1.3 A typical, and a 120,000-hour
MTBF [13] pp. 32-33, [14] pp. 32-33. The Nexus 128 guide's requirements are the Macintosh software
contract in miniature: "Power Macintosh or Mac OS compatible computer, with a PCI expansion slot",
Mac OS 8.0 or later, QuickDraw 3D 1.5.4 and QuickTime 3.0 [13] p. 2.

The three retail cards are one design in three dressings: the same Rage 128 GL device ID, the same
128-bit memory interface, the same FCode structure, differing in memory size and in the video
peripherals. The Orion's own ROM has not been captured; the Xclaim VR 128's ROM (card number
109-57400-00) is the closest stand-in because the Orion's board number is documented in community
card tables as 109-57400-10 — the same 109-57400 board family (*inferred — unverified*; no Orion
board or ROM has been examined, and the claim rests on secondary card tables [15]). The two cards'
software differs in one observed byte of the FCode: the published device name is `ATY,Rage128v` on
the Xclaim VR 128 and `ATY,Rage128n` on the Nexus 128 [15].

### 1.3 Host machines and the on-board fits

An Old World PCI Power Macintosh — the 7500/8500/9500 generation and its 7300/7600/8600/9600
follow-ons, the 7200, and the beige Power Macintosh G3 — is the Rage 128 card's native host: the
retail cards' installers and the Nexus 128 guide's requirements name exactly that population
[13] p. 2. Everything about the slot side of that machine is [pci.md](../pci.md)'s: the bus itself
(§1.5), the configuration-space architecture the card's header lives in (§2.4-§2.5), the firmware's
command-register policy (§2.6), byte order across the bridge (§3.4), the slot interrupt that
OR-combines the card's INTA# (§3.5), and the Open Firmware startup process that runs the card's
FCode and sizes its BARs (§4.1-§4.4). The host bridges are [bandit.md](../../../machines/tnt/bandit.md)'s;
the platform is [tnt.md](../../../machines/tnt/tnt.md)'s.

The on-board fit is the Blue and White Power Macintosh G3 (1999), a *New World* machine whose
bundled "accelerated graphics card" carries the same Rage 128 GL with 16 MB of SDRAM, QuickDraw 2D
and QuickTime video acceleration, a DVD-decode-module connector, bilinear interpolation and scaling,
16-bit Z buffering, six perspectively correct texture-mapping functions, alpha blending,
transparency and fog [16] §"Graphics Card". Its platform (Grackle, Heathrow, ROM-in-RAM) is outside
this page's scope; the card itself is the same silicon documented here, and the frequent retrofit of
these OEM cards into Old World 8600/9600/beige G3 machines is a community practice whose
boot-time behaviour is an open question (§6.9).

### 1.4 Evidence base

Every hardware fact on this page is derived from primary evidence, but the two halves of the evidence
are not equally deep. The **vendor corpus** is strong: the Rage 128 VR/GL Graphics Controller
Specifications (system level, packages, pins, straps, memory geometries, boot sequences) [1]; the
Register Reference Manual Rev 0.02 with its full per-register bit tables [2]; the Software
Development Guide with the initialisation, mode-setting, CCE and drawing sequences [3]; the Pro
register guide for the superset and the later corrections [5]; the bus-master and CCE/multimedia
register supplements [6] [7] [8]; and the four silicon-revision product advisories [9] [10] [11]
[12]. The **Macintosh record** is thin and is used honestly: four 128 KB card ROM images
(retail Nexus 128, retail Xclaim VR 128, two OEM Rage 128 Pro AGP), their PCI data structures and
detokenized FCode (unannotated — properties extracted, program flow not yet decoded), one
card-info utility dump of the OEM Pro ROM running on a real Mac, and the two retail user's guides
[13] [14]. There is **no** retail Rage Orion ROM, no config-space dump of any retail card, no
disassembly of the embedded Mac OS driver, and no recorded run-time observation of a Mac boot — so
where the page states what the shipped Mac software *does* (as opposed to what the hardware
*offers*), it says so and leans on the vendor documents; everything else is carried in
[§6](#6-open-questions). That is why this page's register file is dense and its programming model
is honest about its gaps.

## 2. Register file

The register reference organises the file into nine classes — setup/configuration, host interface
(PCI and AGP), VGA, accelerator CRTC/DAC, 2D engine, 3D engine, CCE, multimedia, miscellaneous [2]
§2.1 — and every register answers up to four access routes, printed in its own header as MMR
(register aperture 0), MMR_1 (register aperture 1), IOR (the I/O BAR) and IND (the MM_INDEX indirect
path). This section maps the apertures and the PCI header, then walks the classes; the complete
per-register bit tables are [2]'s chapters 3-8 with the supplement registers of [6] [7] [8], and the
per-register citations below give [2]'s chapter-page pointers.

### 2.1 The apertures and the BAR set

The chip exposes "a fully memory mapped programming model" [2] §2.2 with two register apertures and
two linear apertures, each pair being identical copies:

- **Register apertures 0 and 1** — "contain all the direct-accessed registers on the chip (except
  VGA and PCI configuration registers). They also have index/data pairs for all indirectly accessed
  registers and memories." Register aperture 0's base is the **REG_BASE** BAR at configuration
  offset 0x18; aperture 1's base is readable only in aperture 0, as **CONFIG_REG_1_BASE** at 0x010C,
  and "reading CONFIG_REG_1_BASE is the only method of determining the location of Register
  Aperture 1 that is forward compatible with future generations of hardware." The size comes from
  CONFIG_REG_APER_SIZE at 0x0110. The stated purpose: "For the PowerMac environment, this allows
  one aperture to be marked as cacheable" [2] §2.2.1.
- **Linear memory apertures 0 and 1** — frame-buffer images, bases in **CONFIG_APER_0_BASE**
  (0x0100) and **CONFIG_APER_1_BASE** (0x0104), size in **CONFIG_APER_SIZE** (0x0108); "For the
  PowerMac environment, this allows each to be independently marked as big-endian or
  little-endian." The frame-buffer image "occupies the area in each aperture from offset 0 to
  CONFIG_MEMSIZE-1" [2] §2.2.1. In AGP systems an image of AGP memory follows at AGP_APER_OFFSET —
  inert on PCI cards.

The BAR set as the PCI header lays it out [2] §4.1:

| Config offset | Register | Fields and notes |
|---|---|---|
| 0x10 | MEM_BASE | memory BAR, bits 31:26 (64 MB granularity), PREFETCH_EN (bit 3) default 1; the register text notes the field "Mirror[s] bits 6:1 of APER_1_BASE" — the BAR and the aperture-1 base share decode [2] §4.1 |
| 0x14 | IO_BASE | I/O BAR, bits 31:8; "Bits 7:1 of this field are hardwired to ZERO" [2] §4.1 |
| 0x18 | REG_BASE | register-aperture BAR, bits 31:14 (16 KB granularity) [2] §4.1 |
| 0x30 | BIOS_ROM | expansion-ROM BAR, bits 31:17 (128 KB granularity) plus BIOS_ROM_EN bit 0 [2] §4.1 |

The SDK guide's probe sequence isolates the same fields: mask 0xFC000000 for the memory BAR,
0xFFFFFF00 for I/O, 0xFFFFC000 for registers [3] §3.3. On the Macintosh the bases and sizes are the
firmware's to assign through these BARs ([pci.md](../pci.md) §4.3); the two linear apertures are
then steered by CONFIG_APER_0/1_BASE from inside the chip.

The register apertures themselves are laid out by offset [2] Table 2-1:

| Offset range | Contents |
|---|---|
| 0x0000-0x00FF | non-GUI registers — also directly accessible in I/O space |
| 0x0100-0x0EFF | non-GUI registers not reachable through I/O |
| 0x0F00-0x0FFF | read-only copy of the PCI configuration space |
| 0x1000-0x13FF | Concurrent Command Engine registers |
| 0x1400-0x1FFF | GUI (2D/3D engine) registers |

To this map the supplements add the CCE PM4 block at 0x0700-0x0738 [7], the bus-master block at
0x0A00-0x0AB0 [6], and the surface registers at 0x0B00-0x0B3C [2] §4.1 — offsets the base manual's
Table 2-1 predates. Access width rules come from the same table: byte and word addressing is not
supported for GUI registers — they are dword-only, as are the multimedia registers reachable in I/O
space — and word or dword cycles that span a dword boundary "will not work correctly in all cases"
[2] Table 2-2 and its notes.

Two indirect paths exist for the cases the apertures do not cover. **MM_INDEX** (0x0000) and
**MM_DATA** (0x0004) reach either aperture from I/O space: bit 31 of the index (MM_APER) selects
"0 = Register Aperture, 1 = Linear Aperture 0", and the index's low bits are hardwired to zero —
dword-only access [2] §4.1. And **CLOCK_CNTL_INDEX** (0x0008) / **CLOCK_CNTL_DATA** (0x000C) open
the PLL register file at PLL offsets 0x01-0x13 (§2.5).

### 2.2 PCI configuration space

The full standard header as the chip implements it [2] §4.1 (defaults are the register reference's
power-on column):

| Offset | Register | Value / behaviour |
|---|---|---|
| 0x00 | VENDOR_ID | 0x1002 [R] |
| 0x02 | DEVICE_ID | 0x524B shown as the reset value; the defined IDs are 'RE' 0x5245, 'RF' 0x5246, 'RK' 0x524B, 'RL' 0x524C [R] |
| 0x04 | COMMAND | IO_ACCESS_EN, MEM_ACCESS_EN, BUS_MASTER_EN (all default 0); SPECIAL_CYCLE_EN, MEM_WRITE_INVALIDATE_EN, PARITY_ERROR_EN and SERR_EN are "Disable (always)"; PAL_SNOOP_EN writable; AD_STEPPING default 1; FAST_B2B_EN default 0 |
| 0x06 | STATUS | CAP_LIST = 1, PCI_66_EN = 1, FAST_BACK_CAPABLE = 1, DEVSEL_TIMING = 1 (medium decoding) — the PCI_66_EN default is erratum 6 of §5 |
| 0x08 | REVISION_ID | MINOR_REV_ID bits 3:0, MAJOR_REV_ID bits 7:4, both default 0 ("0000 = initial RAGE 128 revision") |
| 0x09 | REGPROG_ID | register-level programming interface, default 0 |
| 0x0A/0x0B | SUB_CLASS, BASE_CODE | base class 3 (display controller), the standard VGA-compatible subclass observed on a live card [15]; with subclass 0 and REGPROG_ID 0, the class code reads 0x030000 |
| 0x0C/0x0D | CACHE_LINE, LATENCY | writable, default 0 — the Old World firmware's values are [pci.md](../pci.md) §2.5's |
| 0x0E | HEADER | type 0, single-function |
| 0x0F | BIST | present, default 0 |
| 0x10/0x14/0x18 | MEM_BASE / IO_BASE / REG_BASE | §2.1 |
| 0x2C | ADAPTER_ID | subsystem vendor ID (bits 15:0) and subsystem ID (bits 31:16), read-only, default 0 — loaded from the ROM strap bytes at reset (§2.12) |
| 0x30 | BIOS_ROM | §2.1 |
| 0x34 | CAPABILITIES_PTR | non-null: the PCI power-management capability and an AGP capability follow |
| 0x3C | INTERRUPT_LINE | default 0xFF; on the Power Macintosh the register "contains no useful information for drivers" ([pci.md](../pci.md) §2.5) |
| 0x3D | INTERRUPT_PIN | reads the ENINT# strap: "1 = INTA# requested (strapped to enable interrupt)" |
| 0x3E/0x3F | MIN_GRANT / MAX_LATENCY | MIN_GNT default 8, MAX_LAT default 0 |
| 0x4C | ADAPTER_ID_W | write-only alias that sets the subsystem IDs at 0x2C |
| 0x50/0x54 | CAPABILITIES_ID, AGP_STATUS | AGP capability v1.0; inert on PCI parts |
| 0x60 | PWR_MNGMT_CNTL_STATUS | PCI power-management interface, POWER_STATE bits 1:0 |

The subsystem-ID route deserves its own sentence because it is how a card is branded to an OS:
the chip's ROM state machine reads "a total of 5 bytes worth of 'ROM based straps' ... at byte
location 0x70 through 0x74 in the eprom/flashrom" right after PCI reset — "The first four bytes
contain subsys_venid[15:0] and subsys_id[15:0], and the fifth byte is reserved" — and "if neither
the system BIOS nor the add-in card video BIOS supply the subsystem-id and subsystem-vendor-id,
their values are defaulted to chip-id and vendor-id (0x1002)" [1] §4.12.1. The Macintosh ROM images
in the record do not carry ATI strap values there (§5).

The one device-identification wrinkle is MEM_BASE's mirror annotation: the 64 MB memory BAR's base
field "Mirror[s] bits 6:1 of APER_1_BASE" [2] §4.1 — i.e. the BAR decodes the same address bits that
CONFIG_APER_1_BASE programs, and CONFIG_APER_0_BASE is itself read-only, "Mirror bits 6:1 of
APER_1_BASE". Programming the pair consistently (BAR first, then CONFIG_APER_0/1_BASE) is the
driver-visible consequence; the internal decode priority is not documented (§6.12).

### 2.3 Configuration registers

The setup and configuration registers live in the low 0x0100 block, all of them 8/16/32-accessible
and most initialized once at boot [2] §2.1.1:

| Offset | Register | Purpose |
|---|---|---|
| 0x00E0 | CONFIG_CNTL | aperture endianness and VGA decode: APER_0_ENDIAN bits 1:0, APER_1_ENDIAN bits 3:2, APER_REG_ENDIAN bit 4 (§3.1), CFG_VGA_RAM_EN bit 8, CFG_VGA_IO_DIS bit 9, CFG_ATI_REV_ID (R) bits 19:16 [2] §4.1 |
| 0x00E4 | CONFIG_XSTRAP | readable image of the external straps: VGA_DISABLE bit 0, BUS_CLK_SEL bit 1, IDSEL bit 2, ENINTB bit 3, BUSTYPE bits 5:4, AGPSKEW bits 7:6, X1CLK_SKEW bits 9:8, FLASH_ROM bit 10, LCDPE bit 11 [2] §3.1 |
| 0x00E8 | CONFIG_BONDS | bond-out options: RSTRAP bits 1:0, PKGTYPE bit 2, CRIPPLEb bit 3, STRSTb bit 4, AVCOGN bit 6, LCDPE_OVERRIDE bit 8 [2] §3.1 |
| 0x00F0 | GEN_RESET_CNTL | general reset control [2] App. A |
| 0x00F8 | CONFIG_MEMSIZE | frame-buffer size in bytes, bits 25:0, "Bits 20:0 of this field are hardwired to ZERO" (2 MB granularity) [2] §4.1 |
| 0x0100 | CONFIG_APER_0_BASE | linear aperture 0 base, bits 31:26, read-only mirror [2] §4.1 |
| 0x0104 | CONFIG_APER_1_BASE | linear aperture 1 base, bits 31:25, bit 0 of the field hardwired to ONE [2] §4.1 |
| 0x0108 | CONFIG_APER_SIZE | size of *both* linear apertures [2] §4.1 |
| 0x010C | CONFIG_REG_1_BASE | register aperture 1 base, bits 31:13, bit 0 hardwired to ONE [2] §4.1 |
| 0x0110 | CONFIG_REG_APER_SIZE | size of both register apertures [2] §4.1 |
| 0x0114 | CONFIG_MEMSIZE_EMBEDDED | reserved for future on-chip frame buffer; "The RAGE 128 does not have any embedded memory" [2] §2.2.1 |
| 0x0170-0x017C | AGP_BASE, AGP_CNTL, AGP_APER_OFFSET, PCI_GART_PAGE | AGP aperture machinery; on a PCI part, PCI_GART_PAGE is the scatter-gather table pointer for bus-mastered "AGP-offset" transfers (§3.5) [2] §4.1 |
| 0x0010-0x001C | BIOS_0_SCRATCH to BIOS_3_SCRATCH | scratch registers for adapter ROM and application communication — and the chip-detection scratch test of §4.1 [2] App. A, [3] §3.2.3 |

### 2.4 CRTC, DAC and cursor registers

The accelerator CRTC — "not the same as the VGA CRTC registers" [2] §2.1.4 — generates the sync and
blank timing; "all horizontal parameters are in terms of characters (pixels * 8). All vertical
parameters are in terms of lines" [2] §6.1. The register set at a glance:

| Offset | Register | Function |
|---|---|---|
| 0x0050 | CRTC_GEN_CNTL | master control: CRTC_DBL_SCAN_EN bit 0, CRTC_INTERLACE_EN bit 1, CRTC_C_SYNC_EN bit 4, CRTC_PIX_WIDTH bits 10:8 (1=4bpp, 2=8bpp, 3=15bpp, 4=16bpp, 5=24bpp, 6=32bpp), CRTC_CUR_EN bit 16, CRTC_CUR_MODE bits 19:17, CRTC_EXT_DISP_EN bit 24 (0=VGA, 1=extended), CRTC_EN bit 25, CRTC_DISP_REQ_EN_B bit 26 [2] §6.1 |
| 0x0054 | CRTC_EXT_CNTL | VGA overscan enable, VGA_ATI_LINEAR bit 3, VGA_128KAP_PAGING bit 4, VGA_XCRT_CNT_EN bit 6, CRTC_HSYNC_DIS bit 8, CRTC_VSYNC_DIS bit 9, CRTC_DISPLAY_DIS bit 10, CRTC_SYNC_TRISTATE bit 11, flat-panel bits 21-23, VCRTC_IDX_MASTER bits 30:24 [2] §6.1 |
| 0x005C | CRTC_STATUS | CRTC_VBLANK_CUR bit 0, CRTC_VBLANK_SAVE bit 1 (write-1-to-clear via CRTC_VBLANK_SAVE_CLEAR), CRTC_VLINE_SYNC bit 2 [2] §6.1 |
| 0x0200 | CRTC_H_TOTAL_DISP | CRTC_H_TOTAL bits 8:0, CRTC_H_DISP bits 23:16, both in characters minus one [2] §6.1 |
| 0x0204 | CRTC_H_SYNC_STRT_WID | CRTC_H_SYNC_STRT_PIX bits 2:0, CRTC_H_SYNC_STRT_CHAR bits 11:3, CRTC_H_SYNC_WID bits 21:16, CRTC_H_SYNC_POL bit 23 (1 = active low) [2] §6.1 |
| 0x0208 | CRTC_V_TOTAL_DISP | CRTC_V_TOTAL bits 10:0, CRTC_V_DISP bits 26:16, in lines minus one [2] §6.1 |
| 0x020C | CRTC_V_SYNC_STRT_WID | CRTC_V_SYNC_STRT bits 10:0, CRTC_V_SYNC_WID, CRTC_V_SYNC_POL [2] §6.1 |
| 0x0210 | CRTC_VLINE_CRNT_VLINE | current line / vertical-line interrupt trigger [2] §6.1 |
| 0x0214 | CRTC_CRNT_FRAME | frame counter [2] App. A |
| 0x0218 | CRTC_GUI_TRIG_VLINE | the event line the WAIT_UNTIL register can stall on (§3.3) [2] App. A |
| 0x0224 | CRTC_OFFSET | start of displayable memory, in bytes; with CRTC_OFFSET_CNTL (0x0228) controlling tile/linear addressing [2] §6.1 |
| 0x022C | CRTC_PITCH | display pitch in pixels*8, bits 9:0 — "for 24-bpp format modes, the CRTC uses pixels*8 for the pitch, but the rendering engine uses bytes*8" [2] §6.1, [3] §3.4.3 |
| 0x0230-0x0238 | OVR_CLR, OVR_WID_LEFT_RIGHT, OVR_WID_TOP_BOTTOM | overscan border colour and widths [2] §6.2 |
| 0x0240-0x0248 | SNAPSHOT_VH_COUNTS, SNAPSHOT_F_COUNT, N_VIF_COUNT | CRTC count snapshots for video-field synchronisation [2] §6.1 |
| 0x0260-0x0270 | CUR_OFFSET, CUR_HORZ_VERT_POSN, CUR_HORZ_VERT_OFF, CUR_CLR0, CUR_CLR1 | hardware cursor (§3.9) [2] §6.3 |
| 0x0058 | DAC_CNTL | §3.6 and below |
| 0x00B0/0x00B4 | PALETTE_INDEX, PALETTE_DATA | accelerator-mode palette access [2] §6.6 |
| 0x02CC | DAC_CRC_SIG | CRC signature of the displayed frame [2] App. A |

CRTC_GEN_CNTL's CRTC_CUR_MODE field is both cursor mode and a monitor-GPIO steering field: mode 0
is "2bpp monochrome 64x64. 2 colour, transparent, inverse", and modes 4-7 reprogram "PANELID on
AGPIO2/3" with four DISP_ADDR-load timing choices — the panel-ID multiplexing the Macintosh driver
must not trip over when it uses the DDC pins (§3.6) [2] §6.1.

DAC_CNTL at 0x0058 [2] §6.6 holds the fields a Mac driver touches at every mode set: DAC_RANGE_CNTL
bits 1:0 ("Should be set to '10' by default"), DAC_BLANKING bit 2 (0 or 7.5 IRE pedestal),
**DAC_CMP_OUTPUT** bit 7 (R) — the monitor-presence comparator, "used for monitor detection by
sensing if the termination on the R,G&B lines is 75 ohms (no monitor) or 37.5 ohms (monitor
present)" — DAC_8BIT_EN bit 8 (8-bit versus VGA's 6-bit palette), DAC_4BPP_PIX_ORDER bit 9,
DAC_TVO_EN bit 10 and the TV-out fields, DAC_VGA_ADR_EN bit 13 (palette at VGA I/O addresses in
extended modes), and DAC_PDWN bit 15 (DAC power-down, "should save about 56 mA").

### 2.5 Clock and PLL registers

Four on-chip PLLs generate the pixel, memory and auxiliary clocks; all are reached indirectly
through CLOCK_CNTL_INDEX (0x0008: PLL_ADDR bits 4:0, PLL_WR_EN bit 7, PPLL_DIV_SEL bits 9:8
selecting among PPLL_DIV0-3) and CLOCK_CNTL_DATA (0x000C) [2] §3.4. The pixel PLL registers:

| PLL offset | Register | Key fields |
|---|---|---|
| 0x02 | PPLL_CNTL | PPLL_RESET bit 0, PPLL_SLEEP bit 1 (default 1 = powerdown), PPLL_DCYC bits 9:8, PPLL_RANGE bit 10, PPLL_VC_GAIN bits 12:11, charge-pump gains, PPLL_ATOMIC_UPDATE_EN bit 16, PPLL_VGA_ATOMIC_UPDATE_EN bit 17, PPLL_ATOMIC_UPDATE_SYNC bit 18 (0 = update ASAP, 1 = in VSYNC) [2] §3.4 |
| 0x03 | PPLL_REF_DIV | reference divider M, bits 9:0: "PPIIClk = N*PPLL_REF/M"; "divider must be >= 2"; "in general, M is set as large as possible"; PPLL_ATOMIC_UPDATE_R/W bit 15; PPLL_REF_DIV_SRC bits 17:16 (XTALIN, PLLMCLK/2, PLLXCLK/2) [2] §3.4 |
| 0x04-0x07 | PLL_DIV_0 to PLL_DIV_3 | four feedback/post divider pairs: PPLL_FBx_DIV bits 10:0 (N), atomic-update bit 15, PPLL_POSTx_DIV bits 18:16 (0 = /1, 1 = /2, 2 = /4, 3 = /8, 4 = /3, 6 = /6, 7 = /12) [2] §3.4 |
| 0x08 | VCLK_ECP_CNTL | VCLK source select, VCLK_INVERT bit 4, BYTE_CLK_POST_DIV bits 17:16 [2] §3.4 |
| 0x09 | HTOTAL_CNTL | HTOT_PPLL_SLIP bits 18:16 — "the number of VCO phase slips to do in the PLL at every HSYNC. Each VCO phase slip is equal to 0.2 of a PLLVCLK period" — genlock's fine adjustment [2] §3.4 |
| 0x0A-0x0E | X_MPLL_REF_FB_DIV, XPLL_CNTL, XDLL_CNTL, XCLK_CNTL, MPLL_CNTL | the memory (MPLL) and auxiliary (XPLL) synthesizers, same M/N discipline: "MPIIIClk = 2*N*Xtalin/M", legal VCO range "125 MHz to 250 MHz" [2] §3.4 |

The legal pixel-PLL VCO band is likewise "125 MHz to 250 MHz", which forces the post-divider
ladder: the SDK guide's PLL arithmetic (§4.3) picks a post divider from {1, 2, 3, 4, 6, 8, 12} so
that post-divider times dot clock lands inside that band [2] §3.4, [3] §3.4.4-§3.4.5.

### 2.6 Memory control registers

The memory controller "arbitrates requests from the VGA graphics controller, the drawing
coprocessor, the display controller, the video scaler, and the hardware cursor" [1] §4.11. Its
registers [2] §6.5:

| Offset | Register | Function |
|---|---|---|
| 0x0140 | MEM_CNTL | memory configuration, refresh and power-down control [2] §6.5 |
| 0x0144 | EXT_MEM_CNTL | extended memory control [2] App. A |
| 0x0148 | MEM_ADDR_CONFIG | the row/column/bank mapping — one of the fourteen ADDR_MAPPING encodings of [1] Table 4-4, e.g. "12 x 8 x 4" for the 64-Mbit 13x8 SDRAM of a 16 MB or 32 MB card [2] §6.5 |
| 0x014C | MEM_INTF_CNTL | memory interface timing (the values of [1] §5.2) [2] §6.5 |
| 0x0150 | MEM_STR_CNTL | SGRAM-specific features — block write [2] §6.5 |
| 0x0154 | MEM_INIT_LAT_TIMER | initialisation latency timer [2] §6.5 |
| 0x0158 | MEM_SDRAM_MODE_REG | the SDRAM/SGRAM mode register image [2] §6.5 |

The supported memory geometries [1] Tables 4-1 to 4-3: 8-Mbit and 16-Mbit SGRAM SDR (2 MB and 4 MB
per 64-bit granularity, 4 MB and 8 MB at 128-bit), 16-Mbit and 32-Mbit DDR SGRAM on the VR, and
16-Mbit to 64-Mbit SDRAM SDR — the 64-Mbit 13x8 part reaching "16MB [at] 64-bit Granularity [and]
32MB [at] 128-bit" [1] Table 4-3, which is the geometry of the 32 MB Nexus 128 and of 16 MB GL
cards at half population. Clock bounds by signalling class: SDR LVTTL 83-125 MHz at either bus
width; SDR SSTL_3 125-143 MHz (64-bit); DDR SSTL_2 100-125 MHz [1] Table 4-5.

### 2.7 The 2D engine register set

The 2D engine's registers sit in the 0x1400-0x17E0 GUI block; writes to the block pass through the
command FIFO and are consumed in write order, register reads are not FIFOed, and every register in
the block is dword-access-only [2] Table 2-2. The set, by subgroup:

**Destination and source descriptors** [2] §7.1-§7.2: DST_OFFSET (0x1404, byte-aligned, 128-bit
alignment enforced, bits 3:0 read zero), DST_PITCH (0x1408, pixels*8), DST_X (0x141C) and DST_Y
(0x1420) — both 14-bit two's-complement, "range -8192 to 8192", sign-extended, and interpretable as
S.12.2 fixed point when the engine's sub-pixel mode is on — DST_WIDTH (0x140C), DST_HEIGHT
(0x1410), and the composite write forms DST_Y_X (0x1438), DST_WIDTH_X (0x1588),
DST_HEIGHT_WIDTH (0x143C) and DST_PITCH_OFFSET (0x142C, offset bits 20:0 and pitch bits 30:21 in one
dword). SRC_OFFSET (0x15AC), SRC_PITCH (0x15B0), SRC_X (0x1414), SRC_Y (0x1418), SRC_X_Y (0x1590),
SRC_Y_X (0x1434), SRC_PITCH_OFFSET (0x1428) mirror the destination set, with SRC_PITCH sharing the
mono-mode rule that "the destination pitch must be a multiple of 64 pixels" [2] §7.1-§7.2.

**Line engine** [2] §7.1: the Bresenham set — DST_BRES_LNTH (0x1634), DST_BRES_LNTH_SUB (0x1638),
and for polylines the paired LEAD_BRES_ERR/INC/DEC (0x1600-0x1608) and
TRAIL_BRES_ERR/INC/DEC (0x160C-0x1614) with TRAIL_X (0x1618), TRAIL_X_SUB (0x1620) and
LEAD_BRETH_LNTH (0x161C) — plus DST_X_SUB/DST_Y_SUB (0x15A4/0x15A8) carrying the 0.5-pixel start
bias.

**Data path** [2] §7.5: DP_CNTL (0x16C0) — DST_X_DIR/DST_Y_DIR bits, SRC_X_DIR/SRC_Y_DIR,
SUB_PIX_ON, the mono-expansion and transparency bits, and DP_POLY_EDGE (bit 18, written by the
setup engine for anti-aliased polygon edges) — and the composite **DP_GUI_MASTER_CNTL** (0x146C),
which packs the source/destination pitch-offset defaults, clipping defaults, BRUSH and source
types, the ALU mix and "GMC_ROP3 ... Mapped to DP_ROP3 in DP_MIX" into one write [2] §7.5. The
colour-compare group CLR_CMP_CNTL/CLR_CMP_CLR_SRC/CLR_CMP_CLR_DST/CLR_CMP_MSK follows at
0x15C0-0x15CC [2] §7.7, the scissor group SC_LEFT/RIGHT/TOP/BOTTOM (0x1640-0x164C) and the
composite SC_TOP_LEFT/SC_BOTTOM_RIGHT (0x16EC/0x16F0) at §7.6, and the auxiliary scissor pair
AUX_SC_CNTL/AUX1-3 scissors (0x1660-0x1690) whose AUX2_SC_MODE/AUX3_SC_MODE bits choose additive
('OR') or subtractive ('AND NOT') combination with the main scissors [2] §7.6.

**Host and pattern data** [2] §7.3-§7.4: HOST_DATA0 through HOST_DATA7 at 0x17C0-0x17DC — eight
registers, not sixteen as on the mach64 — with HOST_DATA_LAST (0x17E0) terminating host-fed draws;
the pattern engine is the BRUSH group: BRUSH_Y_X (0x1474), BRUSH_SCALE (0x1470) and the 64-dword
BRUSH_DATA0-63 array at 0x1480-0x157C for the 8x8x32-bit pattern register file [2] App. A.

**Status and synchronization** [2] §7.8-§7.9: **GUI_STAT** (0x1740) reports GUI_FIFOCNT bits 11:0
("Number of free CMDFIFO entries", reset value 40), a bank of per-engine busy bits — PM4_BUSY
(16), MICRO_BUSY (17), FPU_BUSY (18), VC_BUSY (19), IDCT_BUSY (20), ENG_EV_BUSY (21), SETUP_BUSY
(22), EDGEWALK_BUSY (23), ADDRESSING_BUSY (24), ENG_3D_BUSY (25), ENG_2D_SM_BUSY (26), ENG_2D_BUSY
(27), GUI_WB_BUSY (28), CACHE_BUSY (29) — and **GUI_ACTIVE** (bit 31), "'OR' of the above bits"
[2] §7.9. **WAIT_UNTIL** (0x1720) is the in-band alternative to polling: writing it stalls the
command FIFO until the AND of every set trigger — CRTC page-flip (EVENT_CRTC_OFFSET, bit 0), the
vertical-line events (bits 1-3, against CRTC_GUI_TRIG_VLINE), bus-master channel idle (bits 4-9),
command-FIFO space (EVENT_CMDFIFO bit 10 with EVENT_CMDFIFO_ENTRIES bits 26:20) and the overlay
flip event (bit 11) [2] §7.8. Five FIFO'd scratch registers, GIU_SCRATCH_REG0-4 at 0x15E0-0x15F0,
give software a command-stream-orderable store [2] §7.9.

### 2.8 The surface registers

Between the apertures and the engine sits a translation layer for up to four drawing surfaces:
SURFACE_DELAY (0x0B00) with its power-of-two/non-power-of-two delay fields, and per surface the
lower/upper bound and info registers — SURFACE0_LOWER_BOUND (0x0B04), SURFACE0_UPPER_BOUND
(0x0B08), SURFACE0_INFO (0x0B0C), repeating every 0x10 bytes for surfaces 1-3 (0x0B14-0x0B3C) [2]
§4.1. Each SURFxn_INFO carries only a PITCHSEL field whose encoding selects the surface's tile
pitch: 0 = "Linear/No translation", then powers of two from 64 bytes to 4096, then the legacy
Macintosh-friendly widths — 8 = 640 bytes, 9 = 1280, 10 = 2560, 12 = 1600, 13 = 3200, 15 = 832,
16 = 1664, 17 = 3328, 18 = 1920, 19 = 3840 bytes [2] §4.1. SURF_TRANSLATION_DIS (SURFACE_DELAY
bit 8, default 1) turns the whole layer off; the default state is linear.

### 2.9 The CCE register set

The Concurrent Command Engine is programmed through the PM4 register block [7]:

| Offset | Register | Function |
|---|---|---|
| 0x0700 | PM4_BUFFER_OFFSET | ring-buffer start, "a 32MB AGP/PCI pointer ... (i.e. bit 25 is '1'). Must be 128 byte-aligned" [7] |
| 0x0704 | PM4_BUFFER_CNTL | ring size as power-of-two QWORD count (bits 5:0; "a 10 means ... 8 KB, while a 20 means 1M QWORD entries (i.e., 8 MB)"; "Minimum size is 256 DWORDs"), PM4_IN_FRAME_BUFFER bit 26, PM4_BUFFER_CNTL_NOUPDATE bit 27, and PM4_BUFFER_CNTL_FIFO_MODE bits 31:28: 0 = non-CCE, 1 = 192-DWORD CCE PIO, 2 = 192-DWORD CCE bus-master, 3 = 128 CCE PIO + 64 indirect BM, 4 = 128 CCE BM + 64 indirect BM, 5 = 64 CCE PIO + 128 indirect BM, 6 = 64 CCE BM + 128 indirect BM, 7/8 = the same with a 64-DWORD vertex-cache channel, 15 = all-PIO. "NOTE: This register is reset with SOFT_GUI_RESET" [7] |
| 0x0708 | PM4_BUFFER_WM_CNTL | the four ring-refill watermarks WMA/WMB/WMC ("in terms of groups of 4 DWORDs") and WB_WM bits 31:24 — when the read pointer is written back to host memory ("in terms of 64 DWORDs") [7] |
| 0x070C | PM4_BUFFER_DL_RPTR_ADDR | host address the engine's read pointer is written back to; "DL_RPTR must be written via PCI bus mastering" [7] |
| 0x0710 | PM4_BUFFER_DL_RPTR | the read index itself, bits 22:0 [7] |
| 0x0714 | PM4_BUFFER_DL_WPTR | the driver's write index, bits 22:0; PM4_BUFFER_DL_DONE bit 31 asks the parser for a wait-for-idle mode [7] |
| 0x0730-0x073C | PM4_VC_VLOFF (0x0730), PM4_VC_VLSIZE (0x0734), PM4_IW_INDOFF (0x0738), PM4_IW_INDSIZE (0x073C) | vertex-list and indirect-buffer descriptors ("Vertex list size in terms of vertices (8 DWORDs)"); with PM4_STAT at 0x07B8 whose PM4_FIFOCNT bits 11:0 and PM4_BUSY bit 16 mirror GUI_STAT [7] |
| 0x07D4 | PM4_MICROCODE_ADDR | parser microcode RAM address, bits 7:0 [7] |
| 0x07DC/0x07E0 | PM4_MICROCODE_DATAH / PM4_MICROCODE_DATAL | microcode write port — bits 36:32 and 31:0 of each 37-bit word; "the high DWORD must be written first for address autoincrement to work correctly" [3] §5.2.2 |
| 0x07FC | PM4_MICRO_CNTL | PM4_MICRO_STAT (R) bits 15:0, PM4_MICRO_MUX bits 20:16, and PM4_MICRO_MODE bit 30: "0 = Single step mode, 1 = Free running mode" — the SDK guide's start-up leaves it free-running [7] |

The packet formats the parser consumes — the draw-state, 3D and vertex-cache packets addressed
through the register pages above 0x1800 — are [7]'s register supplement; the 3D engine's own
register block (SETUP_CNTL, SCALE_3D_CNTL, the interpolator and texture registers) is documented
there and in [2] chapter 8's page range, and this page does not restate it: a 3D driver's needs are
served by [7] directly.

### 2.10 Bus-master registers

The bidirectional bus-master engine — used by the CCE, the video capture channels and the
overlay/capture queues — is the BM block at 0x0A00 [6]: BM_FRAME_BUF_OFFSET (0x0A00) and
BM_SYSTEM_MEM_ADDR (0x0A04) define the two ends of a transfer; **BM_COMMAND** (0x0A08) starts it,
with BYTE_COUNT bits 20:0, INTERRUPT_DIS bit 27, TRANSFER_DEST bit 28 ("0 = Transfer to/from frame
buffer memory, 1 = Transfer to/from registers"), FORCE_TO_PCI bit 29, FRAME_OFFSET_HOLD bit 30 and
END_OF_LIST_STATUS bit 31; BM_STATUS (0x0A0C) reads the engine; BM_QUEUE_STATUS and
BM_QUEUE_FREE_STATUS (0x0A10/0x0A14) report the VIP, video-capture and GUI queues' fill and
activity — BM_GUI_ACTIVE at bit 30 of both — and BM_CHUNK_0_VAL / BM_CHUNK_1_VAL (0x0A18/0x0A1C)
pace the queues, including BM_GLOBAL_FORCE_TO_PCI bit 23 for AGP designs (inert on a PCI card) [6].

### 2.11 Interrupt registers

Two registers own the whole interrupt surface [2] §8.1: **GEN_INT_CNTL** (0x0040) — enables for
CRTC_VBLANK (bit 0), CRTC_VLINE (bit 1), CRTC_VSYNC (bit 2), SNAPSHOT (bit 3), BUSMASTER_EOL
(bit 16), I2C (bit 17), MPP_GP (bit 18), GUI_IDLE (bit 19) and VIPH (bit 24) — and
**GEN_INT_STATUS** (0x0044), where each source is one bit read as the latched event and written as
its acknowledge: "CRTC_VBLANK_INT (R) ... Vertical blank started since last cleared" and
"CRTC_VBLANK_INT_AK (W) ... Write '1' clears CRTC_VBLANK_INT status", likewise VLINE, VSYNC,
SNAPSHOT, BUSMASTER_EOL, I2C, MPP_GP, GUI_IDLE and VIPH, plus the capture-port active bits 8-9
[2] §8.1. The enables are qualified by the strap: "No effect if strapped to interrupt disable"
[2] §8.1 — a card with ENINT# pulled high asserts nothing (§2.12).

### 2.12 Reset state and straps

Each register's reset value is the register reference's "Default" column, cited throughout §2; the
deeper reset contract is the specification's boot sequence. The chip latches pin straps on the
PCI reset edge, then — on an add-in card, where the ROMCS# strap is pulled to "add-in card
implementation, bios cycles will occur" — "ROM state machine begins to read 'ROM based straps'",
taking the five bytes at ROM offsets 0x70-0x74 before servicing the first PCI transfer [1]
§4.12.1. The pin straps are shared with signal pins, which is why they are readable back at
CONFIG_XSTRAP [1] Table 4-7:

| Strap | Pin | Default | Meaning |
|---|---|---|---|
| vga_disable | SAD7 | 0 | 0 = VGA controller capability enabled; 1 = extended-mode only |
| idsel# | SAD6 | 0 (AD16) | AGP only: IDSEL pin choice |
| lcd | LCDCDE | 0 | LCD panel interface off |
| id_disable | LCDCLK | 0 | 1 = chip does not respond to configuration cycles (dual-graphics jumper) |
| enint# | VSYNC | 0 | 0 = interrupt enabled |
| bus_clk_sel | HSYNC | 0 | with bustype[1:0], selects the bus mode |
| bustype[1:0] | SAD[5:4] | 0,0 | bus-type table: mode 1 (bus_clk_sel=1, bustype=00) is "Ref. clk, PCI 5v signaling environment" — the Macintosh configuration; mode 4 (default 00,00) is "PLL clk, AGP 1x, 2x" |
| agpskew[1:0], x1clk_skew[1:0] | SAD[3:0] | 0 | AGP/auxiliary clock phase trims |
| add_in_card | ROMCS# | 1 (pull-up) | 1 = add-in card, BIOS/strap cycles occur |

A board-level note rides with them: because the SAD-line straps are shared with the multimedia
port, "the use of on board tristate-able buffers (LS244) is recommended", with strap resistors
4.7-10 kΩ [1] §4.13.2. What the Macintosh cards actually strap — the VGA-disable state in
particular, since Mac OS never drives VGA — is not in the evidence set (§6.8).

## 3. Behaviour

### 3.1 Endianness: the bi-endian apertures

The platform seam is fixed and belongs to the bus, not the card: "byte order on the processor bus is
big-endian and byte order on the PCI bus is little-endian", with the host bridge performing the
transformation ([pci.md](../pci.md) §3.4; [tnt.md](../../../machines/tnt/tnt.md) §2.5). The Rage 128
is a little-endian device on the little-endian side of that seam, so its registers and frame buffer
must be driven with the transformation in mind — the mach64 page works the same rule from the
other generation ([mach64.md](mach64.md) §3.1). What the Rage 128 adds on top is *configurable*
endianness per aperture, in CONFIG_CNTL (0x00E0) [2] §4.1:

| Field | Bits | Encoding |
|---|---|---|
| APER_0_ENDIAN | 1:0 | 0 = little-endian (no swapping); 1 = big-endian, 16-bpp swapping; 2 = big-endian, 32-bpp swapping |
| APER_1_ENDIAN | 3:2 | same encoding, for linear aperture 1 |
| APER_REG_ENDIAN | 4 | 0 = little-endian (no swapping); 1 = big-endian, 32-bpp swapping — for the register apertures |

The chip-level intent is stated twice in the register reference's memory-mapping chapter: the second
register aperture exists "For the PowerMac environment, this allows one aperture to be marked as
cacheable", and the second linear aperture "allows each to be independently marked as big-endian or
little-endian" [2] §2.2.1. The controller specification's one-line bus rule for the platform is:
"When incorporating the RAGE 128 into a non-Intel platform (such as the Apple Power Macintosh),
make sure the platform conforms to the PCI specification" [2] §2.2.3 — i.e. the chip's only
Macintosh-specific demand is a conforming PCI implementation; the endian accommodation is all
in-chip. The 16-bpp and 32-bpp swap widths exist because the aperture swapping acts on pixel
containers, not on dwords in general: which combination the shipped Mac FCode and drivers actually
select for each aperture is *observed* nowhere yet and is §6.5's.

### 3.2 Aperture semantics and frame-buffer addressing

The frame-buffer image fills each linear aperture from offset 0 to CONFIG_MEMSIZE-1, and the engine's
own addressing is a 32-bit "virtual" space: DST_OFFSET is "a virtual address. The lower 32MB maps to
frame buffer, the upper 32MB to AGP_BASE + DST_OFFSET(24:0)" [2] §7.1 — on a PCI card the upper
range is meaningless without the PCI GART (§3.5). Aperture sizes are read from CONFIG_APER_SIZE
and CONFIG_REG_APER_SIZE rather than assumed ("The size may vary in future generations of the
accelerator" [2] §2.2.1); the retail cards publish their own figures — "Supports 16MB relocatable
memory aperture" on the Orion [14] p. 32, 32 MB on the Nexus 128 [13] p. 32 — and the register
reference caps the design at "up to 32MB of frame buffer memory" [2] §2.2.1. The A13 erratum that
the chip "requires a 128 MB of memory aperture which needs to be allocated by the system", reduced
"to 64 MB in the Rage128 A21" [10], is the window-size story of §5; the memory BAR's 64 MB decode
granularity (§2.1) matches the A21 figure.

### 3.3 The command FIFO and engine status

Every write to the GUI register block passes through the command FIFO — the "128-entry command FIFO"
of the specification's feature list [1] §2.1.1 — and is consumed in write order; reads bypass it.
Three contract points, each stated by the SDK guide:

1. **Wait before writing.** "Prior to any writes to any CCE register, it is essential to check the
   state of the GUI engine to ensure that the contents of the command FIFO have been processed and
   the engine is in a state of idleness" [3] §5.2.1 — the guide's `R128_WaitForIdle` precedes every
   sequence it programs.
2. **Idle is not free FIFO.** GUI_ACTIVE (GUI_STAT bit 31) is the OR of the per-engine busy bits
   [2] §7.9; a free command FIFO only says the writes have been accepted, not executed.
3. **Stall in band when possible.** WAIT_UNTIL pushes the same discipline into the command stream:
   the page-flip, vertical-line, bus-master-idle and FIFO-space triggers of §2.7, "Stall CMDFIFO
   based on 'AND' of all set triggers" [2] §7.8 — this is how a double-buffered Mac animation waits
   for the display to pick up a new CRTC_OFFSET without polling.

### 3.4 The 2D engine

The draw engine's data path is the mach64 lineage's, rebased on new registers: per destination
pixel a source select (host data, blit source, pattern registers, constant), a destination source,
a mix function now spanning "full ROP support (including ROP3)" through DP_GUI_MASTER_CNTL's
GMC_ROP3 field [1] §2.1.2, [2] §7.5, and the colour-compare gate of CLR_CMP_CNTL [2] §7.7.
Destination trajectories are the rectangle (DST_WIDTH, DST_HEIGHT, the composite DST_HEIGHT_WIDTH
write) and the Bresenham line (DST_BRES_LNTH and the polylines' paired lead/trail error
accumulators) [2] §7.1; source trajectories are linear, patterned and rotated-pattern as on the
mach64, with SRC_PITCH_OFFSET holding the composite form [2] §7.2. Monochrome expansion and the
scissor set — the plain scissors plus three additive/subtractive auxiliary pairs — round out the
QuickDraw-relevant surface [2] §7.6. Host data flows through the eight HOST_DATA registers with
HOST_DATA_LAST closing the sequence [2] §7.3.

### 3.5 The CCE and bus mastering

The CCE is the generation's defining interface change: a microcoded parser ("the microengine")
between the command FIFO and the engines, fed either by programmed I/O into the FIFO or by PCI bus
mastering from a ring buffer. The start-up discipline [3] §5.2:

1. Wait for engine idle.
2. Load the microcode — "The microcode for the microengine is 256 QWORDs long", written 256 times
   through PM4_MICROCODE_DATAH then PM4_MICROCODE_DATAL, after setting PM4_MICROCODE_ADDR to 0; the
   write address auto-increments per pair [3] §5.2.2.
3. Program the ring: PM4_BUFFER_OFFSET (128-byte aligned), PM4_BUFFER_DL_WPTR, the four watermarks
   of PM4_BUFFER_WM_CNTL, PM4_BUFFER_CNTL for the chosen FIFO mode, and
   PM4_BUFFER_DL_RPTR_ADDR — the host address the engine writes its read pointer back to, "via PCI
   bus mastering" [3] §5.2.3, [7].
4. Set the microengine free-running (PM4_MICRO_CNTL) [3] §5.2.3.

The FIFO modes of PM4_BUFFER_CNTL_FIFO_MODE (§2.9) are the PIO-versus-bus-master dial: modes 1, 3,
5, 7 and 15 are PIO (the host pushes every dword), modes 2, 4, 6 and 8 hand the ring to the bus
master [7]. In bus-master mode the driver publishes packets into the ring and moves only the write
pointer; the engine fetches on its own schedule, gated by the watermarks — "if we denote the actual
numbers of DWORDs in the ring buffer, command FIFO buffer, and CCE FIFO buffer respectively by
l, m and n, the condition for initiating a data transfer is: l > L, or m <= M, or n <= N" [3]
§5.2.3 — and writes the read pointer back so the driver can reclaim space. The caution is absolute:
"All packets must be checked for proper formatting prior to submission to the server. Incorrectly
formatted packets will cause the RAGE 128 to hang" [3] §5.2.4.

On an AGP system the ring lives in AGP memory; on a PCI card the SDK guide's fallback is the **PCI
GART**: "No AGP available, use PCI GART mapping instead", with a page table pointed to by
PCI_GART_PAGE — "a 32bit physical memory address to a 32KB table of page entries ... DWORD
PhysPageNo[8192]" — giving "up to a 32MB continuous address space into PCI system memory via a
scatter-gather mechanism" for the ring and any "AGP-offset" system-memory surfaces [3] §5.2.3,
[2] §4.1. Bit 0 of PCI_GART_PAGE is PCI_GART_DIS, default 1 (disabled). The generic bus-master
engine of §2.10 serves the same transfers outside the CCE — display-list fetch, video-capture and
VIP queues — under BM_COMMAND's descriptor model [6].

Whether Mac OS ever uses CCE bus mastering on these cards is open (§6.6): the mode is available and
the PCI GART is the documented path, but no Mac driver has been disassembled to show which
PM4_BUFFER_CNTL_FIFO_MODE it writes.

### 3.6 Monitor sensing: AppleSense and DDC on one pin set

The four MONID pins carry both monitor-identification disciplines, and the register that drives
them is GPIO_MONID (0x0068): "This register permits software control of the 4 MONID pins to perform
AppleSense for Apple monitors, or the MONID2/1 pins (MONID2 pin = CLK, MONID1 pin = DATA) to
perform Monitor DDC (Display Data Channel) for monitor identification" [2] §3.1. The field layout
per pad: MONID_A bits 3:0 (outgoing level), MONID_Y bits 11:8 (read-back state), MONID_EN bits
19:16 (direction: 0 = input, 1 = output), MONID_MASK bits 27:24 (GPIO enable) [2] §3.1. The DDC
secondary pair — AGPIO2/AGPIO3 — belongs to GPIO_MONIDB (0x006C) and is only usable when
"CRTC_CUR_MODE[2] ... must be enabled", the same CRTC_GEN_CNTL field that selects the panel-ID
multiplexing of §2.4 [2] §3.1.

The chip also offers the load-sensing detector the mach64 line used: DAC_CNTL's DAC_CMP_OUTPUT,
comparing the RGB line termination against 75 Ω / 37.5 Ω to decide whether a monitor is attached at
all, with the documented recipe — "To test if Green is terminated, set Red and Blue to 0 and set
Green to 5A (post palette). If the Green line is terminated, then DAC_CMP_OUTPUT will read back '1'
when the raster is on the above color" — and the caveat that the raster must be in the active area
and the read repeated [2] §6.6.

The retail cards' documented behaviour is the Macintosh discipline: the Nexus 128
"auto-configures itself according to the monitor sense code detected and enables all resolutions
supported by the monitor", falling back to DDC — "On VGA monitors, the NEXUS 128 card uses the
Display Data Channel (DDC) protocol, which is similar to Apple's Monitor Sensing specification" —
and to manual resolution choice on non-DDC VGA monitors; a fixed-code adapter "must be designed to
supply the proper code for your monitor". The card "does not output Sync-on-Green (SOG) signals" [13]
p. 9. Which pin pairs the Mac driver exercises, and in which order, is not observed (§6.7).

### 3.7 Clocking

The reference clock arrives on XTALIN ("PLL reference clock or MXCLK source"), with the A22 advisory
requiring "an oscillator (or an equivalent clock source)" rather than a crystal, because "the input
structure for the reference clock is powered by the core supply which can contain some noise during
high draw engine activity" [12] §"Oscillator Preferred over Crystal". From it the PPLL derives the
pixel clock as "PPIIClk = N*PPLL_REF/M" with N from one of four PPLL_FBx_DIV feedback dividers and a
post divider from the {1, 2, 3, 4, 6, 8, 12} ladder, selected by PPLL_DIV_SEL or by VGA mode's
VGA_CKSEL; the four divider sets let a driver pre-stage four clocks and switch among them with a
single atomic update [2] §3.4. The atomic-update machinery — PPLL_ATOMIC_UPDATE_EN, the _R/_W pair,
and PPLL_ATOMIC_UPDATE_SYNC choosing update-ASAP versus update-in-VSYNC — exists for genlocking
("Should be used when setting new PPLL_REF_DIV or PPLL_FBx_DIV, or when changing PPLL_DIV_SEL ...
for GEN-locking" [2] §3.4), with HTOTAL_CNTL's phase-slip register as the finest trim: 0.2 PLLVCLK
periods slipped at every HSYNC [2] §3.4.

Memory and engine clocks come from the MPLL (X_MPLL_REF_FB_DIV: "MPIIIClk = 2*N*Xtalin/M") and the
XPLL [2] §3.4, bounded by the signalling class of §2.6 [1] Table 4-5. The A22 production advisory
caps them: "the maximum memory/engine clock frequencies (as set by the video BIOS) are 125/80 for
the VR and 100/90 for the GL" [12] §"Maximum Memory/Engine Clock Frequencies" — the figure a Mac
card's FCode must stay inside, since the PC video BIOS it refers to does not run.

### 3.8 Interrupts

The chip's entire interrupt output is one INTA# pin qualified by the ENINT# strap (§2.12), read
back at INTERRUPT_PIN. The nine GEN_INT sources — vertical blank, vertical line, vertical sync,
snapshot, bus-master end-of-list, I2C, MPP, GUI-idle and VIP host — are enabled in GEN_INT_CNTL and
acknowledged write-1-to-clear in GEN_INT_STATUS (§2.11). The Macintosh side of the wire is the
platform's: the slot's four pins are OR-combined into one slot line, so "there can be only one
interrupt source on your device" ([pci.md](../pci.md) §3.5) — which is precisely why the chip offers
the GUI_IDLE interrupt as the in-band alternative to polling GUI_STAT after a large submit, and the
WAIT_UNTIL register as its command-stream twin. A Mac driver installs its handler on the slot's
interrupt source tree ([pci.md](../pci.md) §4.5); the card's own dispatch among its nine sources is
entirely by GEN_INT_STATUS reads.

### 3.9 The hardware cursor

The cursor is a 64x64x2 sprite: mode 0 of CRTC_CUR_MODE, "2bpp monochrome 64x64. 2 colour,
transparent, inverse" [2] §6.1. CUR_OFFSET (0x0260) holds the pattern address, 128-bit aligned
("Bits 3:0 of this field are hardwired to ZERO"); CUR_HORZ_VERT_POSN (0x0264) positions it; the
offset registers handle the edges — "To move the cursor off the top of the display, set
CUR_VERT_POSN to 0, add 16*(number of lines to move off the top) to CUR_OFFSET, and increase
CUR_VERT_OFF by the same number of lines", and off the left edge the same manoeuvre in X — with
CUR_HORZ_VERT_OFF's fields giving "Height of cursor is (64-CUR_VERT_OFF)" and pixel-0 origin
control [2] §6.3. CUR_CLR0 and CUR_CLR1 (0x026C/0x0270) are the two 24-bit cursor colours, fed
straight to the DAC. The tear-free contract is CUR_LOCK (bit 31 of all three position registers):
"Locks the CUR_OFFSET, CUR_HORZ_VERT_POSN and CUR_HORZ_VERT_OFF registers to allow tear free atomic
updating of the cursor shape and/or position" [2] §6.3. In double-scan modes the cursor stays in
physical lines, "limited to 64 physical lines in height, which means only 32 logical lines" [2]
§6.1.

## 4. Programming model

### 4.1 PCI identity and the expansion ROM

The PC-oriented detection ladder of the Software Development Guide is a useful statement of the
identity surface even though Mac OS reaches the card differently: find vendor 0x1002, read the
device ID against the Rage 128 table, or "scan the BIOS segment" for the ROM signature ('AA55'),
the ATI product signature ('761295520') and the string 'R128' within the first 512 bytes, or run the
scratch-register test — write 0x55555555 then 0xAAAAAAAA to BIOS_0_SCRATCH through the I/O port and
read both back [3] §3.2. On the Power Macintosh, Open Firmware performs the equivalent discovery by
configuration cycles ([pci.md](../pci.md) §4.1), and the card's expansion ROM carries the rest.

The observed ROM structure of the two retail GL cards [15]:

| Field | Nexus 128 (113-57502-103) | Xclaim VR 128 (113-57406-108) |
|---|---|---|
| Image signature / PCIR pointer | 0x55AA at 0, PCIR at 0x20 | same |
| PCI data structure: vendor / device | 0x1002 / 0x5245 | same |
| Code type / indicator | 0x01 (Open Firmware FCode) / 0x80 (only image) | same |
| Image length | 0x12000 bytes | 0x1CA00 bytes |
| FCode image | format 8, `start1` at offset 0x40, checksum verified, 0x11EAA bytes | same form, 0x1C861 bytes |
| Embedded Mac OS driver | PEF (`Joy!peff`) named `.Display_Rage128`... with name `ATY,Rage128n` | `.Display_Rage128`, name `ATY,Rage128v` |

The flash part behind the ROM is observed on the OEM Pro card only: an Atmel AT49F001T (128 KB),
with a 10 ms write delay, and `regPLL_13` reading 0x00004491 after the Mac driver runs — one of the
few live register values on record [15]. The same dump shows the card answering in slot "SLOT-A"
with revision ID 0x00, and its FCode publishing `ATY,Rom# = "113-63001-110"`,
`ATY,Card# = "109-63000-00"`, Fcode "1.70", driver "1.0f01" [15].

### 4.2 The FCode bring-up of the retail cards

The retail FCode images are detokenised but not yet annotated [15]; the observed property set of
the Nexus 128's FCode is:

- `name` = "ATY,Rage128n" (device-name), `model` = "ATY,Rage128"
- `ATY,Rom#` = "113-57502-103", `ATY,Card#` = "109-57500-00", `ATY,Fcode` = "1.68"
- `ATY,memsize` and `ATY,MEM` (the memory inventory), `ATY,Flags`, `ATY,Status`
- a `reg` property built from `my-address my-space` plus a config-space BAR address
- `driver,AAPL,MacOS,PowerPC`: the embedded PEF ndrv, importing `DriverServicesLib`,
  `InterfaceLib`, `NameRegistryLib`, `PCILib`, `VideoServicesLib` and the Name Registry, interrupt,
  cache-mode (`SetProcessorCacheMode`) and endian-swap (`EndianSwap32Bit`, `EndianSwap16Bit`)
  interfaces — the imported-function list is the clearest window yet into what the Mac driver does:
  it manages its own interrupt set and flips the aperture cache mode, exactly the mechanisms
  [2] §2.2.1 says the dual apertures exist for [15].

The card's published properties feed the Open Firmware startup process and Mac OS driver matching
([pci.md](../pci.md) §4.2, §4.4); the same ROM that publishes them also carries the five
strap bytes the hardware reads at reset (§2.2), which on these images hold FCode program data
instead of subsystem IDs — the observed bytes at ROM offsets 0x70-0x74 are not an ATI
vendor/product pattern, so the strap load takes FCode data as the subsystem IDs unless the
production flash differs from the archived image [15] (*observed* in the images; the resulting
configuration-space values on a real retail card are unverified, §6.2).

### 4.3 Mode setting

A driver that cannot run the PC BIOS — which on a Macintosh is every driver — sets modes manually
[3] §3.4.3:

1. **Blank the display** through CRTC_EXT_CNTL's CRTC_HSYNC_DIS, CRTC_VSYNC_DIS and
   CRTC_DISPLAY_DIS, and enable it again when done.
2. **Clear the interferers**: overscan colour and widths, the overlay scaler, the MPP TV-out and
   general-purpose configs, the subpicture decoder, VIP transfers, the I2C bus, GEN_INT_CNTL, and
   both capture triggers [3] §3.4.3.
3. **Program the CRTC**: CRTC_GEN_CNTL for extended mode, CRTC enable, cursor off, pixel width and
   composite sync off; CRTC_EXT_CNTL by read-modify-write, then VGA_ATI_LINEAR and
   VGA_XCRT_CNT_EN; DAC_CNTL by read-modify-write preserving bits 2:0, DAC_8BIT_EN set, TVO and
   VGA palette addressing off, DAC_MASK 0xFF; the horizontal pair — CRTC_H_DISP =
   visible pixels/8 - 1 (bits 0:8), CRTC_H_TOTAL = total characters - 1 (bits 16:23), sync start,
   width and polarity in CRTC_H_SYNC_STRT_WID; the vertical pair likewise in CRTC_V_TOTAL_DISP and
   CRTC_V_SYNC_STRT_WID; then CRTC_OFFSET (0 for the plain case), CRTC_OFFSET_CNTL cleared, and
   CRTC_PITCH in pixels*8 [3] §3.4.3.
4. **Program the pixel PLL**: read the reference frequency, reference divider and the PLL
   output-frequency bounds from the BIOS header's PLL information block (header at BIOS+0x48,
   pointer at +0x30, reference frequency word at +0x0E, reference divider at +0x10, minimum and
   maximum PLL output at +0x12 and +0x16) [3] §3.4.4; then "the Feedback Divider must be from 128
   to 255 inclusive, and the Post Divider can be one of 1, 2, 3, 4, 6, 8, or 12", picking the post
   divider so that post-divider times dot clock lands in the legal VCO band, then
   `PLL output = (Reference Frequency * Feedback Divider) / (Reference Divider * Post Divider)`
   [3] §3.4.5.

The Macintosh cards' published mode set is the union of the fixed Apple sense-code resolutions —
512x384 at 60 Hz, 640x480 at 67, 832x624 at 75, 1024x768 at 75, 1152x870 at 75 — and a long VESA
ladder from 640x480@60 to 1920x1440@60 on the Nexus 128 [13] pp. 29-30 (the Orion's table ends at
1600x1200 [14] pp. 29-30), all at up to 32 bpp: "All video resolutions, regardless of memory
configuration, have a maximum color depth of 32 bits per pixel" [13] p. 29. Which M/N/post triples
the Mac FCode computes per mode is in the undecoded part of the ROM (§6.4).

### 4.4 Engine discipline at run time

Beyond mode setting, the SDK guide's run-time rules are the ones an accelerated QuickDraw driver
lives by: maintain the queue — wait for GUI_STAT idle before submitting more than the FIFO holds
[3] §4.2; use the composite registers (DST_PITCH_OFFSET, DP_GUI_MASTER_CNTL) to set state in
single FIFO'd writes [3] §4.3; feed host-data draws through HOST_DATA0-7 and close with
HOST_DATA_LAST [2] §7.3; and for CCE-fed work, the ring-buffer server of [3] §5.3 — publish,
advance the write pointer, let the engine's read-pointer writeback reclaim space. The guide's
drawing chapters (rectangles, polylines, polyscanlines, the four BitBlt variants with transparency
and scaling, small and large text) are worked examples over exactly the registers of §2.7 [3]
chapter 6.

### 4.5 The Mac OS driver stack

The software side is named by the vendor's own materials: the Nexus 128 guide ships the **ATI
Displays** control panel and "ATI Multimedia Components ... [that] enable all of the advanced
features of your card, including 3D and 2D acceleration", requires QuickDraw 3D 1.5.4 and
QuickTime 3.0, and supports "QuickDraw 3D RAVE", "OpenGL support using Conix OpenGL software",
32-bit rendering with 16- or 32-bit Z and an 8-bit stencil, single-pass bilinear/trilinear
filtering, mip-mapping, alpha blending and fog — with 3D disabled in 256 colours [13] pp. 1-2,
11-14. The ATI installer warns that a hang after installation "can occur when the Mac OS software
was installed, or other ATI accelerator graphic accelerator cards were installed", with the
shift-key extension bypass as the remedy [13] p. 4. The driver archive record adds the OS-side
pieces the guide does not name: Apple's ATI RAGE 128 Update 1.0, the `ATI Rage 128 3D Accelerator`
RAVE extension, `ATI Graphics Accelerator`, `ATI Video Accelerator`, Apple's OpenGL 1.1.2/1.2.1
renderer set, and ATI's September 2001 ROM Update whose "Rom Xtender" extension replaces the card
ROM's driver at load time on Mac OS 9 [15]. None of these binaries is disassembled; their register
behaviour is §6's largest single gap.

### 4.6 Silicon revisions and the errata ladder

The revision is readable in two places: REVISION_ID's MAJOR_REV_ID/MINOR_REV_ID nibbles [2] §4.1
and "the revision level ... extracted from the last three digits of the ATI part number" [10] —
215R4BASA13 is an A13, 215R4GASA22 an A22 GL. The ladder the advisories build:

| Revision | Status | What the advisory says |
|---|---|---|
| A12 | "preliminary engineering samples ... significant errata" | screen corruption (partially fixed in A13 by metal changes, fully in A21); 2D/3D hangs with partial driver workarounds; "the video BIOS for A12 implements a reduced memory and engine clock frequency of 75 MHz" [9] |
| A13 | "engineering samples, and are provided for evaluation only. The A13 is not a production part" | pixel-cache write failures (process-related); pixel-cache cross-allocation deadlocks with software purging/flushing workarounds; low Vil on AGP and GPIO pads, worked around at 2.8 V core; DP_WRITE_MASK affecting the Z plane (an OpenGL CDRS conformance failure); the 128 MB aperture requirement; the 66 MHz capability bit at 33 MHz; the NAND-tree; an arbitration inefficiency [10] |
| A21 | first production silicon | every A12/A13 erratum's fix column points here: "Deadlock conditions have been isolated, and the pixel cache control logic has been fixed in the Rage128 A21"; "The Rage 128 memory aperture requirement visible to the system has been reduced to 64 MB" [10] |
| A22 | "the second production revision ... ramping for high volume production" | 2.8 V core (VDDC, AVDD, PVDD) fixing the I/O threshold; heat-sink recommendation; RSET 374 Ω; oscillator required; AGP_PUMP_GAIN default corrected by the BIOS; clock caps 125/80 (VR) and 100/90 (GL); LCD 5-pixel shift at 1024x768; texture mirroring broken between certain coordinates; the OGL line-AA optimization [11] [12] |
| A23 | announced | the A22 items "identified and resolved in the A23 revision": the LCD shift, AGP_PUMP_GAIN default, line-AA, the pixel-cache yield redesign [11] |

A driver keys its workarounds off this ladder, which is why the revision ID the card reports and the
advisories it expects must agree (§6.3): the Mac cards ship a year after A13, so they are A21 or A22
silicon — but which, and what REVISION_ID value the Mac FCode leaves in place, is unobserved.

## 5. Quirks & errata

- **Every aperture exists twice, on purpose.** Two register apertures ("one aperture to be marked as
  cacheable"), two linear apertures ("each ... independently marked as big-endian or
  little-endian"), both stated "For the PowerMac environment" [2] §2.2.1. The second register
  aperture's base is readable *only* as CONFIG_REG_1_BASE inside aperture 0 — "the only method ...
  forward compatible with future generations" [2] §2.2.1.
- **The A13 engineering silicon is not a production part.** "The A13 is not a production part" [10]
  — any card that reports A13 is an evaluation sample, and its pixel-cache deadlocks need the
  documented purging/flushing software workarounds "that have an impact on performance" [10].
- **A21/A22 need 2.8 V, not the specified 2.5 V.** "The A21 and A22 revisions require 2.8v core
  supplies" [11]; the specification's 2.5 V figure [1] §1.1 no longer applies — a board-level fact,
  but the reason a marginal A22 card shows "intermittent screen corruption or system hang
  failures" [11].
- **The aperture demand shrank mid-life.** A13 silicon "requires a 128 MB of memory aperture";
  A21 reduced it to 64 MB [10] — and the memory BAR's decode is 64 MB-granular bits 31:26 [2] §4.1,
  matching the A21 figure, not the A13 one.
- **The 66 MHz capability bit lies at 33 MHz.** "In 33MHz PCI implementations, the PCI 66MHz
  capability bit is set to 1" — erratum 6, "should not cause any problems", fixed in A21 [10]. A
  PCI-compliance scan of a Rage 128 will report a 66 MHz-capable device in a 33 MHz slot.
- **The subsystem IDs come from the ROM, or from garbage.** Five strap bytes at ROM offsets
  0x70-0x74 feed ADAPTER_ID at reset; absent them, the IDs "default ... to chip-id and vendor-id
  (0x1002)" [1] §4.12.1. The Mac ROM images carry FCode bytes there instead (§4.2) — so a retail
  card's reported subsystem IDs depend on what its flash actually contains at those offsets.
- **The subsystem-ID register has a write alias.** ADAPTER_ID_W at configuration offset 0x4C
  writes the read-only 0x2C pair — "Any writes to this location (0x4c) will also change the
  content of the subsystem vendor ID at 0x2c" [1] §4.12.1, the motherboard-BIOS route.
- **Incorrect CCE packets hang the chip.** "Incorrectly-formatted packets will cause the RAGE 128
  to hang" [3] §5.2.4 — there is no documented error path; the only recoveries are the engine reset
  and the soft reset of PM4_BUFFER_CNTL's reset domain.
- **A free FIFO is not an idle engine.** GUI_ACTIVE is the OR of fourteen busy bits, and the guide's
  discipline is to wait for idle before touching CCE registers [3] §5.2.1, [2] §7.9.
- **The memory BAR mirrors the aperture-1 base.** MEM_BASE's base field "Mirror[s] bits 6:1 of
  APER_1_BASE" and CONFIG_APER_0_BASE is a read-only mirror of the same bits [2] §4.1 — the
  two-aperture scheme is steered from CONFIG_APER_1_BASE and the BAR together, not independently.
- **24-bpp pitch is stated in two different units.** "For 24-bpp format modes, the CRTC uses
  pixels*8 for the pitch, but the rendering engine uses bytes*8" [3] §3.4.3 — one mode, two pitch
  conventions, and a classic source of skewed displays.
- **DDC and the cursor share a register.** The DDC secondary pair needs CRTC_CUR_MODE[2], a field
  of the same CRTC_GEN_CNTL bits that select the cursor mode and the panel-ID multiplexing [2]
  §3.1, §6.1 — a driver that programs the cursor casually can disconnect its monitor sensing.
- **The strap pins are borrowed pins.** vga_disable rides SAD7, bustype rides SAD[5:4], enint#
  rides VSYNC — and ATI recommends external tristate buffers because an attached multimedia device
  may not release the SAD lines cleanly at reset [1] §4.13.
- **The DAC comparator only means something in the active area.** DAC_CMP_OUTPUT reads must be
  taken while the raster displays the test colour, repeatedly [2] §6.6 — a blank-period read is
  not a monitor test.
- **The OEM Pro card reports revision 0x00.** The one live card on record answers REVISION_ID 0x00
  [15] — the value the register reference calls "Initial version of RAGE 128" [2] §4.1, on Pro
  silicon; what the retail GL cards report is unobserved (§6.3).
- **The name of the chip and the name of the card disagree in the ROM.** The retail FCode publishes
  model "ATY,Rage128" and device names `ATY,Rage128n`/`ATY,Rage128v`; the OEM Pro publishes model
  "ATY,Rage128Pro" with names `ATY,Rage128Ps`/`ATY,Rage128Pd` [15] — driver matching keys on the
  name property, so the suffix is load-bearing.

## 6. Open questions

1. **The Rage Orion's own ROM and subsystem identity.** No Orion ROM or board has been examined;
   the board-family membership (109-57400) is inferred from secondary card tables. What ROM
   revision, FCode version, ndrv name and subsystem IDs an actual Orion carries is unknown — the
   single largest gap for anyone modelling the card.
2. **What the retail cards' configuration space actually reads.** No config-space dump of any
   retail Mac card exists: the subsystem-ID outcome of the strap-load quirk (§5), the BAR values,
   the cache-line and latency values the Old World firmware writes, and the reported revision are
   all unobserved.
3. **Which silicon revision the retail cards carry, and what REVISION_ID says.** A21 or A22 is
   implied by the shipping date, but the card's REVISION_ID value and the revision the Mac drivers
   key their workarounds on are unverified — and the OEM Pro card's 0x00 shows the obvious guess is
   not safe.
4. **The FCode bring-up program.** The retail ROMs are detokenized but not annotated: the
   register-init sequence, the monitor-sense program, the per-mode PLL (M, N, post) tables, the
   endian/cache-mode settings and the memory-controller initialisation the FCode performs are all
   present in the images and undecoded. Every Mac-specific claim in §4 currently ends at the
   property list.
5. **Which aperture pair and which endian/cache combination the Mac drivers use.** The register
   reference states the purpose of the duplicates; the ndrv imports `SetProcessorCacheMode` and
   the endian-swap calls, but which CONFIG_CNTL values and which of MMR/MMR_1 the shipped software
   actually touches is unobserved.
6. **Whether Mac OS uses the CCE at all, and in which mode.** The `ATI Rage 128 3D Accelerator`
   extension is the presumptive RAVE back end, but no Mac driver is disassembled: the
   PM4_BUFFER_CNTL_FIFO_MODE it writes, whether the ring is bus-mastered (and where, given no
   Mac-side GART service is documented), and which microcode it uploads are all open.
7. **The monitor-sensing sequence the Mac driver runs.** AppleSense on GPIO_MONID, DDC on
   MONID2/1 or the AGPIO2/3 pair, the DAC comparator — which the driver uses, in which order, and
   with which sense-code interpretation, is unobserved; the user's guide describes only the
   outcome.
8. **The strap state of the Macintosh cards.** vga_disable in particular: Mac OS never drives VGA,
   but whether the retail cards strap VGA off, or the FCode disables the decode via CFG_VGA_IO_DIS,
   is unestablished. Related: whether any Mac software ever touches the VGA register set (chapter
   5 of [2]) or the VGA aperture below 1 MB — which the platform does not support anyway
   ([pci.md](../pci.md) §2.2).
9. **The Apple OEM card's ROM and the Old World retrofit.** Whether the Blue & White G3's bundled
   card carries a self-contained FCode/ndrv ROM or relies on the New World ROM file — the claim that
   it is dead in an Old World machine rests on community practice, not on an examined card.
10. **The clock rates the Mac cards actually run.** The A22 advisory caps the GL at 100/90 MHz
    memory/engine; the user's guides and community tables name other figures, but the PLL values
    the Mac FCode programs are in the undecoded ROM data.
11. **The interrupt behaviour in the field.** Whether the Mac driver enables CRTC_VBLANK, GUI_IDLE
    or any other GEN_INT source, and its acknowledge sequence at the slot line, is unobserved — no
    run-time log of a retail card exists.
12. **Internal decode priority of the BAR/aperture mirror.** MEM_BASE, CONFIG_APER_0_BASE and
    CONFIG_APER_1_BASE share address bits (§5); which one wins if programmed inconsistently is not
    documented.
13. **The pixel-rate discrepancy.** The product overview claims "Pixel rates of 200 MHz" [4] while
    the controller specification states "Pixel rates up to 250 MHz standard, optional 230 MHz"
    [1] §2.1.6; which figure the GL parts honor at the margin is not resolved by any document in
    the corpus.
14. **The 3D engine's bit-level contract.** The setup-engine, interpolator, texture and blending
    registers are catalogued in [2] chapter 8 and [7], but no worked packet-level capture from
    real hardware exists to validate the corner cases (filter precision, fog tables, texture
    format corner cases) — the manuals alone are the authority, and their OCR transcription is
    imperfect in places.

## References

1. ATI Technologies Inc., *RAGE 128 VR and 128 GL Graphics Controller Specifications*, P/N
   GCS-C04100, Rev 0.05, November 1998 — §1.1 (process, voltages, part-number code); §2.1 (feature
   overview: 2D, 3D, motion video, video port, display, bus and memory support); §2.2 and Table 2-1
   (packages and chip IDs); §3.3-§3.6 (AGP/PCI, memory, monitor, PLL pin groups); §4.11 (memory
   interface: geometries, Tables 4-1 to 4-3, address mapping Table 4-4, clock bounds Table 4-5);
   §4.12 (BIOS ROM interface and the boot-up sequences, ROM-based straps); §4.13 (external
   straps Table 4-7, bus-type combinations Table 4-8, board-level buffers); §6 (electrical
   characteristics).
2. ATI Technologies Inc., *RAGE 128 VR / 128 GL Register Reference Manual*, P/N RRG-G04100-C,
   Rev 0.02, 1999 — Chapter 2 (register classification, memory mapping, apertures, MM_INDEX,
   non-Intel note, access-width Table 2-2); Chapter 3 (general I/O control, GPIO_MONID/MONIDB,
   test and debug, clock control and PLL registers §3.4); Chapter 4 (PCI configuration registers
   §4.1, AGP registers, BUS_CNTL §4.3, the aperture/configuration registers of the 0x0100 block
   and the surface registers); Chapter 5 (VGA registers); Chapter 6 (CRTC §6.1, overscan §6.2,
   hardware cursor §6.3, genlocking §6.4, memory control §6.5, DAC §6.6); Chapter 7 (2D engine:
   destination §7.1, source §7.2, host data §7.3, patterns §7.4, datapath §7.5, scissors §7.6,
   colour compare §7.7, control §7.8, status §7.9); Chapter 8 (miscellaneous registers, GEN_INT);
   Appendix A (registers by name and address).
3. ATI Technologies Inc., *RAGE 128 Software Development Guide*, P/N SDK-G04000, Rev 0.01,
   August 1999 — §3.2 (device detection, BIOS-segment scan, scratch test); §3.3 (configuration
   information and the BAR masks); §3.4.3 (manual mode setting); §3.4.4-§3.4.5 (PLL calculation);
   §3.5 (GUI engine initialisation); Chapter 4 (engine command queue maintenance, drawing
   operations); §5.2 (starting the CCE: idle, microcode, ring, bus-master set-up, cautions);
   §5.3 (ring buffer management); Chapter 6 (drawing objects and block transfers); Appendices
   A-D (BIOS functions, VESA extension).
4. ATI Technologies Inc., *RAGE 128* product overview (marketing brief), 1998 — the feature
   summary: SuperScalar Rendering, Twin Cache Architecture, Single-Pass Multi Texturing, DirectX 6
   alignment, the "Pixel rates of 200 MHz" DAC claim cited as §5's discrepancy.
5. ATI Technologies Inc., *RAGE 128 PRO Register Reference Guide*, P/N RRG-G04500-C, Rev 1.01,
   January 2000 — §4.1 (DEVICE_ID and the Pro IDs); Appendix B (the register-change history against
   the VR/GL specification: the flat-panel, TMDS, auxiliary-window and pad-control register
   additions).
6. ATI Technologies Inc., *Rage 128 Register Reference Supplement: Bus Master Registers* — the
   BM_* register set at MMR offsets 0x0A00-0x0AB0: frame-buffer/system address, BM_COMMAND,
   status, queue status and free status, chunk values, the VIP buffer descriptors.
7. ATI Technologies Inc., *Rage 128 Register Reference Supplement: Registers for CCE 3D Packets* —
   the PM4_* register set (PM4_BUFFER_OFFSET/CNTL/WM_CNTL/DL_RPTR_ADDR/DL_RPTR/DL_WPTR at
   0x0700-0x0714, PM4_VC_* and PM4_IW_* at 0x0730-0x073C, PM4_STAT at 0x07B8, PM4_MICROCODE_* at
   0x07D4-0x07E0, PM4_MICRO_CNTL at 0x07FC) and the 3D packet and draw-state register pages
   (SETUP_CNTL, SCALE_3D_CNTL and the interpolator, texture and blending registers).
8. ATI Technologies Inc., *Rage 128 Register Reference Supplement: Multimedia Registers* — the
   overlay, scaler, capture, VIP, iDCT and subpicture register sets (OV0_*, VID*, CAP0_*/CAP1_*,
   VIP_*, SUS_*).
9. ATI Technologies Inc., Product Advisory ER_R4A1, *RAGE 128 (Rev. A12) ERRATA & BRANDING*,
   September 15, 1998 — the A12 engineering-sample errata (screen corruption, hangs, the 75 MHz
   reduced clocks) and the branding part numbers.
10. ATI Technologies Inc., Product Advisory ER_R4B1, *Rage 128 Rev A13 Errata & Branding*,
    October 15, 1998 — the eight A13 errata (pixel-cache write failures and deadlocks, Vil
    threshold, DP_WRITE_MASK/Z plane, the 128 MB aperture, the 66 MHz bit at 33 MHz, NAND-tree,
    arbitration) with their A21 fix schedule, and the A13 branding.
11. ATI Technologies Inc., Product Advisory ER_R4D1, *RAGE 128 VR/GL Rev A22 Errata and Branding*,
    January 8, 1999 — the A22 errata (2.8 V core requirement and its A21/A23 scope, LCD 5-pixel
    shift, AGP_PUMP_GAIN, pixel-cache yield, line AA, texture mirroring) and the A22 branding
    part numbers.
12. ATI Technologies Inc., Product Advisory PA_R4B1, *RAGE 128 VR/GL Rev A22 DESIGN UPDATES*,
    January 21, 1999 — the design updates: 2.8 V core supplies, heat-sink recommendation and
    thermal data, the maximum memory/engine clock frequencies (125/80 VR, 100/90 GL), the
    oscillator-over-crystal requirement, power-supply noise filtering, the 374 Ω RSET value, and
    the reference-schematic list.
13. ATI Technologies Inc., *ATI NEXUS 128 User's Guide*, P/N 107-40201-10, Version 1.0, April 1999
    — the introduction and requirements (pp. 1-2); monitor sensing, DDC and sync-on-green (p. 9);
    software installation (pp. 11-12); the 3D feature set (p. 14); the video mode tables (pp. 29-31)
    and the specifications (pp. 32-33).
14. ATI Technologies Inc., *ATI RAGE ORION User's Guide*, P/N 107-40202-10, 1999 — the
    corresponding Orion material: introduction and requirements (pp. 1-2), monitor sensing (p. 9),
    the video mode tables (pp. 29-31) and the specifications with the 16 MB memory and 16 MB
    relocatable aperture (pp. 32-33).
15. The Macintosh Rage 128 card ROM record — four 128 KB flash images with their PCI data
    structures and detokenized FCode: the retail Nexus 128 ROM 113-57502-103 (device 1002:5245,
    code type 1, image 0x12000 bytes, FCode 0x11EAA bytes, properties name/`ATY,Rom#`/`ATY,Card#`/
    `ATY,Fcode`/`ATY,MEM`/`ATY,memsize`/`ATY,Flags`/`ATY,Status`, the embedded
    `driver,AAPL,MacOS,PowerPC` PEF and its imported-library list), the retail Xclaim VR 128 ROM
    113-57406-108 (same structure, image 0x1CA00 bytes, name `ATY,Rage128v`), the OEM Rage 128 Pro
    AGP ROMs 113-63001-110 and 113-72701-136 (device 1002:5046, FCode 1.70), and the ATI card-info
    utility dump of the 113-63001-110 ROM on a real Mac (the flash part, `regPLL_13`, revision ID
    and the published FCode properties of the OEM card).
16. Apple Computer, Inc., *Developer Note: Power Macintosh G3* (Blue and White), 1999 —
    §"Graphics Card" (the Rage 128 GL accelerated card, its 16 MB of SDRAM and its acceleration
    and 3D features), §"Core ICs" (the card as one of the machine's five core ICs), and the
    display-resolution table of §"Display Resolution Modes" for the accelerated card.
