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

Reused models: the AV family's behavioral **Cuda** (`av/cuda.c`, the same
341S0788 firmware 2.37 part) on the pseudo-VIA1 shift-register transport;
the Tier-1 **6522** core.  SCSI (53C94/53CF96), SWIM3, MACE, ESCC deltas,
and BART/NuBus arrive in later phases (proposal-powerpc-601-pdm.md §6).

## Memory map

| Range | Contents |
|---|---|
| `$00000000-$3FFFFFFF` | DRAM bank windows (HMC-owned; layout per model + config) |
| `$40000000-$4FFFFFFF` | the 4 MB ROM repeating every 4 MB (HWInit self-rebases to the `$40300000` alias; the OS view is `$40800000`) |
| `$50F00000-$50F4FFFF` | AMIC-decoded I/O island (+ the HMC serial port at `$50F40000`) |
| `$5FFFF000` page | machine-ID register (`$5FFFFFFC`) |
| `$60000000-$FEFFFFFF` | undecoded — bus error, delivered as a 601 machine check |
| `$FF000000-$FFFFFFFF` | ROM alias (reset vector fetch at `$FFF00100`) |

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
