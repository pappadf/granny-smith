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

  `$90000000` has two claimants, and which one gets it is decided by what
  actually seated rather than by machine name.  TN1062 pins 256 MB there
  for **Bandit 2**; Chaos's VCI window is at the same address.  On real
  hardware they never collide — a 9500 has no Chaos, a 7500/8500 has no
  Bandit 2 — but our `pm9500` carries both, because Chaos is the stand-in
  host for the onboard video the real machine does not have, and that
  stand-in only materialises when no socket supplied a display card
  (`PCI_SLOT_BUILTIN_FALLBACK`).

  So `tnt_bandit_claim_memory()` runs AFTER `pci_seat_slots()`: if the VCI
  bus has a device, Chaos keeps the window it needs to reach that device's
  apertures; if it is empty, Bandit 2 takes it.  The `$48` address-select
  register follows by construction — a bridge advertises the range only if
  it claimed it — because an OS derives the bridge's ranges from `$48` and
  the register must describe what the model actually decodes.  When Chaos
  leaves `pm9500` for good the condition collapses to "Bandit 2 always
  claims it" with no other change.
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

---

# The Apple Network Servers — a fourth board on this family

`src/machines/tnt/ans500.c` / `ans700.c` add two machines to `tnt` that are
**not Macintoshes**: the Apple Network Server 500/132 and 700/150
("Shiner"), Apple's only non-Macintosh computers, which boot Apple's
production **Open Firmware 1.1.22** ROM (`$962F6C13`) and run **AIX 4.1.5
for Apple Network Servers**. They carry no Mac OS Toolbox at all.

That the family name now covers two non-Macintoshes is the honest cost of
the right decision. The reasoning is Apple's own: the ANS's Grand Central
address is *"defined by the current OpenFirmware and Expansion Manager code
for the TNT ROM releases"*; its ROM is built from the 9500 v2 codebase and
carries the same version string; its entire internal I/O subtree is the
9500's. Apple's developer note opens by declining to re-document any of it —

> *"This specification is unfortunately not one-stop shopping, owing to the
> architectural origins of the Network Servers in the PowerMac 9500 family.
> Therefore much of the hardware detail which is fully documented in the
> PowerMac family is not repeated here. Instead, unique hardware interfaces
> are described."*

— so a separate `src/machines/shiner/` would either duplicate
`hammerhead.c` / `bandit.c` / `grand_central.c` / `dbdma.c` or export them
across a family boundary the hardware does not have.
`tnt_board_desc_t.kind` is the seam instead, exactly as it already
distinguishes a 7500 from a 9500.

Sources: Apple, *Network Server Hardware Developer Notes* (1996); Apple,
*Network Server Developer's Reference Guide* (1996), ch. 6 and 7; Apple,
*Network Server Theory of Operations*; the shipping ROM itself — which on
this machine is an unusually good oracle, because Open Firmware 1.1.22 has
**named Forth words** and `see <word>` decompiles them at the prompt.

## What differs from a 9500

Apple enumerates it, so this table is scope rather than survey. Four items
are boot-critical.

| Delta | Where |
|---|---|
| Three Grand Central external interrupt lines re-purposed; both Bandits ganged onto `Error_Int` | `tnt.h` `ANS_INT_*` |
| Slots 1-2 on Bandit 1, slots 3-6 on Bandit 2 — **six devices on Bandit 1**, no P2P bridge | the profiles' slot tables |
| Two 53C825A fast/wide SCSI controllers at IDSEL 17/18 | `pci/cards/sym53c825.c` |
| **MESH removed** | `tnt_board_desc_t.has_mesh` |
| Cirrus 54M30 PCI video at IDSEL 15, no interrupt | `pci/cards/cirrus54m30.c` |
| The GBUS island: front-panel LCD, keyswitch, board registers, Ethernet PROM | `gbus.c`, `lcd.c` |
| Parity DRAM, 8 DIMM slots, a 512 MB **ROM decode** ceiling | the profiles |
| An L2 cache DIMM that is actually present | `hammerhead.c` `+$E0` |

`ANS_INT_*` carries the one trap worth repeating: **a slot's interrupt line
does not follow its bridge.** Slot 3 sits on Bandit 2 but keeps EXT5 — the
line a 9500 gives Bandit 1's third slot — so a model that derives the line
from the bus is wrong for exactly one slot and right for the other five,
which is the worst failure shape available. The map is data in the profile.

## The GBUS island

Grand Central's Generic Bus provides *"six chip selects and write enable
which the Network Server uses for devices such as NVRAM, Ethernet PROM,
board registers, and the LCD."* On a Macintosh those chip selects are
mostly idle; here they are the server. See `gbus.h` for the address map and
the bit tables.

Three properties, all of them silent when wrong:

* **Every bit is ACTIVE LOW** unless its name ends in `H`. A healthy
  machine reads all-ones, not zero.
* **Board Register 2 is POLLED and never interrupts** — Apple is explicit —
  so a static healthy value satisfies both the ROM and AIX with no event
  plumbing at all. Its eight environmental bits are exposed as *writable*
  object attributes (`machine.board.temp_warn`, …) because injection is the
  only way to exercise the path, and POST prints a published string for
  each one.
* **There are two keyswitches.** The rear one gates power and the sliding
  logic-board drawer and is a power-on precondition; the front
  three-position one is what software reads, in Board Register 1 bits 13/14.
  Both default to LOCKED (`machine.board.keyswitch`, `.rear_key_locked`).

## The front-panel LCD, and why it is built first

`lcd.c` models a write-only HD44780-class panel behind two registers on
GBUS device 3. It is built before anything else because the *Theory of
Operations* puts it before memory — *"It is the job of POST to initialize
the hardware into a working state and establish a software path to the
LCD"* — so it is the machine's only output device during exactly the phase
most likely to break. Apple published the strings POST writes, so
`machine.lcd.text` turns POST into a self-describing test harness, asserted
both positively (the expected progress message appeared) and negatively (no
`MainLBU 825#1 Failed`, no `MainLBU Video Failed`).

Two facts the ROM settled that the documents left open:

* **The panel is 4 lines by 20 columns.** Apple documents only the two
  register addresses; its sample banner line is 23 characters and cannot be
  what the machine writes. The ROM's line-select commands are `$80` / `$C0`
  / `$94` / `$D4` — the canonical 4x20 DDRAM map, where `$94 - $80 = 20` is
  exactly where line 0 ends — and every POST string it writes is padded to
  exactly 20 characters.
* **The controller is an HD44780.** Its initialisation is the textbook
  power-on ritual (`$30 $30 $30 $38 $08 $0C $06 $38 $01`) and nothing else.

A healthy 700/150 settles on:

```
  ROM vers.1.1.22
0048 MB Parity RAM
075MHz 604, 50MHzBus
1024KB Level 2 Cache
```

The CPU figure is `075` rather than `150` because POST measures the clock
by timing an instruction loop against the timebase, and the family models
the 604 at CPI 2 — so the panel reports exactly half the profile's clock.
That is a readout of a deliberate modelling choice (see `tnt_init`), not a
defect, and the ladder asserts what the ROM actually prints so that changing
it would show up as a deliberate change.

## The L2 cache — and the size encoding, decoded

These are the first machines in the repository to report an L2 cache, so
they are the first to run the ROM's L2 test at all: every Macintosh TNT
board answers Hammerhead `+$E0` with `$00`, which makes POST skip it.

`hammerhead.c` had carried "bit `$80` = present, low 3 bits a size code
(encoding unattested)" since the TNT work. The Network Server ROM prints
the size it decoded on the LCD, so sweeping the register and reading line 3
gives the answer outright: `$80` → 512 KB, `$81` → 256 KB, `$82` → 1 MB,
`$83` → 4 MB, with bit 2 ignored. The size lives in bits 1:0, and 512 KB —
not 256 — is code zero.

The strapped bits also have to **survive a write**: the register is
config/status and the ROM drops `$70` into it mid-test and reads it
straight back, so plain store-and-readback would erase the cache the machine
has and the report would come out `0000KB`.

## SCSI — three buses, and a new device class

The machine has **three** SCSI buses: two fast/wide 53C825A channels and the
narrow 53C94 external chain the Macintosh boards already had. The 53C8xx is
a genuinely new device class for this repository — it executes an
instruction set out of host memory rather than being register-driven — and
it has its own document: **docs/core/peripherals/scripts53c8xx.md**, which
also records the two board facts the ROM gave up (`GPIO0` is a presence
strap; the chip is strapped **little**-endian, not big).

The two channels are separate namespaces in the object model:
`machine.scsi` is channel 0 (bays 0-3, Open Firmware's `disk0`..`disk3`)
and `machine.scsi2` is channel 1 (bays 4-6, plus the 700's two rear
drives). This is the first machine here where a SCSI id does not identify a
device on its own, and `scsi_init_named` exists for it.

## Where Open Firmware puts a built-in device's BARs

Each Bandit forwards 16 MB of PCI memory one-to-one at its base + 16 MB —
its own `ranges` property says so — and Grand Central decodes the 128 KB at
the bottom of Bandit 1's. On a Macintosh nothing else lands there. On a
Network Server it is where **every** built-in device's BARs land: Open
Firmware assigns the two 53C825As `$F3100000`/`$F3101000` and
`$F3103000`/`$F3104000`, and Apple's own worked device-tree node shows a
slot-6 card at `$F5100000` inside Bandit 2's. Without the window the
firmware prints `Can't clear C825 interrupt!` and stops.

`tnt_bandit_claim_memory` claims it for `TNT_BOARD_SHINER` only. It is
arguably a gap on the Macintosh boards too, but turning 16 MB of
previously-quiet address space into recoverable transfer errors on a
boot-critical bridge is not a change to make without a Macintosh ROM ladder
run to prove it.

## The console is the screen

Open Firmware's own `(install-console)` opens whatever `input-device` and
`output-device` name and falls back to `ttya` only when one of them fails:

```forth
"input-device"  evaluate catch if 2drop ttya then  ['] input  catch …
"output-device" evaluate catch if 2drop ttya then  ['] output catch …
or if ttya io then
```

A Network Server ships with `input-device kbd` / `output-device screen`,
and both its 54M30 and its ADB keyboard work, so **out of the box the
console is the monitor** — the firmware's progress narration (`msg-write`)
goes to ttya regardless, which is why the boot chatter appears on the
serial port even when the console does not.

Apple documents the alternative and it is `setenv`. The integration rows do
exactly that, once, on the machine's own keyboard, and cold-boot; the Grand
Central NVRAM part is non-volatile and the setting sticks for the same
reason it does on the real machine. `tests/integration/lib/ans.script`
wraps it.

The 54M30 also answers the **legacy** VGA I/O block (`$3B0`-`$3DF`) rather
than its relocatable BAR, because this board installs no pull-down on MD51
and the Alpine's "Enable Offset" bit therefore reads zero. Open Firmware
still sizes and assigns the BAR — it lands at `$00010000`, above the 16 bits
a Bandit even drives — while every real access goes to the fixed addresses.
Without the fixed decode the firmware's write to `$3C4` takes a recoverable
transfer error and the machine check takes down the rest of device
installation with it.

## The framebuffer — a mode that is derived, not configured

Nothing tells the emulator what resolution to present. The part is a VGA, so
the mode lives in the CRTC, the sequencer and the Cirrus extension registers;
`cirrus54m30.c` reads one out of them. What Open Firmware 1.1.22 programs on
this machine, captured by logging every port write across a cold boot:

```
SR01 = $01   8 dots per character clock
SR07 = $F1   Cirrus extended mode, bits [3:1] = 000 = 8 bpp
CR01 = $4F   horizontal display end 79   -> (79 + 1) * 8 = 640 pixels
CR12 = $DF   vertical display end 223, with CR07 bit 1 as VDE bit 8
                                          -> 479 + 1  = 480 lines
CR13 = $50   offset 80, in eight-byte units in a 256-colour mode
                                          -> 640 bytes per scan line
GR05 = $40   256-colour shift mode
```

and 768 writes to the DAC data port — 256 entries of six-bit R, G, B, which
the model expands to eight bits by replicating the top two into the bottom
so `$3F` maps to `$FF` exactly.

8 bpp is not a shortcut. Apple: the part "implements only a little-endian
window into the packed-pixel frame buffer, hence Big Endian operating
systems are limited to 8 bits per pixel" — and at one byte per pixel byte
order does not matter, so the existing `PIXEL_8BPP` path is *correct* rather
than merely convenient. Deeper colour needs a little-endian framebuffer
window in `display_t`, which this repository does not have.

One VGA register is deliberately not store-and-readback: **Input Status
Register 1** (`$3BA`/`$3DA`). Software does not read it for a value, it
reads it for an *edge* — every VGA console waits on the vertical-retrace or
display-enable bit before touching the CRTC or the palette — so it is
derived from the scheduler's clock, which keeps a run deterministic and
keeps a waiting loop from becoming a hang.

## Verification

| Row | Tier | What it holds |
|---|---|---|
| `ans-rom-ladder` | unit | POST reaching the LCD, sizing memory, the CPU/bus and L2 banners, both keyswitch defaults, the active-low environmental register, and the absence of every published failure string whose device we model — on both profiles |
| `ans-pci-slots` | unit | six sockets and three builtins, IDSELs, the rewired interrupt map, the raw config-cycle identities including the `$14` Revision ID that gates machine identity |
| `ans-device-tree` | matrix | `dev / ls`, node properties and device aliases against Apple's published Listing 6-1, driven over the serial console |
| `ans-scsi` | matrix | the SCRIPTS engine, through Open Firmware's own `probe-scsi1` and `probe-scsi2`, on both fast/wide channels |
| `ans-console` | matrix | the machine booted as it SHIPPED — console on the monitor — with the derived 640x480x8 mode and a golden of what Open Firmware draws |
| `ans-macos-2rom` | matrix | the 2.0 prototype ROM booting Mac OS to the desktop on the same hardware model — two unrelated software stacks, one model |
| `ans-aix-boot` | extended, fixture-gated | the documented Service-keyswitch install path, up to `bootapple` launching off the AIX 4.1.5 Install CD |

Note the probe words: this machine has `probe-scsi0`, `probe-scsi1` and
`probe-scsi2`, one per controller. `probe-scsi` and `probe-scsi-all` do not
exist on it, so a row that typed those would prove nothing.

## Booting AIX — where this stands

The Install CD boots, and the boot path is Apple's own rather than an
invention. With a blank store, the front keyswitch in Service and the disc
in bay 0 — the documented condition, *"a Network Server that has never been
booted before"* — Open Firmware finds the disc, reads the EBCDIC `IBMA` IPL
record in its block 0, writes a boot configuration, prints

```
cd
RESETing to change Configuration!
```

asks Cuda to pull the system reset line, comes back through POST, and
launches **`bootapple`**, Apple's own bootstrap:

```
bootapple: launched by "OpenFirmware1.1.22"
bootapple: POST results AOK.  Code is  00010000
bootapple: "AAPL,cpu-id" property is  39002089
bootapple: model info is Power Macintosh,AAPL,ShinerESB;MacRISC
bootapple: boot device is "/bandit/53c825@11/sd@0:aix"
```

Every line there is a fact about this model that Apple's bootstrap checked
and accepted: POST's own recorded results, the CPU identity Hammerhead
supplies, the root `compatible` the 53C825A probe set, and the boot path
chosen off the disc. `bootapple` then reads about 2.7 MB off the disc —
1 360 blocks, two at a time — and jumps into the AIX kernel.

### The LCD is the narrator, and it counts

From here the machine stops printing and starts *displaying*. The
front-panel LCD — built in Phase B because POST needs it — is the only
narrator AIX uses, and sampled as a sequence rather than a snapshot it is
a trace of the boot:

```
510   the configuration manager has started
811   the system planar
812   the standard I/O adapter
890   a SCSI-2 adapter                    (the 53C825As)
868   the integrated SCSI adapter         (the 53C94)
538   passing control to a configuration method
723   a CD-ROM drive or other SCSI device
517   attempting to mount the /(root) and /usr file systems
512   restoring the base customised device information
```

— Apple's own table, in *What's New With the Network Server*. So the
kernel is up, `cfgmgr` walks the device tree, configures the fast/wide
controllers — `cfgpscsi`, driver `pscsidd`, exactly the pair the ODM
extraction named before any of this booted — configures the narrow chain
and the disc in bay 0, and goes looking for a root to mount. The driver
negotiates synchronous transfer, takes INQUIRY (standard and
vital-product-data), MODE SENSE and START STOP UNIT, and reads several
hundred blocks off the Install CD.

**Give it the memory.** That sequence is what 512 MB buys; at 64 MB the
configuration manager does not survive its first pass over the SCSI
adapters. The BOS install boots into a RAM filesystem, and a guest's
memory figure is part of the fixture rather than a detail of it — 64 MB is
a comfortable Macintosh number and it hid seven codes' worth of working
machine.

— and then a second configuration pass over the adapters.

**That is where the ladder currently stands.** What stops it is the first
SCSI id with nothing on it, and a wide bus always has some. The driver and
its SCRIPTS program share a command ring with two indices: the driver
advances its write index on every post, and the script advances its read
index only after a select has *succeeded*. A selection time-out halts the
chip at the SELECT, so the entry is never consumed and the read index never
moves; the driver's time-out handler fails the command and restarts the
chip at the top of the script, which reads the same entry and selects the
same absent target again. Every command posted after that one is
unreachable, and when the buffer AIX keeps re-submitting is finally
reclaimed the handler follows the tag's command pointer — the tag is
`target * 8 + LUN`, indexing a table at `softc+$308` — into a page that is
no longer resident. At interrupt level AIX cannot take that fault, and it
panics. The panel says so, in AIX's own format: `888` flashing alternately
with `102` (unexpected system halt), the **crash code** — which is the
PowerPC vector, `300` for a data storage interrupt — and a dump status.

What resynchronises that ring on real hardware is the open question. Only
three things write the script's index — the script itself on success, a SCSI
bus reset, and the unexpected-disconnect handler — and the time-out path
reaches none of them.

Rungs S11 to S13 (the AIX banner, the BOS install to disk, a cold boot of
the installed disk) are open. The dossier's findings 32 to 38 carry the full
account, and `HANDOVER.md` beside them is the way in.

### What getting here cost, and what it says

Fourteen defects, and not one of them was in Network Server code — every
single one was in shared machinery that no existing guest had pushed on.
In order:

| Defect | Where |
|---|---|
| Cuda's RESET SYSTEM only reset Cuda | `av/cuda.c` |
| `ppc_reset` threw away the CPU's time binding | `cpu/ppc/ppc.c` |
| `Wait Reselect` jumped instead of parking | `scripts53c8xx.c` |
| Memory Move clobbered TEMP, the Call/Return link | `scripts53c8xx.c` |
| Interrupt-on-the-fly latched without driving `IRQ/` | `scripts53c8xx.c` |
| A Wait Disconnect still reported UNEXPECTED DISCONNECT | `scripts53c8xx.c` |
| A selection time-out took the Select instruction's alternate address | `scripts53c8xx.c` |
| A selection time-out was instantaneous instead of STIME0's 204.8 ms | `scripts53c8xx.c` |
| A command completed inside the store that started it | `scripts53c8xx.c` |
| A chip that was arbitrating accepted a second start | `scripts53c8xx.c` |
| `SCNTL1`'s RST bit did not drive the SCSI reset line | `scripts53c8xx.c` |
| `ISTAT`'s ABRT bit did not abandon the operation in flight | `scripts53c8xx.c` |
| Grand Central's mode-1 latch ignored the mask | `grand_central.c` |
| INQUIRY overstated its length and ignored EVPD | `core/peripherals/scsi.c` |

Two patterns are worth carrying forward.

**Every one of the engine defects was the model being more helpful, or
faster, than the part.** Jump instead of park, write TEMP, latch without
asserting, report a disconnect the script had asked for, go somewhere
useful on a time-out instead of stopping dead — and, four times over, do in
no time at all something the hardware takes a fifth of a second to do. Each looked like
the forgiving choice and each one broke a driver that was doing something
perfectly ordinary.

The timing ones deserve their own sentence, because they are the least
obvious and did the most damage. **Zero is a wrong answer for how long
anything takes.** A selection time-out reported the instant nobody answers
completes the whole select-fail-report-retry cycle inside the driver's own
doorbell write; the interrupt storm that follows never lets the clock tick,
so the driver's timers never expire and nothing gives up. A command that
completes inside the store that started it re-enters the driver's interrupt
handler while it still holds its own lock, and AIX panics on the assertion
that catches exactly that. Neither is a performance question. Both are the
difference between a machine that is slow and a machine that has stopped.

The Open Firmware driver never noticed any of them because it never waits,
never calls, never uses interrupt-on-the-fly, never asks for a VPD page,
polls with interrupts masked and holds no locks: one guest agreeing with a
model proves much less than it feels like it does.

**The machine's own instruments are better than any amount of reasoning.**
`see <word>` at the Open Firmware prompt settled every question the ROM
could answer. Level-5 SCRIPTS logging over a whole boot costs one
200 000-line file and shows the runaway loop outright. And three digits on
a four-line character LCD, cross-referenced against a table Apple printed,
replaced an afternoon of guessing about where AIX had got to. On this
machine, read the LCD first.

AIX 4.1.5 for the Network Server has **no public source**, so the TNT
reflex — disassemble the guest — looks expensive. It is not. `pscsidd`
and `pscsiddpin` ship **uncompressed** on the Install CD, and although
their XCOFF symbol tables are stripped, every function still carries its
AIX **traceback table**: a zero word, a fixed header, and the function's
own name, sitting after its last instruction. Fifty lines of scanner
recover 24 and 44 named functions with their addresses, and from there the
interrupt handler reads directly. That is how `bsc_intr`'s dispatch on
`SIST0:SIST1` — read as one 16-bit value, masked `$048F` — was settled,
and it is the technique to reach for on any AIX binary.

Its SCRIPTS labels (`phase_reselect`, `sync_nego`, `reqack_too_large`,
`tpf_too_small`, `patcha`…`patchg`) are a specification of what the engine
still has to get right. Its C side has `bsc_ioctl_sleep` and
`e_sleep_thread`: the configuration method issues an ioctl and **sleeps**,
so a command that never completes shows up as a completely idle machine
rather than as anything resembling a crash. The fourth lever remains
`ans-macos-2rom`: any device can be cross-examined through a stack we do
understand.
