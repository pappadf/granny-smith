# PDM family — Power Macintosh 6100/7100/8100

`src/machines/pdm/` implements the first-generation Power Macintosh
platform ("PDM"): the first machines whose main CPU is the PowerPC 601
(`src/core/cpu/ppc/`, docs/core/cpu/ppc.md).  `config_t.cpu` is NULL on
these machines — the classic 68k world (Toolbox, drivers, the 68LC040
emulator) exists only as bytes in the 4 MB ROM that the 601 executes.

Models: `pm6100` (6100/60, 60 MHz), `pm7100` (7100/66, 66 MHz), `pm8100`
(8100/80, 80 MHz); all 2:1 bus, all sharing the 1994-03 ROM (stored
checksum `9FEB69B3`, covering the image's 3 MB 68k half — the trailing
megabyte holds the PPC exception tables, HWInit, the nanokernel, and the
68k-emulator code/dispatch tables).

Sources: Apple, *Power Macintosh Computers* Developer Note (1994); the
Power Macintosh 8100 schematic set; the MPC601 User's Manual; the shipping
ROM itself, whose hardware-init sequence is the primary behavioral oracle.

## Board model

Two Apple ASICs around Tier-1 silicon:

- **HMC** (`hmc.c`) — memory controller: one 35-bit bit-serial config
  register, the RAM bank windows with probe/alias/relocation semantics,
  the machine-ID register at `$5FFFFFFC`, and the wait-state timing model
  HWInit's bus-ratio measurement depends on.  See `hmc.md`.
- **AMIC** (`amic.c`) — the I/O controller: all `$50Fxxxxx` decode, a
  pseudo-VIA1 (a real 6522 core instance at stride `$200`), the
  pseudo-VIA2/RBV-style slot+device interrupt bank, the top-level
  interrupt control register driving the 601's single level-sensitive INT
  line, the DMA-engine register file, and the VBL raster.  See `amic.md`.

Behind AMIC's decode, two datapaths have their own modules:

- **AWACS + sound engine** (`awacs.c`) — the codec command port and the
  double-buffered output DMA engine, rendering into the shared host audio
  stream.  See `awacs.md`.
- **Onboard video** (`ariel.c`) — the Sonora-model control registers, the
  Ariel II CLUT/DAC, and the scanout of the framebuffer that lives at
  physical DRAM 0.  See `video.md`.
- **SWIM III + the floppy** (`swim3.c`, `swim3_xfer.c`) — the third-
  generation Sony controller at `$50F16000`, the Sony drive-register
  sense/strobe protocol, and the sector transfer engine behind the AMIC
  floppy DMA channel: MFM and GCR reads and writes, whole-track format,
  and raw capture, against the one internal manual-inject SuperDrive every
  model in the family has.  See `swim3.md`.
- **BART** (`bart.c`) — the NuBus '90 bridge on the 7100 and 8100: the
  `$F0000000` register file, the slot and super-slot windows for the three
  connectors `$B`/`$C`/`$D`, and the recoverable faults an empty slot
  answers a Slot Manager probe with.  The 6100's bridge ships on an
  optional PDS adapter that is not modeled, so that machine presents no
  bridge at all and the ROM's probe finds that out.  See `bart.md`.

Reused models: the AV family's behavioral **Cuda** (`av/cuda.c`, the same
341S0788 firmware 2.37 part) on the pseudo-VIA1 shift-register transport;
the Tier-1 **6522** core; the shared **NuBus** bus controller and card
drivers (`core/peripherals/nubus/`) behind BART; the shared **floppy**
module (`core/peripherals/floppy.c`) for the drive and its media.  MACE
arrives in a later phase (proposal-powerpc-601-pdm.md §6).

## Memory map

| Range | Contents |
|---|---|
| `$00000000-$3FFFFFFF` | DRAM bank windows (HMC-owned; layout per model + config) |
| `$40000000-$4FFFFFFF` | the 4 MB ROM repeating every 4 MB (HWInit self-rebases to the `$40300000` alias; the OS view is `$40800000`) |
| `$50F00000-$50F4FFFF` | AMIC-decoded I/O island (+ the HMC serial port at `$50F40000`) |
| `$5FFFF000` page | machine-ID register (`$5FFFFFFC`) |
| `$90000000-$EFFFFFFF` | NuBus super slot space, 256 MB per slot (BART; 7100/8100) |
| `$F0000000` page | BART register file (7100/8100); a read faults on a 6100, which is how the ROM learns it has no bridge |
| `$FB000000-$FEFFFFFF` | NuBus standard slot space `$B`/`$C`/`$D` + the PDS `$E` window, 16 MB each (BART) |
| everything else in `$60000000-$FEFFFFFF` | decoded by nobody — reads `$FF` (AMIC's error here is the unrecoverable 40 µs kind, which no ROM path exercises) |
| `$FF000000-$FFFFFFFF` | ROM alias (reset vector fetch at `$FFF00100`) |

An address a BART window claims but no seated card answers takes a
*recoverable* transfer error: the 601 seam delivers a machine check, and
the nanokernel reflects it into the 68k emulator as an ordinary bus error
— which is exactly what the Slot Manager's declaration-ROM probe catches
to record an empty slot.  See `bart.md`.

## Timing model

CPI is 1.0 (with 601 branch folding in the core), which makes HWInit's
DEC-timed measurement loops land exactly: measured CPU clock snaps to the
profile frequency and the bus-ratio measurement computes 2 — the values
the ROM stores into NKProcessorInfo and uses to pick the Gestalt box
class.  The 60.15 Hz tick drives VIA1 CA1; VIA timers run on the
exact-rational 783.36 kHz scaling (`via_set_exact_clock`), so
guest-measured timer rates are exactly φ2-equivalent.

## Boot ladder

The family is developed ladder-first against the shipping ROM
(proposal-powerpc-601-pdm.md §6.1).  `tests/integration/pdm-rom-ladder`
boots the ROM headless and asserts every marker up to the committed
high-water rung — currently **L20**: HWInit end-to-end, kernel entry, the
nanokernel's HTAB/translation bring-up, the 68k emulator dispatching the
Start Manager through Cuda/PRAM init and video init, the boot chime
matched sample-exactly against a golden WAV, and the gray desktop matched
as a screen golden — parking at the Phase-G SCSI-scan wall.

Past that wall, `tests/integration/suite-pdm` boots System 7.5 from SCSI to
the Finder desktop on all three models, and covers the NuBus bridge: a
Macintosh Display Card 24AC in slot `$C` of an 8100 (enumerated, driven, and
carried as a second screen), the empty-socket fault contract, and a
save/restore round trip with a card seated.
