# 68K Macintosh Video Subsystems

This document catalogs the video hardware that shipped with — or as accessories for — the Motorola 68000-family Macintosh line. It covers both **NuBus expansion cards** designed by Apple and the **on-board video controller ASICs** that became standard from the Mac II era onward.

Apple terminology used throughout:

- **TFB / JMFB / DAFB / RBV / etc.** — internal *codenames* for the video hardware, almost always taken from the ASIC project name. These are the names by which Apple's own engineering documentation refers to each part, and they are how each driver is named.
- **sRsrc / sResource** — a slot-resource record in either NuBus card ROM or a pseudo-slot Declaration ROM. Each video device has a board sRsrc plus one functional sRsrc per supported mode/depth combination.
- **spID / mode** — the 16-bit identifier the Slot Manager uses to pick a depth or timing.

---

## 1. The Compact Macs (1984–1990): no programmable video

Before the Mac II, video was not "a subsystem" so much as a fixed scan-out of a hard-coded region of main memory:

| Model | Year | Logic | Output |
|---|---|---|---|
| Macintosh 128K / 512K / Plus | 1984–86 | IWM/PAL state machine reads main RAM | Built-in 9″ 512×342 1-bit |
| Macintosh SE | 1987 | "BBU" gate array | Built-in 9″ 512×342 1-bit |
| Macintosh SE/30 | 1989 | "GLUE" ASIC, same scan-out | Built-in 9″ 512×342 1-bit |
| Macintosh Classic / Classic II | 1990–91 | Same PAL/state-machine pattern | Built-in 9″ 512×342 1-bit |
| Macintosh Portable | 1989 | Custom LCD controller | Active-matrix 640×400 1-bit |

There is no programmable video device on these machines — the framebuffer is just a fixed range of main RAM scanned out at 60.15 Hz, and QuickDraw simply writes into it. Depth is fixed at 1 bit-per-pixel and there is no driver to load, no slot resource to enumerate, and no hardware CLUT.

These machines could only get color (or more pixels) by adding a NuBus card — which on the SE/30 meant the **PDS** (Processor-Direct Slot) plus a third-party adapter card.

---

## 2. NuBus Video Cards (Apple)

Apple shipped two NuBus video card families itself. Both follow the same model: a full-length NuBus card with its own framebuffer RAM, a CLUT/RAMDAC, and an Apple-defined declaration ROM holding the slot's driver and mode tables. The driver code is MC68020 assembler and integrates with the Mac OS Slot Manager exactly like any third-party slot device.

### 2.1 TFB — *Toby Frame Buffer* — Macintosh II Video Card (1987)

- **Codename:** Toby. Officially the **Macintosh II Video Card**, the first product on Apple's NuBus.
- **Bus:** NuBus, full-length card.
- **Capabilities:** 640×480 60 Hz, 1/2/4/8 bpp depending on installed VRAM (256 KB or 512 KB). Single DB-15 output for the AppleColor High-Resolution RGB Monitor (13″).
- **Notable hardware quirks:**
  - The original chip had a *byte-smearing* shortcut whereby a longword write to certain registers worked because the bus DTACK timing replicated the byte across all four lanes. This broke on 68040 machines and required driver-side fixes that issue real per-byte stores instead of the original longword writes.
  - The reset register is byte-wide despite living at a longword-aligned address — another piece of latent fragility that surfaced as bus widths changed.
  - VBL handling is folded entirely inside `WaitVSync` to lower interrupt overhead on slower 68020 machines.
- **Lineage:** Toby is the ancestor of every later Apple framebuffer. The driver structure (a Mac OS slot device driver with `Open` / `Control` / `Status` / `Prime` / `Close` plus VBL via slot interrupt) became the template that Elmer, RBV, V8, DAFB, and every later Apple framebuffer reuses essentially unchanged.

### 2.2 JMFB — *Jackson Memory Frame Buffer* / *Elmer* — Macintosh Display Card 4•8 / 8•24 (1990)

- **Codenames:** Jackson (the ASIC) / Elmer (the board).
- **Bus:** NuBus, full-length card.
- **Capabilities:**
  - **4•8 card:** 4 or 8 bpp at up to 640×480.
  - **8•24 card:** 1/2/4/8/16/24 bpp at resolutions up to 1152×870 (Apple Two-Page Monochrome, Portrait, 13″ RGB, 16″ RGB, etc.). 24 bpp requires the larger card.
  - Programmable CLUT (Brooktree-family RAMDAC); per-channel gamma tables of either 1 or 3 channels.
- **Lineage:** Elmer is the direct successor to Toby — same driver skeleton, but expanded mode tables and a more capable CLUT. The 8•24 card with a daughtercard becomes the **8•24 GC**, which adds an AMD Am29000 RISC for QuickDraw acceleration; that is a separate driver lineage.
- **Driver placement:** Elmer originally shipped with the driver in the card's own declaration ROM, then later versions of the system ROM carry an updated copy of the driver and substitute it for the on-card version. This is how Apple keeps fixing bugs in 1987-vintage NuBus cards in machines built years later.

### 2.3 Other Apple NuBus video cards

For completeness, Apple's full Apple-branded NuBus video card line included:

| Card | Year | ASIC | Notes |
|---|---|---|---|
| Macintosh II Video Card | 1987 | TFB / Toby | Driver per §2.1 |
| Macintosh Display Card 4•8 | 1990 | Jackson / JMFB | Driver per §2.2 |
| Macintosh Display Card 8•24 | 1990 | Jackson / JMFB | Driver per §2.2 |
| Macintosh Display Card 8•24 GC | 1990 | Jackson + AMD Am29000 RISC | Separate driver lineage (RISC-accelerated QuickDraw) |
| Macintosh 24AC Video Card | 1992 | (third-party OEM, Apple-branded) | Card carries its own declaration ROM |

Third-party NuBus cards (RasterOps, Radius, SuperMac, E-Machines, …) all carried their own driver in their card's declaration ROM; Apple's job was only to standardize the Slot Manager interface so any sufficiently conformant card would be recognized at boot and presented to the Window Manager as a screen.

---

## 3. On-Board Video Controllers (Apple, 68K era)

From the Mac II family forward, every "modular" Mac and every all-in-one with color got a dedicated video ASIC on the logic board. These controllers all live behind a *fake* NuBus slot (typically slot `$E` or slot `$0`) so the Slot Manager and standard video drivers can talk to them uniformly. Each has its own declaration-data payload defining the sResources for every supported (timing × depth) combination, and a driver that follows the Toby/Elmer skeleton.

### 3.1 RBV — *RAM-Based Video* — Mac IIci, IIsi

- **Codenames:** RBV (chip) / Erickson (the IIsi variant). An experimental SE/30-class variant existed but did not ship.
- **Used by:**
  - **Macintosh IIci** (1989) — the first machine to integrate video onto the logic board.
  - **Macintosh IIsi** (1990).
- **Why "RAM-Based":** unlike Toby/Elmer, the framebuffer is **carved out of main RAM** rather than living in dedicated VRAM. This costs the CPU memory bandwidth but saves a chip. On the IIci this means the first 320 KB of bank A is the framebuffer, and the system runs measurably slower with built-in video enabled (until a NuBus card takes over and the on-board video is disabled).
- **Capabilities:** 1/2/4/8 bpp at 640×480. Supports the Apple 12″ Mono/RGB, 13″ RGB, Portrait, and full-page monitors, with timing selected by the monitor's three sense lines (the *sense code*; later extended to multiscan-aware "AltSense" handshaking).
- **Notable weakness:** RBV has marginal interrupt latency, which created visible problems on the IIsi (16 MHz) until worked around by inserting a Time-Manager-driven "Faster Aurora" hack in the driver.

### 3.2 DAFB — *Direct Access Frame Buffer* — Quadra & early-AV family

- **Codenames per machine:**
  - **Quadra 700** — *Spike*
  - **Quadra 900 / 950** — *Eclipse*
  - **Quadra 800** — *Zydeco*
  - **Quadra 610 / Centris 610** — *Wombat*
  - **Quadra 650 / Centris 650** — *Wombat* (variant)
  - **Quadra 605 / LC 475** — *WLCD* ("Wombat-LCD")
- **Capabilities (varies by machine):** 1/2/4/8/16/24 bpp on resolutions from 512×384 up to 1152×870, with enough VRAM (typically 512 KB–2 MB on a SIMM). DAFB is the first Apple part to drive the **Apple Multiple Scan** displays (variable timings selected via extended sense codes).
- **CLUT / RAMDAC zoo:** because Apple second-sourced DACs aggressively, DAFB's driver carries paths for several different RAMDACs — Brooktree (Bt-series), AMD (AC824A / AC823), and AT&T/Sierra ("Antelope"). Each requires slightly different per-entry write timing (the Antelope, for example, needs roughly a 1 µs delay between the R, G, and B writes of each `SetEntries` triplet because it has no internal dual-ported CLUT RAM).
- **Lineage:** DAFB is the on-board successor to Elmer/JMFB — it brings 8•24-class capability to the logic board. It is itself succeeded on the AV class by Civic, and on the LC line by V8/Sonora.

### 3.3 V8 — *Elsie / V8* — LC family

- **Codenames:** V8 (chip) / Elsie (the project, derived from "L-C").
- **Used by:**
  - **Macintosh LC** (1990).
  - **Macintosh LC II** (1992) — same V8.
  - **Macintosh LC 930** family path also exists.
- **Capabilities:** 1/2/4/8/16 bpp at 512×384 (the LC's native "12-inch RGB"), with 8 bpp also on 640×480. VRAM is on a SIMM, populated to 256 KB or 512 KB.
- **Apple II video modes:** the LC was sold with the Apple IIe Card that fits the LC PDS; V8 contains hooks for that card's Apple II-emulator video formats. The same hooks also appear in the RBV driver (so the IIe Card can be used in IIsi / IIci as well).
- **Lineage:** V8 is a cost-reduced sibling of DAFB. It later begets Tim and Apollo as further reductions, and is succeeded on the LC III by Sonora.

### 3.4 Tim ("Jaws") — Classic II / fixed-display variants

- **Codenames:** Tim (the chip) / Jaws (the project). Earlier internal name was Waimea. The driver was forked from V8's.
- **Used by:** Tim drives the integrated grayscale video on Apple's lower-end "compact-but-cost-reduced" machines; it pairs with V8's family of integration parts. Its presence is gated as a *fixed device*, meaning the depth is not user-changeable.
- **Capabilities:** Fixed 1/2/4 bpp grayscale at the panel's native resolution. The driver opens and closes the Backlight driver in `VidOpen` / `VidClose`, indicating a portable / all-in-one LCD display path. The widely accepted mapping is that Tim drives the *Macintosh Classic II* internal display.
- **Lineage:** A direct trim of V8.

### 3.5 Apollo — small-machine variant

- **Codenames:** Apollo (chip) / VISA (an earlier name for the same family) / Eagle (a related ASIC family). The driver structure mirrors Tim's almost line-for-line.
- **Used by:** the Apollo logic-board family. Apollo is a fixed-device driver (same mechanism as Tim), opening the Backlight driver during `VidOpen`.
- **Capabilities:** Fixed depth, fixed resolution; 4-bit grayscale. The specific machine mapping for "Apollo" is not asserted by the driver itself; it is most commonly attributed to the *Macintosh Color Classic* video path.
- **Lineage:** Sibling of Tim, derived from V8.

### 3.6 GSC ("DBLite") — gray-scale PowerBooks

- **Chip:** **GSC** — Gray-Scale Controller. The driver is named *DBLite* after the project.
- **Used by:** PowerBook **160**, **180**, Duo **210**, Duo **230**.
- **Capabilities:** 4-bpp grayscale (16 levels) at the panel's fixed 640×400 resolution. The GSC's panel timings (skew register) must be re-programmed when bit-depth changes.
- **Power management:** on certain bring-up boards ("Niagra") the GSC could not deliver real VBL interrupts, so the driver uses a **Time Manager pseudo-VBL** instead. The driver opens and closes the Backlight driver alongside its own Open / Close.
- **Lineage:** A portable-targeted cousin of Tim — small framebuffer, fixed timings, backlight-aware.

### 3.7 CSC — *Color Screen Controller* — color PowerBooks

- **Chip:** **CSC** — Color Screen Controller.
- **Used by:** the first color portables — PowerBook **165c** (1993) and **180c** (1993). Later patched to support the closely-related "Malcolm" and "AJ" color-PowerBook variants in the same generation.
- **Capabilities:** 4 or 8 bpp on the fixed-size internal LCD; framebuffer in either 512 KB or 1 MB of VRAM.
- **Fixed-entry CLUT:** the CSC supports a *fixed-entry CLUT* mode in which some color-table entries are not user-programmable, so backlight/UI elements stay correct across mode and gamma changes.
- **VBL:** uses the same Time-Manager-task fallback as DBLite, at approximately 60.147 Hz.
- **Lineage:** Color-LCD descendant of GSC/DBLite.

### 3.8 Civic — Quadra 660AV / 840AV

- **Codenames:** *Cyclone* = **Quadra 840AV**; the **Quadra 660AV** is the "Tempest"-class sibling. The same chip family is referenced under bring-up names like *Puma* and *TV*.
- **Capabilities:** Civic is the first 68K-Mac framebuffer that integrates **video-in / video-out**:
  - 1/2/4/8/16 bpp at standard Mac timings up to 832×624 / 1024×768.
  - **Composite & S-Video out** (NTSC and PAL), with on-chip **convolution** to anti-flicker text on TV outputs.
  - **Video overlay / video-in**, programmable on a per-mode basis.
  - **VDC** (Video-in Data Converter / chroma key).
  - **RGB ↔ composite mode switching** at runtime, including the "boot on TV" scenario where the user can hot-switch back to an RGB monitor.
- **Lineage:** Civic is DAFB's successor on the AV class, with the AV-specific composite/overlay engine bolted on. It pairs with the **Sebastian** AV companion chip on the audio side.

### 3.9 Sonora — LC III, Performa, early Power Macintosh bring-up

- **Used by:**
  - **Macintosh LC III / LC III+** (1993).
  - **Macintosh LC 520 / 550 / 575**, Performa 4xx / 5xx.
  - The same chip family also handles the early-engineering bring-up of **PDM** — the first PowerPC machines (Power Macintosh 6100/7100/8100, 1994).
- **Capabilities:** 1/2/4/8/16 bpp at multiple Apple Multiple Scan timings up to 832×624. Drives the *Rubik-512* and *Rubik-560* timing variants and can switch between them on the fly. Includes a **SetSync** / **GetSync** call ("Dark Star Support") for energy-saving (DPMS-style) monitors.
- **Sound integration:** Sonora is more than a video chip — it also contains the DFAC sound-init register hooks. Sonora is the LC III's combined video-and-sound ASIC.
- **Lineage:** Sonora is V8's direct successor — same logic-board class, much-expanded mode list and CLUT, plus integrated audio.

### 3.10 ATI Mach64 / Diamond — early PowerMac PCI transition

- **Used by:** This is **not a 68K target.** It is the bring-up software for the ATI Mach64-class chip and a Weitek-9000-based "Diamond" board on Apple's first **PCI Power Macintosh** machines (7500/8500/9500 generation, 1995). It is mentioned here only because the same Slot Manager / declaration-ROM scaffolding is reused on those machines, so the boundary between "68K Mac video" and "early PowerMac video" is blurry at the software layer even though the bus is different (PCI rather than NuBus).
- For 68K Macintosh video purposes, treat this as out of scope.

---

## 4. Cross-Reference: Machine ↔ Video

The following table summarizes which video device drives each 68K-era Macintosh. Where a machine has two listed devices, the first is the on-board controller and the second is what a NuBus card displaces it with.

| Machine | Year | On-board device | NuBus / PDS expansion |
|---|---|---|---|
| Macintosh II | 1987 | (none — no on-board video) | TFB (Macintosh II Video Card) |
| Macintosh IIx | 1988 | (none) | TFB / JMFB |
| Macintosh IIcx | 1989 | (none) | TFB / JMFB |
| Macintosh SE/30 | 1989 | Built-in 1-bit 9″ scan-out | NuBus-via-PDS third-party cards |
| Macintosh IIci | 1989 | **RBV** | JMFB (8•24) |
| Macintosh IIfx | 1990 | (none) | JMFB / 8•24 GC |
| Macintosh IIsi | 1990 | **RBV** ("Erickson") | JMFB |
| Macintosh LC | 1990 | **V8** ("Elsie") | LC-PDS cards |
| Macintosh Classic II | 1991 | **Tim** ("Jaws") | — |
| Macintosh Quadra 700 | 1991 | **DAFB** ("Spike") | JMFB / 8•24 GC |
| Macintosh Quadra 900 / 950 | 1991/92 | **DAFB** ("Eclipse") | JMFB / 8•24 GC |
| Macintosh LC II | 1992 | **V8** | LC-PDS |
| Macintosh LC III / LC III+ | 1993 | **Sonora** | LC-PDS |
| Macintosh LC 475 / Quadra 605 | 1993 | **DAFB** ("WLCD") | LC-PDS |
| Macintosh LC 520 / 550 / 575 | 1993 | **Sonora** | LC-PDS |
| Macintosh Centris/Quadra 610 | 1993 | **DAFB** ("Wombat") | NuBus adapter |
| Macintosh Centris/Quadra 650 | 1993 | **DAFB** ("Wombat") | NuBus |
| Macintosh Quadra 800 | 1993 | **DAFB** ("Zydeco") | NuBus |
| Macintosh Quadra 660AV | 1993 | **Civic** ("Tempest") | NuBus |
| Macintosh Quadra 840AV | 1993 | **Civic** ("Cyclone") | NuBus |
| PowerBook 160 / 180 | 1992 | **GSC / DBLite** | — |
| PowerBook Duo 210 / 230 | 1992 | **GSC / DBLite** | (Duo Dock external) |
| PowerBook 165c | 1993 | **CSC** | — |
| PowerBook 180c | 1993 | **CSC** | — |
| Macintosh Color Classic / Performa 250 | 1993 | (V8-class — *Apollo*-related) | — |
| Macintosh Classic II / Performa 200 | 1991 | **Tim** | — |

(Some "Performa" model numbers are rebadges and share their on-board video with their Macintosh-branded twin.)

---

## 5. Architectural Themes

Several patterns hold across the entire 68K-era video lineup, from the 1987 Toby card to the 1993 Civic AV chip:

1. **Single driver skeleton.** Every Apple video controller — TFB through Sonora — exposes the same Mac OS slot device driver entry points (`Open` / `Prime` / `Control` / `Status` / `Close`), the same VBL routing through the Slot Manager, and the same use of `dCtlStorage` in the AuxDCE for its private state. New parts are integrated by writing a new sResource payload, not a new driver architecture.

2. **Codenames stay forever.** Once a chip lands as "DAFB" or "RBV" in Apple engineering, every later machine that reuses the silicon picks up another codename for the *same* part (Spike, Eclipse, Zydeco, Wombat, …). The driver accumulates `BoxFlag` checks that branch on machine identity rather than being forked into separate drivers.

3. **Sense-line monitor detection.** All Apple framebuffers from RBV onward decode a 3-bit sense code from the DB-15 connector to identify the attached monitor. When multi-scan displays arrived, this scheme was extended ("AltSense") with a second handshake on the same three pins, rather than adding new connector pins.

4. **CLUT/RAMDAC variability is in software.** Apple second-sourced CLUT/DACs aggressively (Brooktree, AMD, AT&T/Sierra "Antelope"), and rather than mask the differences in hardware, the driver branches at runtime — different per-DAC `SetEntries` paths and different per-DAC gamma-write timings.

5. **Time-Manager VBL fallback.** When the chip can't deliver a real VBL interrupt (early portable bring-ups, PDM, "Niagra"), the same trick recurs: install a Time Manager task at ~60.15 Hz and call the VBL handler from there. DBLite, CSC, and bring-up paths in Sonora all do this.

6. **System-ROM patching of card drivers.** Cards that originally shipped with their own declaration ROM (TFB, JMFB) get re-formatted as in-system-ROM drivers in the universal ROM build. The card's local ROM is bypassed and the system supplies the driver. This is the mechanism by which Apple keeps fixing bugs in 1987-vintage NuBus cards in machines built years later.

These patterns — a stable driver template, codename accretion across silicon revisions, sense-line monitor detection, runtime DAC selection, and Time-Manager-based VBL fallback — are the architectural backbone of 68K Macintosh video, and they survive essentially unchanged into the early PowerPC era.
