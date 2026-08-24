# TNT family — Power Macintosh 7500/8500/9500

`src/machines/tnt/` implements the second-generation PCI Power Macintosh
platform ("TNT"): the repository's first PCI machine, its first
Open-Firmware machine, and its first machine with little-endian device
windows.  As on PDM, `config_t.cpu` is NULL — the classic 68k world
exists only as ROM bytes the PowerPC executes — but where PDM's HWInit
did the hardware bring-up, on TNT even that is guest code: the ROM's own
**Open Firmware 1.0.5** probes the chipset, sizes memory and builds the
device tree on our CPU core.

Models: `pm7500` (7500/100, 100 MHz 601, one Bandit), `pm8500` (8500/120,
120 MHz 604, two Bandits), `pm9500` (9500/132, 132 MHz 604, two Bandits);
all sharing the 1995-08 TNT ROM (stored checksums `96CD923D` v1 /
`9630C68B` v2, covering the image's 3 MB 68k half).  The 8500/9500 are
the first machines on the `CPU_MODEL_PPC604` core model
(docs/core/cpu/ppc.md, "The 604 model").

Sources: Apple, *Power Macintosh 7500 and 8500 Computers* Developer Note
(1995); Apple, *Power Macintosh 9500 Computer* Developer Note (1995);
Motorola, MPC604UM/AD; the shipping ROM itself (its Open Firmware device
tree, 68k DecoderInfo tables and ConfigInfo page are the primary
behavioral oracle), cross-checked against the OS driver corpus for these
exact machines (Linux powermac, NetBSD macppc, OSF/Apple MkLinux DR3).

## Board model

Phase B (the current high-water) models the chipset skeleton:

- **Hammerhead** (`hammerhead.c`) — the north bridge (system bus / DRAM /
  ROM / L2 controller) as a logged store-and-readback register file of
  128 x 32-bit registers on `$10` centres at `$F8000000`, big-endian (the
  one non-LE block).  Attested specials: the `$3001xxxx` part identifier
  POST gates on, the uniprocessor ArbConfig/WhoAmI/IntReg values, and the
  no-L2 presentation that makes POST skip its cache test.  The DRAM bank
  registers are unattested (proposal risk R1); every access is logged so
  the model can be fitted to what Open Firmware actually does.
- **Bandit x2 + Chaos** (`bandit.c`) — the AR-to-PCI host bridges
  (`$F2000000`, `$F4000000` on two-bridge boards) and the display-bus
  variant (`$F0000000`).  Config address/data ports at `+$800000` /
  `+$C00000` (little-endian; zero = idle; `data + (offset & 3)` carries
  the low offset bits), type-0 one-hot IDSEL with devices 0-10 and empty
  IDSELs reading all-ones, the bridge's own device-11 header (vendor
  `$106B`, device `$0001`/`$0003`, revision 3) with the `$48`
  address-select and `$50` mode-select (latching coherency bit).  Chaos
  config space is read-restricted and ignores writes.  The two 256 MB PCI
  memory spaces are claimed empty with recoverable-fault semantics (the
  BART pattern).
- **Grand Central** (`grand_central.c`) — the I/O controller: a 128 KB
  little-endian island at `$F3000000` (the base of Bandit 1's PCI I/O
  window).  Phase B populates the interrupt block (`+$20..$2C`), the
  VIA1/Cuda window (`+$16000`, 16 byte-wide registers on `$200` centres),
  both ESCC apertures (`+$12000` legacy / `+$13000` native, one shared
  Z8530 core), BoxID (`+$1A000`), and the banked NVRAM (`+$1D000` bank
  port / `+$1F000` data window on `$10` centres, 8 KB).  DBDMA, SCSI,
  MACE, AWACS, SWIM3 and RaDACal apertures log and read open bus until
  their phases land.
- **Cuda / VIA1** — the third instantiation of the shared behavioral Cuda
  (machines/av/cuda.c, firmware 2.37) on one real 6522 behind the island
  decode.  TNT-driven additions to the shared model: the polled no-TIP
  response termination the ROM's early-boot driver uses, the sync-cycle
  abort of an unread response (the OF-to-68k handoff leaves its last ADB
  response untaken), and the response-abandonment watchdog.

### The interrupt fabric

Four little-endian registers at `+$20` Events (edge-latched) / `+$24`
Mask / `+$28` Clear / `+$2C` Levels (live), with **two clear modes**
selected by Clear-write bit 31 (`ifMode1Clear`):

- **Mode 0** (power-on; the MkLinux scheme): the CPU line is
  combinational `(events | levels) & mask`; Events clear by explicit W1C.
- **Mode 1** (the NanoKernel's scheme): the line is an **output latch** —
  set by enabled source edges (or by unmasking a pending source), cleared
  by the `$80000000` acknowledge, re-asserted only by the next edge.  The
  kernel classifies from Levels & Mask and never clears Events, so a
  combinational line would storm forever after the first latched edge.

The mode-1 latch semantics are emulator-derived (pinned by the boot
reaching a correct 60.15 Hz tick rate); revisit if a later rung
contradicts them.

### Endianness

The memory map stays big-endian; each LE register block (interrupt block,
BoxID, the Bandit/Chaos config ports; later DBDMA/AWACS/Control) swaps at
its edge with `TNT_LE32` — the family's only sanctioned swap point.
Byte-wide cells on `$10`/`$200` centres need no swapping and are
byte-access only.

## Memory model

```
$00000000-RAM top   DRAM, contiguous from 0 (OF sizes it; the tree is
                    the contract afterwards)
$80000000-$8FFFFFFF Bandit 1 PCI memory — empty: recoverable fault
$90000000-$9FFFFFFF Chaos/VCI memory — likewise
$F0000000-$F1FFFFFF Chaos bridge + display-bus device space
$F2000000-$F2FFFFFF Bandit 1 (config ports at +$800000/+$C00000)
$F3000000-$F3FFFFFF Bandit 1 PCI I/O — Grand Central at the base
$F4000000-$F5FFFFFF Bandit 2 (8500/9500)
$F8000000           Hammerhead register window
$FFC00000-$FFFFFFFF the 4 MB ROM (reset vector $FFF00100)
```

## Boot shape and verification

Cold boot: NanoKernel reset entry (`$FFF00100`) consumes ConfigInfo and
builds the MMU; the exception-table code programs the Cuda pixel-clock
I2C triple; **Open Firmware** probes Hammerhead/Bandit/Grand Central,
sizes memory, writes its environment into a blank NVRAM (`boot`, `ttya`,
`/AAPL,ROM` — observed, settling the seeded-vs-self-initialised
question), and hands off; POST logs to NVRAM; the 68k emulator dispatches
the SuperMario ROM through low-memory init, timer calibration and the
live tick chain.

`tests/integration/tnt-rom-ladder` asserts the §7.1 ladder markers up to
the committed high-water rung (currently **T8**: 68k dispatching, low
memory live, the 60.15 Hz tick at rate).  The run parks at the Phase-D
video wall (no Control model; `ScrnBase` stays 0).

Known open items at this phase:

- The BoxID model-code pinning (rung T4): the 68k `BoxFlag` reads `$66`
  (the 7200 fallback) for every 2-bit code tried, so the ROM's model
  dispatch reads more of the register than bits 11-12; re-examined at the
  Phase-D About box.
- The 7500's 601 RTC tick source keeps the PDM 7,833,600 Hz assumption
  until a ladder rung measures it (proposal §4.4).
- The `interruptableDeviceTable` / per-channel SCC interrupt split: the
  shared Z8530's single INT line currently fans to Grand Central
  interrupts 15 and 16 together (both 68k IPL 4); split with Phase F.
