# ATI Rage 128 GL

The ATI Rage 128 GL as a retail Macintosh PCI display card — the Rage
Orion, Xclaim VR 128 and Nexus 128 — modelled in
[`src/core/peripherals/pci/cards/rage128.c`](../../../../../../src/core/peripherals/pci/cards/rage128.c).
The chip's hardware reference is
[`rage-128.md`](../../../../../reference/hardware/pci/cards/rage-128.md);
this page covers the emulator's model, what drove each choice, and what is
not modelled yet. Like the Mach64 GX, the card is driven by its **own**
firmware: Open Firmware runs the FCode in its expansion ROM to build the
node, and Mac OS loads the `.Display_Rage128` ndrv that ROM publishes. The
model answers registers and nothing else.

| | |
|---|---|
| **Card kind** | `rage128` (`card_class = "display"`, `requires_prom`) |
| **PCI ID** | `1002:5245` ("RE", Rage 128 GL PCI), class `$030000`, revision `$00` (the first silicon; the production A22 value is unknown and every guest read of it is logged) |
| **ROM** | the Xclaim VR 128's `113-57406-108` (FCode 1.69, node `ATY,Rage128v`; catalogue default) or the Nexus 128's `113-57502-103` (`ATY,Rage128n`); identities in [`expansion-rom.md`](../../../../../reference/hardware/pci/expansion-rom.md) |
| **BARs** | BAR0 64 MB prefetchable memory, BAR1 256 B I/O, BAR2 16 KB memory, ROM 128 KB |
| **Memory** | 16 MB (Rage Orion, Xclaim VR 128); `memory=32m` for the Nexus 128 |
| **Monitor** | `monitor=vga` (default: a VGA monitor answering DDC), `vga_noddc`, `13in_rgb`, `21in_rgb` |

## 1. Responsibilities & design

**Status.** Milestones 4b and 4c of the Rage 128 work: the PCI face, the
four apertures and their byte-order swappers, the register file and its
config mirror, the PLL file, monitor sense and DDC, the palette, the CRTC
turned into a display descriptor at 8, 15/16 and 32 bpp, the hardware
cursor, and the VBLANK interrupt — enough for the card's ndrv to run
System 7.6 at every depth it offers. The 2D draw engine,
the Concurrent Command Engine and the 3D engine are later milestones: their
registers are plain storage here, and `GUI_STAT`, `PC_GUI_CTLSTAT` and
`PC_NGUI_CTLSTAT` report idle.

**Driven by the FCode's needs.** The model was written against a decode of
the Xclaim FCode's probe and `open`, and three facts from that decode shaped
it:

- **Probe goes through the I/O BAR.** Before Open Firmware maps BAR0 and
  BAR2, the FCode maps BAR1 and reaches the *whole* register file through
  `MM_INDEX`/`MM_DATA` at I/O `$0`/`$4` — the clocks, the memory controller,
  sense, DDC and the mode set all happen there. The card's `reg` property
  does not list BAR1, but Open Firmware 1.0.5 and 2.4 both size and assign
  it, so it is a real BAR (not a strapped decode, as the Mach64's I/O was).
- **The sense path is chosen by a VSYNC loopback.** The FCode drives
  `CRTC_V_SYNC_STRT_WID` bit 23 (vertical-sync polarity) and watches
  `GPIO_MONID` pad 3 follow it. A VGA cable does not loop it back, so the
  FCode reads DDC and falls back to sense code `$717` (VGA, 640 × 480 at
  60 Hz); an Apple cable does, and only then are the three Apple sense
  lines read. The `monitor=` option picks the cable.
- **`get-msecs` must advance.** The probe calibrates a busy loop and then
  waits about a second for DDC plus 32 ms steps for each clock; on the G3
  that is long enough that keystrokes typed during it are lost (the test
  waits for the probe's end, below).

## 2. Key types & files

- `rage128_t` — the card: the 2048-dword register file (`reg[]`,
  little-endian values as the chip holds them), the 64-entry PLL file, the
  AGP command and power-state latches, VRAM, the palette and its two
  auto-incrementing indices, the monitor, the EDID block and the DDC slave,
  and the scanout state.
- `r128_monitor_t` — a cable: whether it loops VSYNC back (Apple sense),
  whether an EDID EEPROM answers on it, and its sense straps in the Mach64
  card's three-step extended model.
- `r128_ddc_t` — the bit-banged I²C slave.
- `scripts/rage128/rage128_regs.py` — the register name ↔ offset table the
  constants here are checked against.

## 3. Behaviour/algorithms

**Apertures.** One register file, four ways in:

| Face | Bytes | Order |
|---|---|---|
| BAR2 + 0 | register aperture 0, `$0000`–`$1FFF` | little-endian |
| BAR2 + 8 KB | register aperture 1 | little-endian, or host order with `CONFIG_CNTL.APER_REG_ENDIAN` |
| BAR1 | `$00`–`$FF` (the non-GUI block, incl. `MM_INDEX`/`MM_DATA`) | little-endian |
| `MM_INDEX`/`MM_DATA` | anything; `MM_INDEX` bit 31 (`MM_APER`) selects VRAM | little-endian |

The manual names one swap bit for "the register apertures" without saying
which; it is applied to aperture 1 only, the copy the manual says exists for
the Power Macintosh, so a little-endian driver on aperture 0 is never
disturbed. Byte and halfword accesses reach the lanes they address ("add 1,
2 or 3 to the address", RRG Table 2-2), carried into the register write as a
lane mask so side-effect registers know which lanes moved.

BAR0 is two 32 MB linear apertures over the same VRAM. Each has its own
swapper (`APER_0_ENDIAN`, `APER_1_ENDIAN`): 16 bpp swapping XORs a CPU byte
address with 1, 32 bpp swapping with 3, so a big-endian host's pixel store
lands as the little-endian pixel the CRTC scans. Beyond the memory size the
aperture is empty (reads `$FF`, writes dropped). The `CONFIG_APER_*` and
`CONFIG_REG_*` registers are computed from the assigned BARs: the low bit of
the aperture-1 and register-aperture-1 base fields is hardwired to one, so
the second copies sit at BAR0 + 32 MB and BAR2 + 8 KB.

**Configuration space.** The generic header plus: `AD_STEPPING` hardwired in
the command register, `MIN_GNT` 8, `INTERRUPT_LINE` resetting to `$FF`, and
the capability list at `$50` (AGP 1.0, then power management at `$5C`,
which terminates it). Registers `$F00`–`$FFF` are a read-only mirror of the
whole of it.

**PLL.** `CLOCK_CNTL_INDEX` bits 5:0 select a PLL register and bit 7
(`PLL_WR_EN`) gates writes; `CLOCK_CNTL_DATA`'s lanes reach the register's
bytes. The PPLL atomic-update bit 15 of `PPLL_REF_DIV` and `PPLL_DIV_0..3`
clears at once — the FCode polls it, for up to half a second, after every
divider write. No clock is derived from the dividers yet; the raster runs at
the host frame rate.

**Monitor sense and DDC.** `GPIO_MONID` is four open-drain pads against a
pull-up: a pad reads low when the card drives it (`EN=1, A=0`), when the
monitor's ties pull it, or (pad 1, SDA) when the DDC slave holds it. Pads
0/2/1 are Apple's SENSE0/1/2 (crosswise, because pads 2 and 1 are also DDC
SCL and SDA); pad 3 is the VSYNC loopback. The DDC slave is level-driven: it
detects START/STOP from SDA moving while SCL is high, samples on SCL's rising
edge, changes SDA only while SCL is low, and serves a synthesised EDID 1.3
for a 640 × 480 VGA monitor at `$A0`/`$A1`. `DAC_CNTL.DAC_CMP_OUTPUT`
reports a load whenever a monitor is attached and the comparators are
enabled.

**Scanout.** `CRTC_H_TOTAL_DISP` bits 23:16 + 1 characters wide,
`CRTC_V_TOTAL_DISP` bits 26:16 + 1 lines, `CRTC_PITCH` × 8 pixels per line,
`CRTC_OFFSET` in **bytes** (the manual says 64-bit words; the FCode writes
`$8000` and publishes BAR0 + `$8000` as its frame buffer). The raster is on
when `CRTC_EN`, `CRTC_EXT_DISP_EN`, display requests enabled and
`CRTC_EXT_CNTL.CRTC_DISPLAY_DIS` clear. 8 bpp scans VRAM in place through the
palette (`DAC_MASK` applied); 15, 16 and 32 bpp are byte-swapped into a
compose buffer each frame, since the display layer takes big-endian pixels.

**Hardware cursor.** A 64 × 64 map at `CUR_OFFSET`, each line eight bytes
of AND bits then eight of XOR bits, leftmost pixel in bit 7 (SDK §4.4):
AND/XOR `00` colour 0, `01` colour 1, `10` transparent, `11` the complement
of the pixel beneath. `CUR_HORZ_VERT_OFF` says where in the map the visible
part starts; `CUR_HORZ_VERT_POSN` (vertical in 10:0, horizontal in 26:16)
places it. The colours are always 24-bit RGB; at 8 bpp the composite takes
the nearest palette entry. The cursor is an overlay into `compose`, never
written to VRAM; with it on, even 8 bpp is presented through `compose`.

The card's ndrv keeps the System's arrow in this cursor. It moves only on
real pointing-device motion: Mac OS feeds `DrawHardwareCursor` from the
Cursor Device Manager, so the harness's low-memory mouse placement
(`machine.adb.mouse.move … "global"`, which writes `MTemp`/`RawMouse`/
`Mouse`) moves hit-testing but not the arrow on screen. `"hw"` mode is real
ADB motion and moves both.

**Interrupts.** `GEN_INT_STATUS` latches VBLANK and VSYNC every frame
whether or not they are enabled (write 1 to clear); `GEN_INT_CNTL` gates
the INTA line, which is level and held until acknowledged. `CRTC_STATUS`
bit 0 is the live blank, bit 1 the since-last-cleared latch.

## 4. Object-model / shell surface

`machine.pci.slot[N].card` carries `framebuffer` (the shared display node,
nominated as `machine.screen.source`), `monitor` (`id`, `ddc`,
`apple_sense`) and, under the advanced category, `regs` (`vram_size`,
`crtc_gen_cntl`, `config_cntl`, `read(offset)` without side effects,
`pll(index)`).

## 5. Checkpointing

The register file, PLL file, AGP/power latches, palette and indices, the
DDC slave's state and VRAM. The ROM travels in the slot's checkpoint part.
A checkpoint from a card of another memory size fails the size-tagged read
loudly.

## 6. Testing

- `tests/integration/rage128-prom` — the ROM catalogue (milestone 4a).
- `tests/integration/suite-tnt`, row `pm9500-76-rage128` — System 7.6 from
  the MESH disk to the Finder on the card (chime, mount, goldens), then
  256 colours and millions from the Control Strip, each a golden.
- `tests/integration/tnt-pci-rage128` — the config header before any
  instruction; the node Open Firmware 1.0.5 builds from the FCode, read
  back with `.properties` over the serial console, for each cable and for
  the Nexus ROM; the same node under the beige G3's Open Firmware 2.4; the
  ndrv driving the grey desktop in thousands of colours; the apertures,
  swappers and the index pair.

## 7. Known debts

- No pixel clock: refresh is the host's, and PLL dividers are stored only.
- `CRTC_OFFSET_CNTL` flip latching, `CRTC_VLINE` interrupts and packed
  24 bpp are not modelled.
- The 2D engine, CCE and 3D engine (milestones 4d–4g).
- The revision byte is `$00` until a real card is read.

## 8. See also

- [`rage-128.md`](../../../../../reference/hardware/pci/cards/rage-128.md) — the chip.
- [`pci.md`](../../pci.md) — the bus, config header, bus mastering.
- [`expansion-rom.md`](../../../../../reference/hardware/pci/expansion-rom.md) — PROM identity and the catalogue.
