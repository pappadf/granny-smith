# The IMS TwinTurbo 128

**Contents:**

1. [Overview](#1-overview) — the chip and the vendor, the cards, host machines and OEM provenance, the
   evidence base and its limits
2. [Register file](#2-register-file) — PCI identity and the memory map, the chip register file, the two
   DAC register files, configuration and status registers, reset state
3. [Behaviour](#3-behaviour) — endianness, the draw engine, monitor sensing, clocking, interrupts
4. [Programming model](#4-programming-model) — the expansion ROM container, the FCode bring-up, the mode
   tables, the DAC init programs, the Mac OS driver stack, the Linux driver as a second witness
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 The chip and the vendor

The **IMS TwinTurbo 128** is a 128-bit VRAM graphics accelerator from Integrated Micro Solutions
(IMS, later **ixMicro**): a single PCI controller chip plus a separate palette DAC, described by its
vendor as "a high performance 128-bit display adapter for PCI Power Macintosh computers", with "a
high speed 32-bit PCI bus interface, 128-bit VRAM graphics acceleration, and Video Playback
acceleration" [1] Introduction. The chip itself is **Cosmo** — that name is the register prefix in
the Linux driver's source ("TwinTurbo (Cosmo) registers" [3]) and the naming scheme of every chip
register variable in the card's own FCode (`cosmo_hes`, `cosmo_scr`, …) [2], so it is the part's own
name, not a label applied from outside. The silicon family has two PCI identities: device **$9128**
(TT128, the Macintosh cards of this page) and device **$9135** (TT3D, the later 3D-capable variant
with a TI TVP RAMDAC path) [3]; the Macintosh-visible member is the TT128, and the TT3D is covered
here only where the shared driver treats it.

The accelerator processes "128-bits of graphics data every two clock cycles" — 16 pixels in
256-colour mode, 8 in thousands, 4 in millions — and, being VRAM-based rather than DRAM-based,
"screen resolution and refresh rate have little effect on graphics acceleration performance" [1]
Introduction. The board carries "Video Playback acceleration" hardware, and "special hardware …
included on the Twin Turbo-128 display adapter" accelerates video playback; a "special YUV-RGB
hardware" path on the Twin Turbo-128M and M2 is described as reserved "to accelerate software MPEG
movie playback in future software releases" [1] Introduction (§4.5 records that a later Mac OS
driver did ship such a path).

There is no VGA core: the card is a pure linear-framebuffer accelerator, and — unusually for the
1995–1997 display-card generation — it uses **no PCI I/O space at all** (§2.1), where the Apple
mach64 card of the same platform boots through sparse I/O ([mach64.md](mach64.md) §2.2).

### 1.2 The cards

The Macintosh card line spans several memory configurations, all on the same TT128 silicon and the
same PCI identity ($10E0:$9128). The model letter names the memory size; the letter suffix (on the
Apple-shipped cards) names the RAMDAC generation.

| Card | VRAM | RAMDAC | Open Firmware `name` | ROM on record | Notes |
|---|---|---|---|---|---|
| Twin Turbo-128M | 4 MB soldered [1] App. B | not established | not established | — | base retail model; FCC ID MAJPCI9501 [1] App. C |
| Twin Turbo-128M2 | 2 MB, upgradeable to 4 MB with VRAM DIMMs [1] App. B | IBM 624-class ("Irazu" in the card's FCode) [2] [3] | `IMS,tt128mb` [2] | `ixmicrotwinturbo_M2` (64 KB) [2] | FCC ID MAJPCI9521 [1] App. C; the DIMM sockets force a physical slot constraint [1] Ch. 1 |
| Twin Turbo-128M4A | 4 MB, not expandable (*observed*: Apple's 9600/200 card is "4 MB of buffer memory and cannot be expanded" [4]) | IBM 624-class (*inferred*: the Linux driver attributes its IBM register set to "the 2MB and 4MB cards" [3]) | `IMS,tt128mbA` (*observed* as the ndrv driver description name in the Mac OS driver record [7]) | — | Apple OEM card of the Power Macintosh 9600/200 and /233 [4] [6] |
| Twin Turbo-128M8 | 8 MB [1] App. B | not established | not established | — | "FCC class A only" [1] App. B; 32 MB system RAM recommended [1] Ch. 1 |
| Twin Turbo-128M8A | 8 MB, not expandable (*observed*: Apple's enhanced-9600 card is "8 MB of buffer memory and cannot be expanded" [5]) | TI TVP3030-class ("Zelea" in the card's FCode) [2] [3] | `IMS,tt128mb8A` [2] | `TwinTurbo 128M8A Revision 3.8Ab2` (64 KB) [2] | Apple OEM card of the Power Macintosh 9600/300 and /350 [5]; "often shipped with the PowerMac 9600" [7] |

The two ROM dumps on record are the two ends of the line — the 2 MB IBM-DAC M2 and the 8 MB
TVP-DAC M8A — and they happen to be the two witnesses this page leans on hardest (§1.4). The base
M and the M4A are documented only at the board level ([1] [4]) and through the Mac OS driver's
match names [7]; no ROM image of either is in the evidence set (§6.6).

Board-level facts common to the line [1] Appendix B: the output signal is "RS-343 RGB with
selectable sync on green. Separate syncs included"; sync requirements are "Horizontal 31 khz to
93 khz, Vertical 60 hz to 100 hz"; supply voltage 4.75–5.25 V at 1.7 A; and the back panel carries
**two connectors** — the Apple standard video connector (DB-15, sense lines on pins 4, 7 and 10
[1] App. B) and a VGA-style Mini D-Sub, with "the resolutions available for display … dependent on
which video connector is being used" [1] Ch. 1. The M2's VRAM DIMM upgrade is user-installable, with
a published compatibility table (NEC uPD482234, Toshiba TC528257, OKI M548262 and Samsung
KM428C256 families; the -60/-70 timing grades marked compatible, the faster -35-grade parts not)
and a JP2 jumper that "may need to be removed" if vertical stripes appear [1] App. B.

### 1.3 Host machines and OEM provenance

The TwinTurbo is a **PCI Power Macintosh card subject**: it requires "a PCI based Power Macintosh"
[1] Ch. 1, and the platform contract — the three address spaces, the configuration header, the
big-endian host to little-endian bus seam — is [pci.md](../pci.md)'s and is cited here, never
restated. Its host machines, in the evidence set:

- **Power Macintosh 9600/200 and 9600/200MP** (1996): "a new accelerated display card (Power
  Macintosh 9600 only)" — 4 MB, non-expandable [4] Ch. 1. Apple's own announcement of the 9600/233
  and 9600/200 names the part: "une nouvelle carte vidéo IMS Twin Turbo 128 M4A" [6]. The machine
  itself is the six-slot TNT platform machine ([tnt.md](../../../machines/tnt/tnt.md) §6.5), which
  presents itself to software as a 9500 ([tnt.md](../../../machines/tnt/tnt.md) §1.2).
- **Power Macintosh 9600/300 and 9600/350** (1997, the "enhanced" 9600): the display card has
  "8 MB of buffer memory and cannot be expanded", with 8, 16 and 24 bits per pixel on all monitor
  sizes [5] Ch. 1. This is the M8A whose ROM and driver description are on record [2] [7].
- **Any other PCI slot**: the card is a retail product that "works on other PCI PowerMacs as well"
  [7], and the manual's system requirements allow "PCI Power Macintosh or Compatible computer"
  [1] App. B. Third-party Macintosh clones are also recorded as TwinTurbo hosts in the collecting
  record, but no formal document in the evidence set names a clone configuration (§6.9).

What the TwinTurbo is *not*: it is not the card of the 9500 — that machine's bundled card is the
Apple mach64 board ([mach64.md](mach64.md) §1.2) — and the 8600 has built-in video of its own, so
the TwinTurbo appears there only as a retail upgrade. The contrast with the ATI card generation is
the reason this page exists as a card subject: the TwinTurbo is the *other* Old World PCI display
card, from the *other* silicon vendor, with a completely different register architecture and a
completely different boot discipline (memory-only, §2.1; blit engine, §3.2).

### 1.4 Evidence base and its limits

No IMS register datasheet is publicly available — the vendor manual is a user's installation and
control-panel guide [1], not a programmer's reference. The register file below is therefore
reconstructed from **two independent primary witnesses** that happen to agree at almost every
point where they overlap:

- the vendor's own **FCode**, in two ROM dumps (M8A revision 3.8Ab2 and M2), detokenized
  byte-exactly here — the property list, the register-init programs, the mode tables, the
  monitor-sense program and the RAMDAC init sequences [2]; and
- the **Linux kernel's `imsttfb` driver** — officially released source, itself descended from the
  1997 powermac console `imstt` driver, with "some register values … contributed by Damien Doligez,
  INRIA Rocquencourt" [3] — the only public register-level program for the chip.

Board-level facts come from the vendor manual [1]; OEM facts from the two Apple developer notes
[4] [5] and Apple's press release [6]; the Mac OS run-time driver from the preserved "9600 Graphics
Accelerator" extension record [7]; and the platform contract from [pci.md](../pci.md). The honest
consequence: the page is **register-grade for the boot and mode-set path** — the path both witnesses
exercise — and **observed-only for the draw engine** (one blit program family, [3]) and for
everything neither witness touches, which is a great deal: the engine's command encoding beyond the
observed values, the interrupt story, reset values, and the silicon internals. Those live in
[§6](#6-open-questions) rather than in padded prose.

## 2. Register file

### 2.1 PCI identity and the memory map

The PCI identity, as the two ROM images declare it and the Linux driver matches it:

| Field | Value | Witness |
|---|---|---|
| Vendor ID | $10E0 (IMS) | PCIR of both ROMs [2]; the driver's `PCI_VENDOR_ID_IMS` [3] |
| Device ID | $9128 (TT128); $9135 (TT3D) | PCIR [2]; driver device table [3] |
| Class code (ROM PCIR) | $038000 on the M8A image (display controller, sub-class *other*); $030000 on the M2 image (display, VGA-compatible) | *observed* in the two PCIR structures [2] — note the M2's "VGA-compatible" claim, which the hardware contradicts (no VGA core) |
| Expansion ROM | one image, code type $01 (Open Firmware), indicator $80 (last image); 61,440 bytes (M8A) / 55,296 bytes (M2) | *observed* [2] |
| BARs | one 32-bit memory BAR (offset $10), size $02000000 (32 MB); expansion ROM BAR ($30), size $00010000 (64 KB); **no I/O BAR** | *observed*: both ROMs publish `reg-space-size $02000000` and `rom-space-size $00010000` [2]; the Linux driver takes BAR0 and maps three regions out of it [3] |

The card is a memory-only device: every register — chip, RAMDAC, framebuffer — is reached through
the single 32 MB BAR. This is the structural difference from the ATI cards of the same platform,
whose mach64 generation keeps a bootstrap register in sparse I/O because its memory aperture is
disabled at power-up ([mach64.md](mach64.md) §2.2); the TwinTurbo's aperture is permanently decoded,
so its FCode never asks Open Firmware for an I/O window at all (§4.2).

Inside the 32 MB BAR, three regions (the Linux probe's mapping, confirmed by the FCode's own
register constants [2] [3]):

| BAR offset | Size used | Contents |
|---|---|---|
| +$000000 | 2 MB (M2), 4 MB (M4A, *observed* [4]), 8 MB (M8/M8A [1] [5]) | the linear framebuffer |
| +$800000 | 4 KB | the chip (Cosmo) register block — all 32-bit registers, §2.2 |
| +$840000 | 4 KB | the RAMDAC register block — byte registers at a 4-byte stride, §2.3 |

The FCode's `reg` property declares the memory BAR **prefetchable** (its `phys.hi` constant is
$42000010; the bit meanings are [pci.md](../pci.md) §2.7's) [2] — even though the same 32 MB region
contains the control registers, a combination a conservative PCI implementation maps
non-prefetchable. The Apple mach64 card's BAR is assigned non-prefetchable in its
`assigned-addresses` ([mach64.md](mach64.md) §4.1); the TwinTurbo's `reg` claims the opposite for a
region that arguably deserves it less (§5).

### 2.2 The chip register file

The register file the Linux driver enumerates and the FCode addresses — one file, little-endian
32-bit registers, offsets within the +$800000 block. The FCode names are the vendor's own
(`cosmo_*`) [2]; the driver names are `imsttfb`'s [3]; where a register's role is exercised by
either witness it is stated, otherwise the table just records that the register exists.

| Offset | Driver name | FCode name | Role as evidenced |
|---|---|---|---|
| $00 | S1SA | — | blit source-1 start address (fill-XOR source; copy source) [3] |
| $04 | S2SA | — | blit source-2 start address [3] (never written by the witnesses' programs) |
| $08 | SP | — | blit source pitch/step: `(pitch << 16) \| (±pitch & $FFFF)` for copies; pitch for fills [3] |
| $0C | DSA | — | blit destination start address (byte offset into the framebuffer) [3] |
| $10 | CNT | — | blit extent: `(height-1) << 16 \| (width-1)`, the width halfword negated for right-to-left copies [3] |
| $14 | DP_OCTL | — | blit destination pitch/step (±pitch) [3] |
| $18 | CLR | — | blit fill colour, host-replicated across the pixel [3] |
| $20 | BI | — | blit mask 1 — written $FFFFFFFF by the fill program [3] |
| $24 | MBC | — | blit mask 2 — written $FFFFFFFF by the fill program [3] |
| $28 | BLTCTL | — | blit command/start (observed values: $840 fill-copy, $40005 fill-XOR, $05 copy, $85 right-to-left copy; §3.2) [3] |
| $30 | HES | cosmo_hes | scan timing — horizontal end of sync [2] [3] |
| $34 | HEB | cosmo_heb | scan timing — horizontal end of back porch [2] [3] |
| $38 | HSB | cosmo_hsb | scan timing — horizontal start of blank [2] [3] |
| $3C | HT | cosmo_ht | scan timing — horizontal total [2] [3] |
| $40 | VES | cosmo_ves | scan timing — vertical end of sync [2] [3] |
| $44 | VEB | cosmo_veb | scan timing — vertical end of back porch [2] [3] |
| $48 | VSB | cosmo_vsb | scan timing — vertical start of blank [2] [3] |
| $4C | VT | cosmo_vt | scan timing — vertical total [2] [3] |
| $50 | HCIV | cosmo_hciv | written 1 by both witnesses at mode set [2] [3] |
| $54 | VCIV | cosmo_vciv | written 1 by both witnesses at mode set [2] [3] |
| $58 | TCDR | cosmo_tcdr | written 4 by both witnesses at mode set [2] [3] |
| $5C | VIL | cosmo_vil | vertical interrupt line — both witnesses copy VSB's value into it [2] [3] |
| $60 | STGCTL | cosmo_stgctl | scan timing generator control: bit 0 (cleared at init [3]), bit 1 (cleared while reprogramming [2]), bits 4/5 (horizontal/vertical sync outputs [3]), bits 7–9 ($380 — cleared for monitor sensing and blanking, restored after [2] [3]) |
| $64 | SSR | cosmo_ssr | screen start row — panning; written 0 at mode set, then the pan offset in 8-byte units [2] [3] |
| $68 | HRIR | cosmo_hrir | written $0100 (half-SAM boards) / $0200 (full-SAM) at mode set [2] [3] |
| $6C | SPR | cosmo_spr | screen pitch — display width in 64-bit units: width/4, halved again on 8 MB boards [2] [3] |
| $70 | CMR | cosmo_cmr | written $FF (half-SAM) / $1FF at mode set [2] [3] |
| $74 | SRGCTL | cosmo_srgctl | screen refresh generator control — the version/EDO-dependent init values of §4.2, then $03 (+$40 half-SAM) at mode set [2] [3] |
| $78 | RRCIV | cosmo_rrciv | RAM refresh control — written 1 [2] [3] |
| $7C | RRSC | cosmo_rrsc | RAM refresh — written $980 [2] [3] |
| $88 | RRCR | cosmo_rrcr | RAM refresh — written $11 at mode set, bit 1 cleared during reprogramming [2] [3] |
| $80 | GIOE | cosmo_gioe | GPIO output enable — the monitor sense line drivers, bits 5:3 [2] |
| $84 | GIO | cosmo_gio | GPIO — sense line read-back, bits 5:3; bit 2 is polled by the M2's monitor-present test [2] |
| $8C | SCR | cosmo_scr | system control — memory configuration and byte order: bits 9:8 are the per-depth byte-swap pair [3]; the board-variant OR bits ($400 8 MB, $04 8-column, $01 EDO) [2]; init and mode-set values in §4.2 |
| $90 | SSTATUS | cosmo_sstatus | status: bit 7 busy, bit 6 blit-complete (§3.2); bits 11:8 the chip version (§2.4) [2] [3] |
| $94 | PRC | cosmo_prc | card configuration straps (§2.4) [2] [3] |

Offsets $7C/$88: the driver's enumeration places RRCSC at $7C and RRCR at $88, leaving $84's
neighbour $80/$84 for the GPIO pair — the FCode constants confirm every one of these addresses
independently [2]. The engine group ($00–$28) is *never touched by the FCode* — Open Firmware draws
the boot screen with plain CPU fills through the linear framebuffer [2] — so everything known about
the engine comes from the Linux driver's fill and copy programs (§3.2) and whatever the Mac OS
driver's ndrv does, which is not decoded at register level in the evidence set (§6.2).

### 2.3 The RAMDAC register files

Two DAC generations ship on TT128 cards, and the card's FCode gives each its own name — the M2's
**"Irazu"** register set and the M8A's **"Zelea"** register set [2]. The indirect register lists
identify them beyond reasonable doubt as the **IBM 624 palette DAC** family and the **TI TVP3030**
family respectively, because the Linux driver's IBM and TVP indirect maps match the FCode
name-for-name and index-for-index [3] (*inferred* from that match; no datasheet for either part is
cited on this page, and the identification is only as strong as the match). Both live at BAR
+$840000, byte-wide registers at a 4-byte stride.

**IBM 624-class ("Irazu") — the M2's DAC.** Direct registers [2] [3]:

| Offset | Driver name | FCode name | Role |
|---|---|---|---|
| $00 | PADDRW | irazu_clutwraddr | palette write address (auto-increments per colour) |
| $04 | PDATA | irazu_clutdata | palette data (three writes per colour: R, G, B) |
| $08 | PPMASK | irazu_pixmask | pixel read mask — written $FF at init by both witnesses |
| $0C | PADDRR | irazu_clutrdaddr | palette read address |
| $10 | PIDXLO | irazu_indlow | indexed register address low |
| $14 | PIDXHI | irazu_indhigh | indexed register address high (the cursor RAM lives at index page 1) |
| $18 | PIDXDATA | irazu_datareg | indexed register data |
| $1C | PIDXCTL | irazu_indctl | indexed register control — the FCode writes 1 at init [2] |

Indexed registers (address low; the parenthesized reset values are the Linux driver's annotations)
[3], the subset the FCode names confirmed [2]:

| Index | Name | Reset | Role |
|---|---|---|---|
| $02 | CLKCTL | (0x01) | clock control — PLL enable; $21 at FCode init [2], $01 unblank / $C0 powered down [3] |
| $03 | SYNCCTL | (0x00) | sync control — $0F to force syncs low in power-down [3] |
| $04 | HSYNCPOS | (0x00) | horizontal sync position |
| $05 | PWRMNGMT | (0x00) | power management — $1F powers down, $00 restores [3] |
| $06 | DACOP | (0x02) | DAC operation — FCode writes $0B, the Linux driver $02 (§5) |
| $07 | PALETCTL | (0x00) | palette control |
| $08 | SYSCLKCTL | (0x01) | system clock control — "PLL programming enabled" |
| $0A | PIXFMT | — | pixel format: `bpp >> 3 + 2` — 8 bpp writes 3, on both witnesses [2] [3] |
| $0B | BPP8 | — | 8 bpp control — 0 written at init |
| $0C | BPP16 | — | 16 bpp control — $01 for 555, $03 for 565 [3] |
| $0D | BPP24 | — | 24 bpp control |
| $0E | BPP32 | — | 32 bpp control |
| $10 | PIXCTL1 | (0x05) | pixel PLL control 1 — $05 written at clock set on both witnesses |
| $11 | PIXCTL2 | (0x00) | pixel PLL control 2 — 0 written at clock set |
| $15 | SYSCLKN | — | system clock N (reference divider) |
| $16 | SYSCLKM | — | system clock M (VCO divider) — $4F at FCode init [2] [3] |
| $17 | SYSCLKP | — | system clock P |
| $18 | SYSCLKC | — | system clock C (charge pump bias) |
| $20 | PIXM0 | irazuind_pixelm0 | pixel PLL M0 |
| $21 | PIXN0 | irazuind_pixeln0 | pixel PLL N0 |
| $22 | PIXP0 | irazuind_pixelp0 | pixel PLL P0 |
| $23 | PIXC0 | irazuind_pixelc0 | pixel PLL C0 — $02 written by both witnesses |
| $30–$48 | CURSCTL … CURS3B | — | hardware cursor control, position and the three cursor colours — initialized *off* (CURSCTL 0) by the driver [3] |
| $60–$62 | BORDR/BORDG/BORDB | — | border colour |
| $70 | MISCTL1 | irazuind_ctl1 | miscellaneous control 1 — $01 at init, $11 in power-down |
| $71 | MISCTL2 | irazuind_ctl2 | miscellaneous control 2 — $45 at init, $55 in power-down |
| $72 | MISCTL3 | irazuind_ctl3 | miscellaneous control 3 — $00 |
| $78 | KEYCTL | — | key control / DB operation |

The dot clock for this DAC is "20MHz * (m + 1) / ((n + 1) * (p ? 2 * p : 1))", with "c … the charge
pump bias which depends on the VCO frequency" [3]; §4.3 tabulates the M2's mode tables against that
formula and they agree to the mathematical hair.

**TI TVP3030-class ("Zelea") — the M8A's DAC.** Direct registers [2] [3]:

| Offset | Driver name | FCode name | Role |
|---|---|---|---|
| $00 | TVPADDRW | zelea_indexreg | palette/cursor RAM write address and index |
| $04 | TVPPDATA | zelea_clutdata | palette data |
| $08 | TVPPMASK | zelea_pixmask | pixel read mask |
| $0C | TVPPADRR | zelea_clutrdaddr | palette/cursor RAM read address |
| $10 | TVPCADRW | zelea_cur_colorwraddr | cursor/overscan colour write address |
| $14 | TVPCDATA | zelea_cur_colordata | cursor/overscan colour data |
| $1C | TVPCADRR | zelea_cur_colorrdaddr | cursor/overscan colour read address |
| $24 | TVPDCCTL | zelea_cur_control | direct cursor control |
| $28 | TVPIDATA | zelea_datareg | indirect index data |
| $2C | TVPCRDAT | zelea_cur_ramdata | cursor RAM data (512 bytes of cursor pattern) |
| $30/$34 | TVPCXPOL/TVPCXPOH | zelea_cur_xlow/zelea_cur_xhigh | cursor X position |
| $38/$3C | TVPCYPOL/TVPCYPOH | zelea_cur_ylow/zelea_cur_yhigh | cursor Y position |

Indexed registers [2] [3] (driver name, index, parenthesized resets): TVPIRICC indirect cursor
control $06 (0x00); TVPIRBRC byte router control $07 (0xe4); TVPIRLAC latch control $0F (0x06);
TVPIRTCC true-colour control $18 (0x80 — 8 bpp; $44 555; $45 565; both witnesses [2] [3]);
TVPIRMXC multiplex control $19 (0x98; $4D at 8 bpp [2] [3]); TVPIRCLS clock selection $1A (0x07;
$05 at init [2]); TVPIRPPG palette page $1C (0x00); TVPIRGEC general control $1D (0x00 — bit $20
is the sync-on-green pair, §3.3); TVPIRMIC miscellaneous control $1E (0x00; $08 at init [2] [3]);
TVPIRPLA PLL address $2C; TVPIRPPD pixel clock PLL data $2D; TVPIRMPD memory clock PLL data $2E;
TVPIRLPD loop clock PLL data $2F; TVPIRMLC MCLK/loop clock control $39 (0x18; $38 ORed by the
FCode when the loop clock is selected [2]); TVPIRSEN sense test $3A (0x00); TVPIRTMD test mode
data $3B; TVPIRRML/TVPIRRMM CRC remainder $3C/$3D; TVPIRRMS CRC bit select $3E; TVPIRDID device
ID $3F (0x30); TVPIRRES software reset $FF.

The indexed-address mechanism differs between the two DACs and both are observed: the IBM part
takes an 8-bit index through PIDXLO/PIDXHI and data through PIDXDATA [3]; the TVP part takes its
indirect index through the *direct* index register at $00 and exchanges data through $28 [2] [3].

### 2.4 Configuration and status registers

Two registers carry the card's identity, and both witnesses read them at bring-up:

- **PRC** (offset $94) reports the board configuration. The FCode decodes four bits [2]: bit 9
  ($200) — "half SAM", the VRAM serial-access-memory organization; bit 8 ($100) — EDO VRAM; bit 6
  ($40) — "8 column" memory; bit 10 ($400) — 8 MB. The Linux driver reads bit 2 ($04) to size an
  IBM-DAC card — "(tmp & 0x0004) ? 0x400000 : 0x200000" [3] — so on the M2-class boards bit 2 is
  the 2-versus-4 MB strap. (The two bit sets are not contradictory: they are different board
  properties decoded from the same register; which bits are fitted on which board is §6.1.)
- **SSTATUS** (offset $90) reports the chip version in bits 11:8 — the FCode's `isversion2` test
  is `((SSTATUS & $F00) >> 8) == 2` [2], and the Linux driver prints "chip version %u" from the
  same field [3]. Both witnesses branch their SRGCTL init value on silicon generation (§4.2).

SCR is the memory- and byte-order control register; its observed bits: bits 9:8 select the
per-depth byte swap — "bits 8 and 9 in the SCR register control endianness correction (byte
swapping). These bits must be set according to the color depth", 8 bpp $000, 16 bpp $100, 24 bpp
$200, 32 bpp $300, a table the Linux author attributes to "poking around with MacsBug" on a real
card [3]; the FCode ORs in the board bits ($400 with 8 MB, $04 with 8-column, $01 with EDO) [2];
and the mode-set writes are $D098 (M2) and $15098 (M8A) [2] — the Linux driver's own values for
2 MB ($059D) and 4/8 MB ($150DD) boards [3] share the high half with the FCode's and differ in the
low configuration bits, consistent with per-board tuning. SCR $80 is the FCode's first mode-set
write, before the timing registers are reprogrammed [2].

### 2.5 Reset state

No document in the evidence set gives reset values for any Cosmo register, and neither witness
reads a register before writing it in the bring-up path (the first chip-register reads are the
PRC/SSTATUS configuration probes after the first init writes [2]). The only anchored pre-state is
behavioural: the Linux driver clears STGCTL bit 0 as its first register operation "to initialize
the card" [3], which implies the display is live-or-don't-care at reset and must be quiesced
rather than assumed. Everything else about power-on state is §6.5.

## 3. Behaviour

### 3.1 Endianness

The card is a little-endian PCI device on the big-endian Power Macintosh host, on the platform
seam of [pci.md](../pci.md) §3.4. All register traffic observed in both witnesses is explicitly
little-endian: the Linux driver wraps every chip-register access in `read_reg_le32` /
`write_reg_le32` ("in_le32"/"out_le32" on PowerPC) [3], and the card's own FCode uses the Open
Firmware little-endian accessors `rl@`/`rl!` for every chip register and `c@`/`c!` for the DAC
bytes [2]. Unlike the Rage 128's answer to the same problem — two complete copies of every aperture
so the host can map each independently ([rage-128.md](rage-128.md) §3.1) — the TwinTurbo has one
aperture and solves pixel order **inside the chip**, through the SCR byte-swap pair (§2.4): the
driver's per-depth values keep the framebuffer correct for a big-endian host at 8/16/24/32 bpp
[3]. The DAC's pixel format registers carry the same duty on the DAC side (PIXFMT on the IBM part,
the TVP true-colour/multiplex pair on the TVP part, §2.3).

### 3.2 The draw engine

The engine group (§2.2, offsets $00–$28) is programmed by issue-style register writes: set up
masks, colour, source and destination geometry, then write BLTCTL to start. The complete observed
repertoire is the Linux driver's three programs [3], reproduced here because they are the only
register-grade description of the engine that exists:

**Solid fill (ROP_COPY)** — wait `SSTATUS & $80` clear; `DSA = y·pitch + x·Bpp`; `CNT =
((height−1) << 16) | (width·Bpp − 1)`; `DP_OCTL = pitch`; `BI = $FFFFFFFF`; `MBC = $FFFFFFFF`;
`CLR` = the colour replicated to all bytes; `BLTCTL = $840`; then wait bit 7 clear and bit 6 clear.

**Fill, XOR mix** — same DSA/CNT/DP_OCTL, plus `S1SA = DSA` and `SP = pitch` (the destination is
also the source), then `BLTCTL = $40005`, and the same completion waits.

**Screen-to-screen copy** — the direction contract lives in the operands: if `sy < dy` the copy
starts from the *bottom* row (`sy += height; dy += height; sp |= −pitch & $FFFF; DP_OCTL =
−pitch & $FFFF`), else from the top; if `sx < dx` it starts from the *right* edge
(`bltctl |= $80` — the horizontal-direction bit — and the CNT width halfword is written negated,
`−width & $FFFF`). Then `S1SA = old`, `SP`, `DSA = new`, `CNT`, `DP_OCTL`, `BLTCTL = bltctl`,
and the same two waits [3]. This is the same far-edge-first rule that makes overlapping scrolls
correct on the mach64 ([mach64.md](mach64.md) §3.4), expressed as a signed step instead of
direction bits.

The two status bits the programs poll: SSTATUS bit 7 — wait-for-idle before *submitting* — and
bit 6 — the blit-complete flag waited on after submission [3]. What BLTCTL's other bits encode
(fine-grained rop selection, source 2, line draws, the meaning of the $840 vs $40005 command
bits) is not exercised by any witness and is §6.2. The FCode never uses the engine: Open Firmware
paints boot screens with `fill` and `move` through the linear framebuffer [2] — so a
re-implementation's engine can be entirely unexercised by the boot path.

### 3.3 Monitor sensing

The sense hardware is the Apple standard three-line scheme on DB-15 pins 4, 7 and 10 [1] App. B,
read through GIO/GIOE bits 5:3. The FCode's sense program is the same four-step protocol as the
Apple cards': read the primary code with all lines released (`GIOE` bits 5:3 cleared, then read
`GIO & $38 >> 3`), then drive each line low in turn and read the other two back, packing the
result into a 6-bit extended code, then restore all-input [2]. The mach64 card's identical
program is ([mach64.md](mach64.md) §3.6)'s.

The FCode then maps the codes to modes with a case statement on the primary code and, for codes 6
and 7, on the extended code [2]:

| Sense code | Extended code | Selected mode (M8A ROM) | Extended code | Selected mode |
|---|---|---|---|---|
| 0 | — | 1152×870@75 | | |
| 3 | — | 1152×870@75, monochrome flag set | | |
| 6 | $03 | 832×624@75 | $23 | 1152×870@75 |
| 7 | $2D | 832×624@75 | $3A | 1024×768@75 |
| 7 | $17 | 640×480@60 (VGA) | $3F | 640×480@60 (VGA) |
| other | — | 640×480@67 (the Apple default) | | |

The M2 ROM's decode table is identical [2]. Note that $3F — the all-floating code — is mapped to a
mode rather than treated as "no monitor"; the *presence* test is separate and different on the two
ROMs: the M2's `monitor_attached` clears STGCTL's $380 bits, forces GIOE to 0, reads GIO **bit 2**
and restores; the M8A's `monitor_attached8` first sets 640×480@60 and paints the screen grey
($70,$70,$70), waits 23 ms, and polls the TVP's **sense test** register bit 0 instead [2]. The
open aborts when no monitor is attached — the card's FCode, like the Apple card's, refuses to open
without one. What GIO bit 2 is wired to on the M2 is §6.14.

Connector behaviour at the user level: with the Apple connector and a fixed-sense monitor, only
that monitor's safe resolutions are offered; with the VGA connector, VGA resolutions are offered
instead, and "the Apple 20" monitor or an equivalent with an external sense code adapter or using
the VGA connector" unlocks 1600×1200 when the Option key is held during the resolution click [1]
Ch. 3. A wrong sense choice is recoverable by deleting the Display Preferences file or zapping
PRAM [1] App. A.

Sync-on-green: the retail manual's card offers "selectable sync on green" [1] App. B, with a
control panel to disable it if the picture "has a 'Greenish' or 'Purplish' cast" [1] App. A, and
the M8A's FCode carries the switch as TVP general control bit $20 (`set_sog`) [2]. Apple's
developer notes, by contrast, state the 9600 cards "do not support sync on green" [4] [5] —
whether the Apple-shipped boards omit the feature, the driver disables it, or the notes are
describing the Apple driver's policy is not separable in the evidence set (§6.12).

### 3.4 Clocking

**IBM DAC (M2):** the pixel PLL is programmed through indexed registers PIXM0/PIXN0/PIXP0/PIXC0
after setting PIXCTL1=$05, PIXCTL2=$00 [2] [3]; the frequency is

```
dot clock = 20 MHz × (m + 1) / ((n + 1) × (p ? 2p : 1))
```

with c the charge-pump bias "which depends on the VCO frequency" [3]. The FCode's mode tables
(§4.3) carry exactly these quadruples and the computed frequencies land on the standard Apple/VGA
dot clocks, which is the page's strongest cross-validation: two sources that never met agree on
both the register indices and the arithmetic.

**TVP DAC (M8A):** clock programming goes through the PLL address register plus a PLL data
register selected per clock — pixel $2D, memory $2E, loop $2F — with the MCLK/loop control $39
ORed with $38 in the loop-clock case; the FCode's `set_zeleaclockutil` writes four bytes per
clock: the P word with bit 7 cleared, then the N, M and P words from the mode table, and for the
pixel clock then polls the data register's bit $40 until the PLL locks [2]. The Linux driver
writes three bytes per PLL (M, N, P) to the same data registers [3]; the two witnesses disagree
on byte count and on which table word is "M" versus "N" (§5, §6.11). The card's system clock is
programmed once at init — the FCode writes SYSCLKM $4F [2], matching the driver's `SYSCLKM 0x4f`
init value [3].

**On the Mac side**, the mode records carried by the Mac OS driver hold per-mode `clock_div`,
`clock_mul` and `clock_shift` fields [7] — the driver computes PLL words at run time rather than
using a fixed table, which is how the third-party 1080p package could graft arbitrary modelines
onto the stock driver [7].

### 3.5 Interrupts

The register file contains no interrupt-enable or interrupt-status register anywhere the
witnesses touch — the closest thing is VIL, the *vertical interrupt line* register both witnesses
load with VSB's value at mode set [2] [3], and its name is all that is known about it. The Linux
driver is purely polled (SSTATUS busy bits) [3]. The Mac OS ndrv, however, imports and calls
`VSLNewInterruptService`, `VSLDoInterruptService` and `VSLDisposeInterruptService` [7] — the
Mac OS video-services interrupt registration path — so the shipped driver *does* expect an
interrupt from the card. What event sources it, and through which register it is acknowledged,
is unknown (§6.3); the platform's per-slot OR-combined interrupt collection is
[pci.md](../pci.md) §3.5's and [bandit.md](../../../machines/tnt/bandit.md) §3.9's.

## 4. Programming model

### 4.1 The expansion ROM container

Both ROM images are 64 KB with a single FCode image:

| Field | M8A (revision 3.8Ab2) | M2 |
|---|---|---|
| PCI signature | $55AA | $55AA |
| PCIR pointer | $20; vendor $10E0, device $9128, class $038000, code type $01, indicator $80 | $20; vendor $10E0, device $9128, class $030000, code type $01, indicator $80 |
| Image length | 61,440 bytes (120 × 512) | 55,296 bytes (108 × 512) |
| FCode header | at image offset $40: format $08, checksum $659B, length 61,318 bytes | format $08, checksum $C7AF, length 54,990 bytes |
| Strict detokenized walk | 170 words defined, consumed exactly, terminator reached | 152 words defined, consumed exactly, terminator reached |

Both images are therefore standard PCI expansion ROMs satisfying the Old World header contract
that [pci.md](../pci.md) §4.2 states and illustrates with these very ROMs — the platform page
records them as its non-Apple instance of the contract ("the header contract holds across
vendors", [pci.md](../pci.md) §4.2).

### 4.2 The FCode bring-up

The boot program is nearly identical in the two ROMs — the M8A's is the M2's plus the second DAC
generation plus two extra modes. Properties first, at FCode top level [2]: `name` ("IMS,tt128mb8A"
/ "IMS,tt128mb"), `model "tt128m"`, `device_type "display"`, `iso6429-1983-colors`, a `reg`
property built from three entries — configuration space (size 0), the 32 MB memory BAR ($10) and
the 64 KB expansion ROM BAR ($30) — and, at open time, `width`, `height`, `linebytes` (computed
from the selected mode) and `depth` as the literal 8, plus the `driver,AAPL,MacOS,PowerPC`
property carrying the PowerPC ndrv [2].

The `reg` construction is worth naming because it is the vendor's own PCI binding discipline: a
`>reg-spec` helper folds `my-space`/`my-address` with each BAR number and size — the IEEE 1275
address format of [pci.md](../pci.md) §2.7 — and the prefetchable declaration of §2.1 is the
literal `$42000010` constant it uses for the memory BAR [2].

The open sequence, decoded from both ROMs [2]:

1. **`getbase`** — read `assigned-addresses`, pick out the BAR-$10 entry (`phys-regs`) and the
   BAR-$30 entry (`rom-address`); then enable the card's memory decode by reading configuration
   dword +$04 (Command), ORing 2, and writing it back through `$call-parent "config-l@"` /
   `"config-l!"`; then `$call-parent "map-in"` the 32 MB region and remember it as `boardbase`.
   No I/O window is ever requested.
2. **`setup-cosmo…`** — add `boardbase` to every register's constant address (the chip block at
   boardbase+$800000, the DAC block at +$840000).
3. **`getconfig`** — read PRC and SSTATUS and derive `isversion2`, `ishalfsam`, `isedovram`,
   `is8column`, and on the M8A `is8meg` (§2.4).
4. **`setmincosmo`** — the minimal chip init, branching on what step 3 found:

   | Write | Condition |
   |---|---|
   | SRGCTL = $13 | silicon version 2 |
   | SRGCTL = $33 | otherwise |
   | SRGCTL \|= $400 | half-SAM memory |
   | SCR = $D0DC | always |
   | SCR \|= $1 | EDO VRAM |
   | STGCTL = $13B1 | always |

5. **`setminirazu` (M2) / `setminzelea` (M8A)** — the DAC init programs (§4.4).
6. **`monitor_attached`** — abort the open if no monitor (§3.3).
7. **`linearlut`** — write palette address 0, then a 256-entry identity ramp (each entry i written
   as i,i,i) [2]. (Both ROMs actually loop $256 = 598 iterations — §5.)
8. **`getmonitorsense` + `decode-mode` + `setup-entryptr`** — the sense program of §3.3, mapped
   to a mode-table entry pointer.
9. **`set-colors`** — load the standard 16-colour Open Firmware palette (std-16), with the
   monochrome-monitor path computing luminance as `(r·$4D + g·$97 + b·$1C) >> 8` — a 77/151/28
   weight sum, equal to $100 [2].
10. **`setdisplaymode`** — the full mode set. Common sequence [2]: `SCR = $80`; clear STGCTL bit
    1, SRGCTL bit 3 and RRCR bit 1 (quiesce); program the clocks (§3.4); write the eight scan
    timing registers from the mode entry; `HCIV = 1`, `VCIV = 1`, `TCDR = 4`; `VIL = VSB`; `SSR =
    0`; the HRIR/CMR pair by memory type ($100/$FF half-SAM, $200/$1FF otherwise); `SRGCTL = $03`
    (+$40 half-SAM); `RRCIV = 1`, `RRSC = $980`; `STGCTL = $1381`; `SPR` = width/4, halved on
    8 MB boards; `SCR` = $15098 (M8A) or $D098 (M2), OR the board bits; `RRCR = $11`; `GIOE = 0`;
    pixel mask $FF; and on the M8A the TVP true-colour ($80) and multiplex ($4D) controls.
11. **`erasescreen`** — a plain `fill` of the framebuffer through the linear aperture [2].
12. **`set_sog`** with flag 0 — sync-on-green off by default (§3.3).
13. **`setup-fb8`** — wire the Open Firmware fb8 display methods and publish `width`, `height`,
    `linebytes`, `depth 8` [2].

The close sequence is the mirror: `map-out` of the 32 MB region, then Command `&= ~2` through
the parent's config methods — the card turns its own memory decode back off [2]. The whole
bring-up therefore touches configuration space exactly three times (read Command, set bit 1,
clear bit 1) — the same enable-then-disable discipline as the mach64 card's I/O-enable dance
([mach64.md](mach64.md) §4.2), one bit and one space over.

### 4.3 The mode tables

Each ROM carries its mode list as `b(create)` data: 16-bit words per mode — width, height, the
eight scan-timing registers, then the pixel-clock words (byte offset $14 in the entry), and on
the M8A a further loop-clock quad at byte offset $1A [2].

**The M2 (IBM DAC) — five modes** [2]. The dot clock column is computed from the §3.4 formula
and lands on the standard rates:

| Mode | HES | HEB | HSB | HT | VES | VEB | VSB | VT | m / n / p / c | Dot clock |
|---|---|---|---|---|---|---|---|---|---|---|
| 640×480@60 (VGA) | $0C | $12 | $62 | $64 | $02 | $23 | $203 | $20D | $43 / $08 / $03 / $02 | 25.2 MHz |
| 640×480@67 | $08 | $12 | $62 | $6C | $03 | $2A | $20A | $20C | $78 / $13 / $02 / $02 | 30.25 MHz |
| 832×624@75 | $05 | $21 | $89 | $90 | $03 | $28 | $298 | $29B | $3E / $0A / $01 / $02 | 57.3 MHz |
| 1024×768@75 | $0A | $1C | $9C | $A6 | $03 | $20 | $320 | $323 | $07 / $00 / $01 / $02 | 80 MHz |
| 1152×870@75 | $12 | $22 | $B2 | $B6 | $03 | $31 | $397 | $39A | $3C / $0B / $00 / $02 | 101.7 MHz |

The Linux driver's IBM geometry — `hsb = heb + (xres >> 3)`, `ht = hsb + htp`, `vsb = veb +
yres`, `vt = vsb + vtp`, `ves = 3` — reproduces these rows from first principles (its 640×480 arm
is hes $08, heb $12, veb $2A, htp 10, MHz 30.25; its 832 arm is hes $05, heb $20, veb $28, MHz
57.27) [3]; the FCode's and the formula's values differ only in individual sync placements
(HSB $89 vs $88 on 832×624). That is the cross-validation of §1.4 made concrete.

**The M8A (TVP DAC) — seven modes** [2], adding the VGA-timing 800×600 and the 72 Hz 1024×768
that the 8 MB memory can feed:

| Mode | HES | HEB | HSB | HT | VES | VEB | VSB | VT | Pixel-clock words | Loop-clock words |
|---|---|---|---|---|---|---|---|---|---|---|
| 640×480@60 (VGA) | $06 | $09 | $31 | $32 | $02 | $23 | $203 | $20D | $E8 $15 $F3 $C1 | $C1 $3D $F3 $04 |
| 640×480@67 | $04 | $09 | $31 | $36 | $03 | $2A | $20A | $20C | $EF $2E $F2 $C1 | $C1 $3D $F3 $03 |
| 800×600@60 (VGA) | $08 | $0D | $3F | $42 | $04 | $1B | $273 | $274 | $FC $3A $F2 $C1 | $C1 $3D $F3 $02 |
| 832×624@75 | $03 | $11 | $45 | $48 | $03 | $28 | $298 | $29B | $FE $3E $F1 $C1 | $C1 $3D $F3 $01 |
| 1024×768@72 (VGA) | $06 | $10 | $50 | $51 | $06 | $23 | $323 | $326 | $F4 $30 $F1 $C1 | $C1 $3D $F3 $01 |
| 1024×768@75 | $05 | $0E | $4E | $53 | $03 | $20 | $320 | $323 | $FC $3A $F1 $C1 | $C1 $3D $F3 $01 |
| 1152×870@75 | $09 | $11 | $59 | $5B | $03 | $31 | $397 | $39A | $FD $3A $F1 $C1 | $C1 $3D $F3 $01 |

The clock words are tabulated verbatim because their internal naming is exactly where the two
witnesses disagree (§3.4, §6.11): the Linux driver's TVP mode tables pair $EF/$2E/$B2 with the
640×480@67 geometry [3], which matches the FCode's first three words except in the third ($F2 vs
$B2). The scan-timing values are in different units between the two ROMs' tables (the M2's 832×624
row has HSB $89 where the M8A's has $45 — roughly half) — consistent with a different
character-per-register convention per DAC path, but that is inference from two data points and the
registers' true units are §6.1.

What the tables show, beyond the numbers: the FCode's own mode list is a *boot* list — every mode
it can select by sense code — not the card's full mode list. The full list (800×600@72, 1280×960,
1600×1200, 1920×1080 and the rest) lives in the Mac OS driver [1] Ch. 3 Table 3.1 [4] [5] [7], and
Apple's cards expose mode/refresh combinations the retail table does not (§4.5).

### 4.4 The DAC init programs

The M2's `setminirazu` is the IBM DAC's init program [2]; the Linux driver's `ibm_initregs` [3] is
an independent record of the same program, written for the same silicon. Set side by side they
agree on every register they both touch except one:

| Indexed register | FCode (M2 ROM) | Linux driver | Agree |
|---|---|---|---|
| SYSCLKM ($16) | $4F | $4F | yes |
| PPMASK (direct) | $FF | $FF | yes |
| MISCTL1 ($70) | $01 | $01 | yes |
| MISCTL2 ($71) | $45 | $45 | yes |
| MISCTL3 ($72) | $00 | $00 | yes |
| CLKCTL ($02) | $21 | $21 | yes |
| DACOP ($06) | $0B | $02 | **no** |
| PIXFMT ($0A) | $03 (8 bpp: bpp>>3+2) | $03 for 8 bpp | yes |
| BPP8 ($0B) | $00 | $00 | yes |
| PIXCTL1/PIXCTL2 ($10/$11) | $05/$00 at clock set | $05/$00 | yes |
| PIXC0 ($23) | $02 | $02 | yes |

The DACOP disagreement ($0B vs $02) is a genuine divergence between the vendor's boot program
and the later driver's setting — one of the two selects a different DAC operating mode, and
nothing in the evidence set says which is right for what (§6.15). The M8A's `setminzelea`
programs the TVP instead: clock selection $05, MCLK/loop control $38, byte router $E4,
miscellaneous control $08 (OR) / $DF (AND-mask), general control $20, palette page $00 [2] —
again value-for-value the same program as the driver's `tvp_initregs` head [3].

### 4.5 The Mac OS driver stack

The Mac OS software, per the vendor manual [1] Ch. 2–3, is four pieces: the **Twin Turbo
Acceleration** extension, the **Twin Turbo Control Panel** (resolution, bit depth, Pan-Zoom,
general controls, colour management), the **Twin Turbo Control Strip** module, and an Upgrader.
Its documented behaviours include on-the-fly resolution switching with a colour-depth reduction
"if there is insufficient video memory", a zoom feature that steps resolutions down toward
640×480 with edge-driven panning, Caps-Lock as a hardware acceleration kill switch for
performance comparison, text caching, gamma selection in the Monitor Control Panel [1] Ch. 3,
and the mode-set confirmation dialog that reverts after five seconds [1] Ch. 3.

The driver that ships with the 9600 is the **"9600 Graphics Accelerator"** extension, driver
version string "4.06, IXMicro. 1996-1998" [7]. Its preserved record shows [7]:

- **Two embedded ndrv descriptions** — `IMS,tt128mbA` / "IMS TwinTurbo 128M4A" and
  `IMS,tt128mb8A` / "IMS TwinTurbo 128M8A" — i.e. one driver serving both Apple OEM cards, and
  the M4A's Open Firmware name recorded in the driver's own match list (`IMS,tt128mbA`, the M2
  ROM's `IMS,tt128mb` plus the A).
- **PEF import tables**: DriverServicesLib (interrupt functions, timing services, pool
  allocation), NameRegistryLib (the Registry property/entry calls plus
  `ExpMgrConfigReadWord`/`ReadLong`/`WriteWord` — the Mac OS route to configuration space),
  PCILib, VideoServicesLib (`VSLNewInterruptService`, `VSLDisposeInterruptService`,
  `VSLDoInterruptService`), and `TheDriverDescriptionDoDriverIO` [7].
- **A private Name Registry property**: `IMS,private-control` [7].
- **A QuickTime component**: "TwinTurbo MPEG Decompressor" / "TwinTurbo Decompressor", registered
  via `RegisterBottleRocketComponent` (the component's internal name is "BottleRocket", its
  loader "Akira") [7] — the shipped form of the manual's promised MPEG path [1] Introduction.
- **Six named gamma tables** — Mac Standard, Page-White, Mac Gray, Mac RGB, NTSC/PAL and Active
  Color LCD [7] — the same named set the card's FCode palette path implies.
- **The driver's mode records**, whose field names survive in the decompressed globals:
  `Display Mode ID`, `Width`, `Height`, `Refresh Rate`, a second mode/width/height/rate quad,
  then a timing block of `h_back_porch`, `h_active`, `h_front_porch`, `h_total`, `v_back_porch`,
  `v_active`, `v_front_porch`, `v_total`, `clock_div`, `clock_mul`, `clock_shift`, and a
  "confirm res switch" flag [7]. The third-party resolution package built on this driver reports
  the extension "based on the original '9600 Graphics Accelerator'" with only the globals
  decompressed, "the driver code is unchanged", and tested on Mac OS 7.6.1 and 9.2.2 [7] —
  which is also the page's only *observed* statement of the driver's working OS range.

Apple's cards expose different mode sets from the retail twins: the 1996 9600 card supports
8/16/24 bpp on small and medium monitors and 8/16 on large, from 512×384@60 through 1600×1200
[4] Table 1-3; the enhanced 9600's 8 MB card supports 8/16/24 on all sizes, adding 800×600@85
and 1024×768@85 among others [5] Table 1-3 — a mode list the FCode's seven-entry boot table does
not begin to cover, confirming that the OS driver's mode data is authoritative and independent.

### 4.6 The Linux driver as a second witness

The `imsttfb` driver's bring-up, as a cross-check on the FCode's [3]: match device $9128 → IBM
DAC, **unless** the Open Firmware node name is `IMS,tt128mb8` or `IMS,tt128mb8A`, in which case the
TVP register set is used (device $9135 is always TVP) — the DAC generation is selected by node
name because the PCI device ID does not distinguish them; request BAR0; map the framebuffer at
BAR+0 (4 MB for IBM cards, 8 MB for TVP), the chip registers at +$800000 and the DAC at +$840000,
4 KB each; read PRC to size the memory (bit 2 on IBM cards; 8 MB fixed on TVP); zero the
framebuffer; clear STGCTL bit 0; run the DAC init table; select the mode from NVRAM on PowerPC
(`NV_VMODE`/`NV_CMODE`, defaulting to 640×480@67 at 8 bpp — the same default mode the FCode's
sense decode falls back to) or 640×480@8bpp otherwise; then set colour depth (the TVP
true-colour/multiplex values of §2.3, the IBM BPP16 $01/$03 for 555/565 and PIXFMT bpp>>3+2),
the IBM pixel PLL, and the chip's scan timing and SCR byte-swap pair. Blanking is the STGCTL
$380 mask plus, on the IBM DAC, the full power-down program (CLKCTL $C0, PWRMNGMT $1F, SYNCCTL
$0F, MISCTL1 $11, MISCTL2 $55) and its reverse on unblank [3]. Panning writes SSR in 8-byte
units [3]. The driver's ioctl surface (register get/set, DAC register get/set, indexed register
get/set, each bounds-checked against the 4 KB blocks) is the complete documented register access
path from a non-Apple operating system [3] — the same hardware contract a PowerPC Linux or
[MkLinux](../../../os/mklinux.md) §1.4-era console drives, with no FCode or Mac OS driver involved.

## 5. Quirks & errata

- **No I/O space, ever.** The card decodes one 32 MB memory BAR and nothing else; the FCode never
  requests an I/O window and the only configuration-space traffic is the memory-decode bit
  (§4.2). A model that assumes a display card needs an I/O path (as the mach64's bootstrap does,
  [mach64.md](mach64.md) §2.2) breaks this card.
- **The 32 MB BAR is declared prefetchable** — `phys.hi` $42xxxxxx in the `reg` property [2] —
  while containing the control registers at +$800000/+$840000. The Apple mach64 card's BAR is
  non-prefetchable ([mach64.md](mach64.md) §4.1); the TwinTurbo claims more and deserves it less.
- **The two ROMs disagree about their own class code**: $030000 (display, VGA-compatible) in the
  M2's PCIR, $038000 (display, other) in the M8A's [2] — and the M2's "VGA-compatible" claim is
  false of the hardware (there is no VGA core).
- **The device ID does not identify the DAC.** Device $9128 ships with either the IBM or the TVP
  DAC, and the Linux driver selects the register set by the OF node name (`IMS,tt128mb8` /
  `IMS,tt128mb8A` → TVP) [3]; the FCode ROMs hard-wire their own generation [2]. A card model
  must key the DAC off the node name, not the ID.
- **SCR bits 9:8 must track the colour depth** — 8 bpp $000, 16 bpp $100, 24 bpp $200, 32 bpp
  $300 — or the framebuffer bytes scramble per pixel; the pair is the card's entire answer to
  the big-endian host (§3.1) [3].
- **The mode set quiesces in a specific order**: SCR $80, then bit 1 of STGCTL, bit 3 of SRGCTL
  and bit 1 of RRCR cleared *before* the clocks and timings are written, restored in the final
  register writes [2] [3] — a reprogram-during-scan discipline the sequencing of which matters
  on real monitors.
- **Both ROMs' `linearlut` loops $256 = 598 iterations** of a 256-entry-per-identity ramp — 342
  entries past the end of a 256-entry palette [2]. Whether the DAC's write address wraps at 256
  or the extra writes spill into adjacent RAM is not established; the boot palette is overwritten
  by `set-colors` and then the OS anyway, so the bug is invisible in operation (§6.10).
- **DACOP diverges between the two witnesses** — $0B in the vendor FCode, $02 in the Linux driver
  (§4.4) — the one register the two independent init programs do not agree on.
- **The two ROMs detect "no monitor" differently** — M2: GIO bit 2 with drivers off; M8A: a grey
  screen and the TVP sense-test register (§3.3) [2] — and both map the all-floating extended code
  $3F to a mode rather than treating it as absence.
- **The boot mode list is not the card's mode list.** The FCode tables carry five (M2) or seven
  (M8A) entries [2]; the retail and Apple driver mode lists are far larger (§4.5) — a model that
  implements only the FCode's modes will pass Open Firmware and fail Mac OS.
- **The engine is optional for the boot path** — the FCode never touches registers $00–$28 [2],
  so a partial engine model boots the card fine and only fails under an operating-system driver.
- **The hardware cursor exists on both DACs and is initialized off** by every witness
  (CURSCTL 0 [3]); no Mac OS driver path in the evidence set enables it.
- **`SPR` is width in 64-bit units and halves on 8 MB boards** (width/8 vs width/4) [2] — pitch
  is not a byte count, and the 8-column boards change the divisor again through SCR bit $04.

## 6. Open questions

1. **The scan-timing registers' units and semantics.** HES/HEB/HSB/HT and the vertical group are
   known only as opaque mode-table values; the M2 and M8A tables encode the same geometry on
   apparently different scales (§4.3), and what a "character" is per DAC path, what HCIV/VCIV/TCDR
   count, and what HRIR/CMR actually set are all unestablished — the witnesses write them, no
   document explains them.
2. **The draw engine's full contract.** BLTCTL's command encoding beyond the four observed values
   ($840, $40005, $05, $85), the role of S2SA and BI/MBC as masks versus compare planes, whether
   the engine can draw lines or monochrome-expand, and its completion semantics beyond SSTATUS
   bits 7/6 — the Mac OS ndrv's engine traffic is not decoded at register level, so its usage
   would be new evidence.
3. **Interrupts.** The shipped ndrv registers a VSL interrupt service [7], but no interrupt
   register exists in the observed file and the live configuration space (interrupt pin, line)
   is not recorded; what the card asserts, when, and how it is acknowledged is unknown.
4. **Live PCI configuration space.** Class code, revision ID, cache-line, latency, min-grant and
   max-latency as the card answers them at run time — only the ROM's PCIR declarations are on
   record (§2.1), and they disagree between revisions.
5. **Reset state of every register.** No datasheet, and both witnesses write before they read
   (§2.5); the power-on values a re-implementation must produce (as opposed to those the
   software overwrites) are unsettleable from this evidence.
6. **The base M and the M4A cards' ROMs.** No dump of either exists; whether the M4A's ROM is
   the M2's image, the M8A's, or its own revision, and what the plain `IMS,tt128mb` name on the
   M4A card would be, is open — only the driver's match name [7] hints at `IMS,tt128mbA`.
7. **The silicon-generation matrix.** `isversion2` (SSTATUS bits 11:8 == 2) selects SRGCTL $13
   over $33 [2] [3], but which physical silicon the version-1 population is, and which cards
   carry it, is unrecorded.
8. **The Number Nine connection.** The TT128 silicon is widely attributed to a relationship
   with Number Nine Computer's contemporary 128-bit boards, but nothing in the evidence set
   documents any such relationship, so this page makes no claim about it either way.
9. **Clone configurations.** Third-party PowerPC clones are recorded in the collecting record as
   TwinTurbo hosts, but no formal document names a specific machine; the manual's "PCI Power
   Macintosh or Compatible" [1] is as far as the evidence goes.
10. **The `linearlut` overrun** (§5): does the IBM/TVP palette write address wrap at 256, or do
    the extra 342 writes land in overscan/cursor colour RAM? Harmless in practice, unsettleable
    from the witnesses.
11. **The TVP PLL byte protocol.** The FCode writes four bytes per clock (P-with-bit-7-cleared,
    then N, M, P) and polls bit $40 for lock [2]; the Linux driver writes three (M, N, P) [3];
    the mode tables' word order differs correspondingly ($EF/$2E/$F2 vs m=0xEF, n=0x2E, p=0xB2).
    Which is the TVP3030's actual programming sequence — and whether one of the two is subtly
    wrong — needs a TI datasheet not present in the evidence set.
12. **Sync on green on the Apple cards.** The retail manual offers selectable sync-on-green [1],
    the FCode carries the TVP general-control switch [2], and Apple's notes say the 9600 cards
    "do not support sync on green" [4] [5] — unresolved whether that is board, driver or policy.
13. **The YUV-RGB / MPEG path's registers.** The hardware exists [1], the driver registers a
    QuickTime decompressor component [7], but no register path for it is documented anywhere;
    even its aperture location is unknown.
14. **GIO bit 2 on the M2** — polled as the monitor-present bit (§3.3); its electrical meaning
    (a presence pin on the connector, a comparator output, a strap) is not established.
15. **The IBM DAC's never-exercised registers** — PALETCTL, KEYCTL, the BPP24 path, the cursor
    block — exist in the driver's map with reset annotations [3] but no program on record uses
    them on this card; their semantics on the TwinTurbo specifically (as opposed to the 624
    family in general) are unverified.

## References

1. Integrated Micro Solutions, Inc., *Twin Turbo 128 for PCI Power Macintosh*, owner's manual,
   Revision 3.0, 1996 — Introduction ("About the Twin Turbo-128", "About Monitors and Resolution",
   "About True Color", "About Acceleration"); Chapter 1 "Installing the Hardware" (system
   requirements, unpacking, installation); Chapter 3 "Control Panels" (Table 3.1 "Display
   Resolution Chart", resolution and bit-depth selection, Pan-Zoom, general controls); Appendix A
   "Troubleshooting"; Appendix B "Specifications" (hardware specifications, VRAM DIMM
   compatibility Table A.1, Apple standard and Mini D-Sub video connector pinouts); Appendix C
   (FCC IDs MAJPCI9501, MAJPCI9521).
2. IMS/ixMicro TwinTurbo 128 expansion ROM dumps — the TwinTurbo 128M8A (ROM revision 3.8Ab2,
   64 KB EPROM image) and the TwinTurbo 128M2 (64 KB image): *observed* — the $55AA PCI
   expansion-ROM signature and PCIR structures (vendor $10E0, device $9128, class $038000 /
   $030000, code type $01, indicator $80, image lengths 61,440 / 55,296 bytes), and the
   byte-exact FCode detokenization of both programs (format $08, checksums $659B / $C7AF, declared
   lengths 61,318 / 54,990 bytes, 170 / 152 words defined, strict walks consuming exactly the
   declared programs): the published property list and `reg` construction, the `getbase`
   configuration-space sequence, the register-address constants and init programs, the mode
   tables, the monitor-sense program and sense-to-mode decode, the RAMDAC indexed writes, and
   the `driver,AAPL,MacOS,PowerPC` property with its PEF import tables.
3. The Linux kernel frame-buffer driver `drivers/video/fbdev/imsttfb.c` — the officially released
   open register reference for the chip: the copyright headers (derived from the powermac console
   "imstt" driver, Sigurdur Asgeirsson 1997, with Jeffrey Kuskin and Danilo Beuche; "some
   register values added by Damien Doligez, INRIA Rocquencourt"); the "TwinTurbo (Cosmo)"
   register enumeration; the IBM 624 and TI TVP3030 direct and indirect register maps with their
   reset annotations; the `ibm_initregs`/`tvp_initregs` init tables; the IBM dot-clock formula
   and `compute_imstt_regvals_*` mode geometry; the fill/copy engine programs and their SSTATUS
   busy-bit discipline; the probe's aperture mapping and DAC-class selection; the SCR
   byte-swap table (from MacsBug investigation on real hardware); the blank/pan/ioctl paths.
4. Apple Computer, Inc., *Developer Note: Power Macintosh 7300, 7600, 8600, and 9600 Computers*,
   1996 — Chapter 1 "Delta Guide to the New Models": §"New Features" (the new accelerated display
   card of the 9600), §"Accelerated Display Card" (4 MB non-expandable buffer, Table 1-3 display
   types and pixel depths, the sync-on-green note), Table 1-4 (configurations of the 9600/200 and
   9600/200MP).
5. Apple Computer, Inc., *Developer Note: Power Macintosh 8600 and 9600 Computers* (enhanced
   models), 1997 launch draft — Chapter 1: §"Accelerated Display Card" (the enhanced 9600's card:
   8 MB non-expandable, Table 1-3 display types and pixel depths, the sense-code and
   sync-on-green notes), Table 1-4 (configurations of the 9600/300 and 9600/350).
6. Apple Computer, Inc., press release announcing the Power Macintosh 9600/233, 9600/200 and
   9600/200MP, 17 February 1997 (archived French-language edition) — the 9600/233 and 9600/200
   "comportent une nouvelle carte vidéo IMS Twin Turbo 128 M4A".
7. The ixMicro TwinTurbo Mac OS driver record — the "9600 Graphics Accelerator" system extension
   (driver version string "4.06, IXMicro. 1996-1998") as preserved in the Mac OS system software
   for the Power Macintosh 9600 and in a third-party resolution-customization package built on
   it (extracted for this page): *observed* — the two embedded ndrv driver descriptions
   (`IMS,tt128mbA` / "IMS TwinTurbo 128M4A" and `IMS,tt128mb8A` / "IMS TwinTurbo 128M8A"), the
   PEF import tables (DriverServicesLib, NameRegistryLib, PCILib, VideoServicesLib with the VSL
   interrupt services, and `TheDriverDescriptionDoDriverIO`), the `IMS,private-control` Name
   Registry property, the QuickTime "TwinTurbo MPEG Decompressor" component and its
   "BottleRocket"/"Akira" registration path, the six named gamma tables, the driver's
   display-mode record field names in the decompressed globals, and the package's Read Me
   (the driver is the original extension with globals decompressed, "the driver code is
   unchanged"; tested under Mac OS 7.6.1 and Mac OS 9.2.2; the M8A card "often shipped with the
   PowerMac 9600").
