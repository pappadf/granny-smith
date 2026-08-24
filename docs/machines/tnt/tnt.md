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

Phases B-C (the current high-water) model the chipset skeleton plus the
DMA architecture:

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
  Z8530 core), BoxID (`+$1A000`), the banked NVRAM (`+$1D000` bank
  port / `+$1F000` data window on `$10` centres, 8 KB), and the eleven
  DBDMA channel windows (`+$8000+n*$100`).  Phase D adds AWACS
  (`+$14000`) and the RaDACal RAMDAC (`+$1B000`, control.c); SCSI, MACE
  and SWIM3 apertures log and read open bus until their phases land.
- **DBDMA** (`dbdma.c`) — the descriptor-based DMA engine, Phase C: one
  implementation, eleven channels (channel *n* raises Grand Central
  interrupt *n*), each a little-endian register file (`channelControl`
  mask/value writes, `channelStatus`, `commandPtrLo`, plus the three
  condition-select registers kept live) executing 16-byte little-endian
  descriptors fetched from guest physical memory against a per-channel
  injected device port `{out, in, s_bits}`.  Register truth is the
  shipping-silicon programming model Linux and OSF/NetBSD agree on
  (command nibble 31:28 with NOP=6/STOP=7; branches, waits and
  interrupts are the b/w/i modifier fields; status RUN/PAUSE/FLUSH/WAKE/
  DEAD/ACTIVE/BT at `$8000..$0100`) — Apple's pre-release 1993 `DBDMA.h`
  describes an earlier revision Grand Central does not implement.
  Design invariants, all unit-pinned (`tests/unit/suites/dbdma`): status
  transitions are synchronous with the control write (the canonical
  stop/reset poll loops terminate on the next read), descriptors are
  refetched rather than cached (the operation-word commit rule; STOP
  parks *on* its descriptor so the overwrite-and-WAKE ring idiom works),
  and `resCount`/`xferStatus` write-back is guest-visible *before* the
  channel interrupt.  A channel whose device port is not attached yet
  stalls its data commands honestly — except audio-out (channel 8),
  which carries an interim instant-consume sink until the Phase D AWACS
  datapath: Open Firmware plays its boot beep through channel 8 and
  polls for completion, so a stalling channel would hang the boot in
  firmware.
- **Cuda / VIA1** — the third instantiation of the shared behavioral Cuda
  (machines/av/cuda.c, firmware 2.37) on one real 6522 behind the island
  decode.  TNT-driven additions to the shared model: the polled no-TIP
  response termination the ROM's early-boot driver uses, the sync-cycle
  abort of an unread response (the OF-to-68k handoff leaves its last ADB
  response untaken), the response-abandonment watchdog, and (Phase D
  part 2) the re-presentation of a sync-aborted SOLICITED response once
  the sync completes — the abort resets the transport, not the firmware's
  output queue, and the TNT ROM's InitADB sends its ADB SendReset through
  the early polled driver then installs the interrupt driver with the
  reply still in flight.  The machine also feeds the 60.15 Hz reference
  into VIA1 CA1 each frame (the AMIC/AV line, third instance).
- **Control video** (`control.c`, Phase D part 2) — Control (343S1154) as
  PCI device 11 on the Chaos bus: Open Firmware sizes and assigns its two
  BARs through the Chaos config ports (`$14` = the 4 KB register block,
  `$18` = the 64 MB VRAM aperture, both landing in the `$90000000` VCI
  memory space, which this model now claims and dispatches — unclaimed
  addresses keep the recoverable-fault semantics).  32 little-endian
  registers on `$10` centres (the controlfb map), 4 MB VRAM as two 2 MB
  banks with the mode-dependent aperture views the sizing probe expects
  (bank 2 shows through at `+$600000`; the framebuffer lives in the
  `+$800000` half with pixel 0 at +16), the RaDACal byte cells at GC
  `+$1B000` (index/cursor/misc/CLUT on `$10` centres; misc `$20` bits 3:2
  = 8/16/32 bpp), monitor sense modeled as a 13"/14" strap (line C
  grounded — extended sense `$2B`, the head of the ROM's own mode table),
  and VBL as Grand Central interrupt 30 at 60 Hz while `intr_ena` is set.
  Scanout presents through the shared `display_t`; geometry derives from
  pitch/depth and the vertical-blank registers.  Not yet exercised
  end-to-end: the boot parks before the ROM's video driver loads (below).

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
the committed high-water rung (currently **T10**: 68k dispatching as an
identified Power Macintosh 7500, low memory live, the 60.15 Hz tick at
rate, the DBDMA engine playing Open Firmware's beep — its descriptors
live in ROM at `$FFE00090` — through the AWACS datapath against a
sample-exact golden WAV, and the interrupt fabric's mode-1 discipline
visible in the registers).  The run parks at the video wall (no Control
model; `ScrnBase` stays 0).  The interrupt-fabric and engine semantics
are additionally unit-pinned in `tests/unit/suites/tnt_gc` (the MkLinux
initialisation sequence and events-driven acknowledge, the NanoKernel's
`$80000000` mode-1 latch semantics, NVRAM banking, BoxID, island DBDMA
routing) and `tests/unit/suites/dbdma` (the engine in isolation).

### Machine identity (solved — both halves)

The shipping ROM identifies the machine TWICE, and neither half is the
community's "BoxID bits 11-12" reading:

- **The 68k routine** (`$FFC14844`): Hammerhead `+$00` first byte `$39`
  selects the TNT path (a `$3001xxxx` identifier is the 7200/Catalyst —
  the earlier reading that made every Phase-B boot a 7200), Hammerhead
  `+$20` bit 30 marks the 9500, and BoxID little-endian bit 11 marks
  the 8500.  All three profiles identify: `BoxFlag` `$3E`/`$3F`/`$3D` =
  gestalt 68/69/67.
- **Open Firmware's decode** (OpenFW image `$104db+`, solved in Phase D
  part 2 by resolving the image's own token dictionary): the model
  selector is `m = (HH+$20 byte0 >> 5) | ((HH+$20 byte0 >> 1) & 8)` —
  `$80` (bit 31) reads as the 7500/8500 class, `$40` (bit 30) as the
  9500 — and the 7500-vs-8500 split is BoxID LE **bit 13** (set =
  7500).  An unrecognised box gets `compatible "AAPL,????"` and a
  device tree with **no `chaos`/`control` display nodes** — which was
  the entire "OF never probes the display" wall.  With the identity
  right (rung T6), OF instantiates both nodes, probes the VCI bus and
  assigns Control's BARs; the 9500 correctly gets neither node (no
  onboard video on the real machine).

Known open items at this phase:

- **Rung T11 (the boot-liveness wall)**: the tree and the Control model
  are in place, but the 68k boot parks just after its first
  interrupt-unmask (`MOVE #$2000,SR` in the master init sequence at ROM
  `$FFC002DC`): the main thread is captured by a VIA-cascade interrupt
  whose 68k-side IPL never drops, and the ROM's video init
  (`_DisplayDispatch` at `$FFC00336`, the `mtej` ndrv match, ScrnBase)
  never runs.  See the Phase-D handover for the full diagnosis and the
  kernel/emulator interrupt-reflection questions it narrows to.
- The 68k startup chime does not play at the current wall (the OF beep
  is rung T10's instrument); revisit when the boot advances.
- The 7500's 601 RTC tick source keeps the PDM 7,833,600 Hz assumption
  until a ladder rung measures it (proposal §4.4).
- The `interruptableDeviceTable` / per-channel SCC interrupt split: the
  shared Z8530's single INT line currently fans to Grand Central
  interrupts 15 and 16 together (both 68k IPL 4); split with Phase F.
