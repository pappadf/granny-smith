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
  variant (`$F0000000`).  This file is the family's **PCI bridge
  adapter**: it owns the chipset facts and delegates every config cycle
  to the generic bus controller in
  [core/peripherals/pci](../../core/peripherals/pci.md).  Config
  address/data ports at `+$800000` / `+$C00000` (little-endian; zero =
  idle; `data + (offset & 3)` carries the low offset bits), type-0
  one-hot IDSEL, and the bridge's own device-11 header (vendor `$106B`,
  device `$0001`, revision 3) registered as an ordinary device with
  `$48` address-select and `$50` mode-select (latching coherency bit) as
  its two quirk registers.  Devices 0-10 and empty IDSELs read all-ones
  because nothing is registered there — not because of a literal.  Chaos
  config space is read-restricted (`$00-$0F`, `$14`, `$18`) and ignores
  writes outside its two BAR offsets; both quirks are applied in the
  adapter, around the generic dispatch, because they are facts about
  Chaos rather than about the device behind it.  The two 256 MB PCI
  memory spaces are handed to their buses as decode **windows**: an
  access there reaches whichever seated device's BAR decodes it, and
  takes a recoverable transfer error when none does (the BART pattern).

  Each Bandit also forwards an **8 MB PCI I/O window at its own base**
  (`$F2000000`, `$F4000000`), driving only the low 16 address bits, so
  the 64 KB I/O space aliases through it 128 times.  The bridge's own
  `ranges` property, dumped from a real 9500 under Open Firmware (Apple
  TN1062), is the specification:

  ```
  01000000 00000000 00000000  F2000000  00000000 00800000   I/O,  8 MB
  02000000 00000000 F3000000  F3000000  00000000 01000000   mem, 16 MB
  ```

  So the two 16 MB windows above a Bandit are **not** interchangeable:
  the bridge base carries PCI I/O, and the next 16 MB is pass-through
  MEMORY — which is how Grand Central is reached.  (Earlier text here and
  in `tnt.h` had this the other way round.)  Chaos claims no I/O window
  at all.

  One window is still deliberately NOT claimed, and the `$48` register
  says so (an OS derives the bridge's ranges from it, so the register
  must describe what the model actually decodes):

  - **Bandit 2 has no memory-space window.**  TN1062 pins it at
    `$90000000`, 256 MB — but our `pm9500` still carries Chaos (the
    documented no-onboard-video deviation) whose VCI window is at that
    same address.  On real hardware they never collide: a 9500 has no
    Chaos, a 7500/8500 has no Bandit 2.  The claim lands with the Chaos
    removal.  The 9500's Bandit-2 sockets still probe correctly meanwhile:
    discovery is config cycles, and an unpopulated IDSEL reads all-ones.
- **Grand Central** (`grand_central.c`) — the I/O controller: a 128 KB
  little-endian island at `$F3000000` (Bandit 1's pass-through MEMORY
  window — *not* its I/O window, which is at the bridge base).  Phase B populates the interrupt block (`+$20..$2C`), the
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
  PCI device 11 on the Chaos bus — a registered card kind
  (`tnt_control`, BUILTIN attach) that the machine's slot table names, so
  the runtime device and the configuration view come from one
  declaration.  Open Firmware sizes and assigns its two
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
  and VBL as Grand Central interrupt **26** at 60 Hz while `intr_ena` is
  set (the dossier's map guessed 30; the shipping video driver toggles
  mask bit 26 as it writes `INTR_ENA`, and Apple's own 9500
  external-interrupt table gives 30 to the second-CPU doorbell).
  Scanout presents through the shared `display_t`; geometry derives from
  the blank-pair timing registers (width = (hsblank − heblank) × 2,
  height = (vsblank − veblank) / 2) with the pitch as the scan-line
  stride only — the boot's own 640x480 32 bpp mode programs pitch 2592 =
  640×4 + a 32-byte pan margin, so pitch-derived width paints junk.
  The VBL is bit 2 of INTR_ENA/INTR_STAT (the ROM's ndrv enables `$4`
  then `$C` and spin-polls status bit 2 for the retrace during its
  mode-set).  Exercised end-to-end since rung T11: the ROM's own `mtej`
  control driver mode-sets the chip and QuickDraw paints the gray
  desktop into the BAR-assigned VRAM aperture (`ScrnBase $90800210`).

### The interrupt fabric

Four little-endian registers at `+$20` Events (edge-latched) / `+$24`
Mask / `+$28` Clear / `+$2C` Levels (live), with **two clear modes**
selected by Clear-write bit 31 (`ifMode1Clear`):

- **Mode 0** (power-on; the MkLinux scheme): the CPU line is
  combinational `(events | levels) & mask`; Events clear by explicit W1C.
- **Mode 1** (the NanoKernel's scheme): the line is an **output latch**,
  and the scheme is **interrupt-on-CHANGE** (the AMIC INTMODE=1 law) —
  set by any change of an enabled level source (assertion OR
  deassertion), by an enabled event edge, or by unmasking a pending
  source; cleared by the `$80000000` acknowledge; re-asserted only by
  the next change.  The kernel classifies from Levels & Mask and never
  clears Events, so a combinational line would storm forever after the
  first latched edge.  The DEASSERT half is load-bearing: it is the only
  mechanism in the whole kernel/emulator contract that lowers the
  emulated 68k IPL — `ExtIntHandlerTNT` re-reads quiet Levels and stores
  IPL 0 through `EmuIntLevelPtr`; the 68k emulator's delivery path never
  touches the stored IplValue and never calls `PrioritizeInterrupts`
  (the TNT handler stores the value without the `$8000` reprioritize
  flag), so without deassert changes the emulator redelivers the stale
  level forever and the 68k base context starves at its first unmask —
  which was exactly the Phase-D2 boot wall.

The mode-1 latch semantics are emulator-derived (pinned by the boot
reaching a correct 60.15 Hz tick rate, and by the T11 desktop requiring
the deassert-change law); revisit if a later rung contradicts them.

### PCI slot topology

Each model declares a `pci_slot_decl_t` table (`pm7500.c` and friends)
that the profile encoder and `pci_init` share, so `machine.profile`'s
`pci_slots` block and the runtime cannot drift.  Sockets are PCI devices
**13/14/15** on their bridge's bus — the ROM's own `slot-names` bitmask
(`$0000E000`) on the bandit node, corroborated by Apple's Network Server
developer note IDSEL table — and each socket's strapped INTA-D line
(`/INTA`-`/INTD` are tied together per slot on these machines, so there
is no swizzle and a multi-function card collapses onto one line) reaches
a Grand Central external:

| Model | Sockets | Devices | GC interrupts | Builtin |
|---|---|---|---|---|
| 7500 / 8500 | A1 B1 C1 (Bandit 1) | 13 14 15 | 23 24 25 | VCI: Control @ dev 11 |
| 9500 | A1 B1 C1 (Bandit 1) + D2 E2 F2 (Bandit 2) | 13 14 15 each bus | 23 24 25 / 27 28 29 | VCI: Control @ dev 11 |

The numbers are Apple's own 9500 external-interrupt table (External *N* =
GC interrupt 20 + *N*).  They also settle the old 26-vs-30 puzzle: on a
two-Bandit 9500, 26 is Bandit 2's error line and 30 is the second-CPU
doorbell, so on a one-Bandit TNT both are free — which is why Control's
VBL could take 26.  The slot lines are level-sensitive, which the
interrupt block's mode-1 change semantics already handle, deassert edge
included; `tnt_pci_slot_irq` is the whole delivery path.

Every socket ships empty.  A populated socket also sets its **BoxID
presence pin** (`+$1A000` bits 0-5, bit *n*-1 for slot *n*) — the pins
are live population, not a board constant, so an empty socket reads 0
exactly as the hardware does.

Which cards fit a socket is not declared here: it is computed from the
card registry (`pci_card_fits_socket`).  Control is a registered kind
with BUILTIN attachment, so it can never be offered on a socket, and the
9500's `VCI` builtin entry carries forward the documented no-onboard-video
deviation until a real PCI display card retires it.

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
$F2000000-$F2FFFFFF Bandit 1: PCI I/O (8 MB) + config ports at +$800000/+$C00000
$F3000000-$F3FFFFFF Bandit 1 pass-through memory — Grand Central at the base
$F4000000-$F5FFFFFF Bandit 2 (8500/9500): PCI I/O + ports, then pass-through
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
the committed high-water rung (currently **T11**: 68k dispatching as an
identified Power Macintosh 7500, low memory live, the 60.15 Hz tick at
rate, the DBDMA engine playing Open Firmware's beep — its descriptors
live in ROM at `$FFE00090` — through the AWACS datapath against a
sample-exact golden WAV, the interrupt fabric's mode-1 discipline
visible in the registers, and the ROM's native control driver painting
the 640x480 gray desktop into the Control VRAM aperture against a
screen golden).  The run parks hunting for boot media (Phase E's
frontier; no SCSI/floppy data paths yet).  The interrupt-fabric and engine semantics
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

- The 68k startup chime STILL does not play by the T11 desktop (the OF
  beep is rung T10's instrument); the boot reaches its media hunt
  chime-less — an open question for Phase E.
- The 7500's 601 RTC tick source keeps the PDM 7,833,600 Hz assumption
  until a ladder rung measures it (proposal §4.4).
- The `interruptableDeviceTable` / per-channel SCC interrupt split: the
  shared Z8530's single INT line currently fans to Grand Central
  interrupts 15 and 16 together (both 68k IPL 4); split with Phase F.
