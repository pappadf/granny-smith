# PCI

`src/core/peripherals/pci/` is the platform-agnostic PCI substrate: the bus
controller, the generic type-0 configuration header, the card-kind
registry, the slot walk and the `machine.pci.*` object model. It is the PCI sibling of `nubus/`, and deliberately a separate
module — see "Why not one expansion-bus abstraction" below.

| File | What it holds |
|---|---|
| `pci.h` / `pci.c` | bus controller: buses, devices, decode windows, config dispatch, slot table, kind registry, slot walk, lifecycle and interrupt fan-outs |
| `pci_card.h` | `pci_device_t` / `pci_device_ops_t` / `pci_card_kind_t` — the only header a card driver under `cards/` needs |
| `config_space.h` / `config_space.c` | the generic type-0 header: IDs, class, command/status, BAR latch + sizing, expansion-ROM BAR, interrupt line |
| `pci_class.c` | the `machine.pci.slot[N]` object surface |
| `cards/` | pluggable card drivers (`mach64gx.c`, `voodoo2.c`, `scripts53c8xx.c` / `sym53c825.c`, `cirrus54m30.c`) |

Host **bridges** are chipset, so they live with their family
(`machines/tnt/bandit.c`). The core never includes a machine header; the
`core-layering` CI row enforces it.

## The model in five sentences

A machine has one `pci_root_t` and one `pci_bus_t` per host bridge. The
family creates the buses, hands each one the decode **windows** its bridge
forwards, and seats its own builtin devices; the slot walk then seats
whatever the boot document names for each socket. A **device** answers config
cycles: registered → the generic header plus whatever quirks its ops
claim; unregistered IDSEL → all-ones, which is the entire "empty slot"
model. A device's **regions** are not geographic the way NuBus slot space
was — the guest's Open Firmware sizes the BARs and assigns them at boot,
and the bus routes an access inside a window to whichever seated device
currently decodes that address. Everything a card *is* must be
discoverable through config cycles: **the guest enumerates, we answer** —
there is no side-channel injection into the device tree, so if the tree is
wrong, the registers are wrong.

## The two contracts that must not break

Both are inherited verbatim from the hand-rolled Bandit model this
replaced, and both are what let a guest probe safely:

- **An IDSEL with no device registered reads all-ones and swallows
  writes.** A probe must never hang. `pci_bus_cfg_read` is the whole
  implementation.
- **PCI space no device decodes takes a *recoverable* transfer error**
  (`memory_signal_bus_error` — the BART pattern). Open Firmware, NetBSD's
  `badaddr()` and the Slot Manager all probe under a fault catcher, so a
  fault here is a *probe mechanism*, not an error path.

## Config space

`config_space.c` is one implementation for every device — bridges, builtin
pseudo-devices, and every future card. It assembles reads from the static
`pci_config_decl_t` plus the live latches, and it is where the universal
BAR-sizing idiom works with zero per-device code: a BAR latches whatever
is written and masks on **read-back**, so a `$FFFFFFFF` probe reads back
`~(size-1)` with the BAR's type bits in the low nibble.

Details worth knowing:

- A register the device does not implement reads **zero**. Only absent
  *devices* read all-ones.
- Writes arrive one byte lane at a time — the bridge's data port carries
  the low two offset bits on the port address — so a multi-lane BAR
  assignment decodes at each intermediate base, exactly as hardware does.
- `command_writable` masks what the command register accepts;
  `command_reset` names bits that are **hardwired on**. Control needs the
  latter: the Chaos bus swallows config writes outside its two BAR
  offsets, so software can never set its command register and the device
  must decode unconditionally.
- `ops->cfg_read` / `ops->cfg_write` intercept first and return `true` to
  claim a register — that is where Bandit's `$48`/`$50` and Grand
  Central's all-ones presence live.
- `cap_ptr` is the capabilities pointer `$34` reads back (read-only, low
  two bits zero); the block it points at lives beyond the header and is
  the device's own, answered through `ops->cfg_read`. A device that
  declares one also sets `PCI_STATUS_CAP_LIST` in `status_reset`, as the
  silicon does. The ATI Rage 128 is the first card with one: its FCode
  reads the pointer before anything else, so it must exist and terminate.

## Region backing and the overlay question

A device declares **what** backs each BAR; the bus decides **where and
when** it appears (`pci_bar_backing_iface`, and `pci_device_regions_changed`
as the single transition point). This is the region-registration helper
NuBus never had — no card repeats `base + offset` arithmetic against
`cfg->mem_map`.

v1 offers **one** backing kind: the device's own `memory_interface_t`,
dispatched by the owning bridge window. That costs a short linear probe
per access and buys correct fault semantics for free, no memory-map churn
(so goldens and determinism are safe), and support for non-linear
apertures — Control's banked VRAM view has a *hole* between its banks that
the sizing probe depends on, which no flat host mapping can express.

A host-overlay fast path for a framebuffer is **not** implemented, and the reason is concrete: the memory
map has no removal counterpart to `memory_map_host_region` (its fill list
is a fixed 16-entry table built for init-time registration), so a *movable*
host-backed BAR cannot be unmapped today. That primitive lands with the
first card that wants the fast path.

## Topology, attachment, and computed fit

The same three-party split NuBus uses:

- **Machines declare topology** — `hw_profile_t.pci_slots`, a
  sentinel-terminated `pci_slot_decl_t[]` naming each socket's bus, IDSEL
  and interrupt line. One pointer feeds both `catalog.profile` and
  `pci_init`, so the configuration view and the runtime cannot drift.
- **Cards declare attachment** — `PCI_ATTACH_PCI` for a real card,
  `PCI_ATTACH_BUILTIN` (the conservative zero default) for a soldered-down
  device only a `BUILTIN` slot entry can name.
- **Compatibility is computed** — `pci_card_fits_socket()`, the one
  predicate shared by the profile encoder and boot validation. Nobody
  enumerates (machine, card) pairs; adding a driver offers it on every
  compatible machine.

`decode ≠ population ≠ fit` is the rule behind that split: what a *bridge*
decodes is chipset truth owned by the family, which *sockets* exist is
topology owned by the profile, and which *cards* fit is computed. Three
parties, three files.

## Slot configuration

What each socket seats is the configuration's: the document's `cards`
(`machine.boot config=`, [object-model.md](../object/object-model.md#the-configuration-document))
names a card per slot (`pci_1` … by the slot's index) with its options,
and the slot's `connected` display entry says whether the monitor is on
it.  `machine.boot slots=` is the older per-slot grammar the document is
translated into, and `pci_card=` / `pci_option=` / `prom=` are sugar for
the first socket (and, for `prom=`, every slot whose card the file
provides).  A pass-through 3D card (`card_class` `"3d"`, the Voodoo2) takes
the screen while it holds the output, whichever device the monitor is
connected to (`pci_connected_display`).
`machine_slots_resolve` validates every entry before the running machine
is touched — the card fits (`pci_card_fits_socket`), the kind accepts each
option (`accepts_option`), the PROM identifies as the card's — and puts the
result in `cfg->build_opts`. `pci_seat_slots` seats each socket from its
entry, else its `default_card`, and hands the factory its slot's entry.

Card-specific object children go through the kind's `attach_objects()`
hook, and options through `accepts_option()`, so the generic layer never
learns a card's identity — the two places `nubus.c` had to include card
headers.

Each seated card names its kind (`machine.pci.slot[N].card.id`, and
`machine.nubus.slot[N].card.id` on the other bus), and the root's part of a
checkpoint carries the slot entries it seated, which a checkpoint restore
builds the slots from. (`machine.restart` builds nothing: the cards it
power-cycles are the ones already seated.)

## Interrupts

One line per slot: `/INTA`-`/INTD` are strapped together on these
machines, so there is no swizzle and a multi-function card collapses onto
one line. `pci_assert_irq` / `pci_deassert_irq` keep the aggregate and
call `machine_substrate_t.pci_slot_irq`; the family owns delivery. There
is no umbrella edge (unlike NuBus's CA1 pulse) because each slot has its
own source bit in the machine's interrupt controller.

## Object model

`machine.pci.slot[N]` carries `number`, `label`, `bus`, `device` and
`irq`. A node exists for **every declared socket and builtin, populated or
not** — the board's topology. It configures nothing: a different card is a
different `machine.boot`. A populated slot
grows a `card` subtree with the identity attributes and a `config` child
(Advanced) exposing the live header and one `bar[i]` node per declared BAR
plus the expansion-ROM BAR. `catalog.pci_cards` lists the registry.

## Why not one expansion-bus abstraction

NuBus and PCI differ in kind, not degree, on the things that matter:
geographic identity vs (bus, device) with addresses from BARs; declaration
ROM at a fixed slot address vs config cycles; fixed regions vs firmware
assignment; 68K driver code in the ROM vs an ndrv inside an FCode image. A
shared `expansion_card_t` base would sprout `if (bus_type == PCI)` in
every shared path. Two small parallel modules beat one abstract one; where
behaviour genuinely is identical — the kind/factory/registry idiom,
checkpoint hooks, display presentation — the *pattern* is copied, which
keeps both readable without coupling them.

## Regions that have no BAR

Parts that predate BAR-based I/O decode at **strapped** addresses instead.
`pci_device_add_fixed_region()` declares one: it answers a PCI address in
`[base, base+span)` whose masked bits equal `match_value`, and hands the
device `pci_addr - base` so the card does its own sub-decode.

The match is a mask/value pair rather than a plain range because the first
such device decodes **sparsely**. A mach64 GX compares only the low bits of
an I/O address against its strapped base and uses the upper bits as a
register select, so it answers 64 scattered dwords out of the 64 KB I/O
space and nothing else; a contiguous claim would have it swallow addresses
it does not drive. For the mach64 that is `match_mask $3FC`, `match_value
$2EC`, `span $10000` — `$3FC`, not `$3FF`, because each selected register
is 32 bits wide and `base+0..base+3` are byte lanes of the same register.
A mask of 0 makes the match vacuous, which is an ordinary contiguous claim.

Fixed regions are gated by the command register's space-enable bit exactly
as BARs are, so a card software has not enabled decodes nothing. The gate
is derived from `cfg.command`, so there is no new checkpointed state.

Faking an I/O BAR instead would be worse, not simpler: a BAR the card's own
`reg` property does not mention is one Open Firmware sizes, finds and
assigns — inventing an address the card does not decode, and consuming I/O
space the card's firmware expects to own outright.

## Endianness at a card's edge

The family rule (`TNT_LE32`) is a *TNT* rule. A pluggable card is not a TNT
device: it can be seated in any PCI machine, so it must not reach for a
family macro. A card whose registers are little-endian applies its own swap
at its own edge and says so in its header comment.

The Voodoo2 (`cards/voodoo2.c`) is the second worked example, and the
richer one: on top of the physical little-endian edge sit three
*guest-controlled* swizzle paths — the register file's per-access byte
swizzle (`fbiInit0[3]` enables it, register **address bit 20** selects it,
so a big-endian driver picks swizzled or raw per write by choosing an
aliased address), the LFB's independent write and read swizzle/word-swap
bits whose transform order *reverses* between directions, and the texture
path's own pair in `tLOD`. The model honours the bits rather than
assuming a big-endian host, because Mac Glide drives them from the
application (`grLfbWriteColorSwizzle`).
### Bridge byte-lane reversal for a little-endian client

A host bridge can be told to reverse its eight byte lanes so that a CPU
running in little-endian mode sees PCI as byte-address-invariant. Turn it
on with `pci_bus_set_lane_reverse(bus, true)`; a bus master consults
`pci_bus_lane_reverse(bus)`. When it is on, the window dispatch applies the
reversal once, before decode: an N-byte access at window offset `o` reaches
PCI offset `o ^ (8 - N)` with its bytes reversed, so no device model ever
sees anything but plain PCI byte addresses and values. This is the
hardware counterpart to the classic PowerPC little-endian *address munge*
(the CPU XORs an access's low address bits by `8 - size` rather than
reordering bytes); the two cancel, so a little-endian guest reaches every
device behind the bridge with plain loads and stores. Anything that
bypasses the CPU — bus-master DMA, and a family's direct mapping of a
pass-through region — must apply the same `^7` byte reversal itself while
the flag is set, because it never went through the window. The Apple
Network Server's Bandit is the first user: its `$50` mode-select bit 24
drives the flag (`machines/tnt/bandit.c`), and clearing it is how the
2.26NT firmware brings the bridge into agreement with a little-endian
Windows NT client.

## Bus mastering

`pci_dma_read` / `pci_dma_write` move bytes between a seated device and
host memory with the device as bus master. Three rules:

- **Gated on `BUS_MASTER_EN`.** A device whose command register has
  `PCI_CMD_MASTER` clear moves nothing: both calls return 0 and a refused
  read leaves the buffer all-ones (a master abort floats high), with a
  log line naming the device — a driver that forgets the bit otherwise
  hangs with no diagnostic. A transfer that works with the bit clear would
  be a fidelity bug.
- **No IOMMU, no CPU MMU.** A PCI address is a guest-physical address on
  these machines (the DBDMA rule). RAM moves through the backing store
  directly; anything else — a device register, unmapped space, a range
  straddling the end of RAM — takes the bus's slow path byte by byte.
- **The bridge's lane reversal applies** (above): while it is on, PCI byte
  `n` of the transfer is host byte `n ^ 7`.

The calls are synchronous. *Pacing* is the card's business, not the bus's:
a command processor that must show progress rather than completion
schedules its own fetches. The Rage 128's CCE is the first card on the API: it
fetches its ring and indirect buffer and writes its read pointer back
through it, completing each fetch inside the guest's `WPTR` write. The 53C8xx SCRIPTS engine predates the API
and still masters through its own copy of the same path
(`cards/scripts53c8xx.c`), which does not consult `BUS_MASTER_EN`.

## Slot kinds

`PCI_SLOT_SOCKET` is a user-populatable connector; `PCI_SLOT_BUILTIN` is a
soldered device the machine names.

`PCI_SLOT_BUILTIN_FALLBACK` is a builtin that stands in **only while no
socket supplies a card of the same class**. The Power Macintosh 9500
shipped with no onboard video at all, so the emulated machine fakes a
Control/Chaos display purely so a cardless boot has somewhere to draw;
seating a real display card retires the fake, because otherwise the guest
sees two monitors where the hardware has one. The test is by `card_class`,
resolved in a first pass over the sockets, so the generic layer never
learns any card's identity and declaration order does not matter.

## Status

The generic core, the TNT family migrated onto it (Bandit/Chaos as adapters,
Control as a registered BUILTIN card kind, Grand Central's config presence
at device 16), slot topology for all three TNT models, the object model,
per-slot configuration and the profile surface — and now the PCI I/O window
on both Bandits, non-BAR region decode, expansion-ROM provisioning
(`docs/reference/hardware/pci/expansion-rom.md`), and the first pluggable card kind,
the Apple Accelerated PCI Graphics Card
(`src/core/peripherals/pci/cards/mach64gx.c`), which boots System 7.6 to a
desktop on a Power Macintosh 9500.

The **ROM-less socket-card path** is exercised for real by the 3dfx Voodoo2
(`cards/voodoo2.c`, `docs/internals/core/peripherals/pci/cards/voodoo2.md`):
`rom_size = 0` and `requires_prom = false` were always legal (no-ROM cards
fall out of the generic core for free) but the Voodoo2 is the first card to
ride them — the guest's Open Firmware sizes and assigns the BAR,
builds a generated `pciVVVV,DDDD` node, loads no driver, and leaves
Memory Space Enable clear for the disk-loaded driver to set. A card that
must never be the machine's display declares `card_class = "3d"` so the
9500's `PCI_SLOT_BUILTIN_FALLBACK` Control is not retired by it.

The **generic bus-master path** (`pci_dma_read`/`pci_dma_write`, above),
the **capabilities pointer** and the first **64 MB prefetchable BAR** are
in the core, pinned by the unit suite (`tests/unit/suites/pci`). The ATI
Rage 128 GL (`cards/rage128.c`,
`docs/internals/core/peripherals/pci/cards/rage128.md`) is the first card
to use all three (its CCE masters the bus), and the first with a BAR its FCode leaves out of `reg`
yet needs assigned: it probes through its I/O BAR.

Not done, with reasons: the host-overlay BAR fast path (above); PCI-PCI
bridges (type-1 cycles keep returning all-ones — no subordinate buses
exist on these machines).

## Card options

`machine.boot`'s `pci_option="key=value[,key=value]"` carries options to
the card `pci_card=` names (a `slots=` entry carries them for any slot).
The generic layer only splits them; each pair is offered to the kind's
`accepts_option()` hook before the boot begins, and one the card does not
accept rejects the boot, so no card identity reaches the boot path and a
typo is never silently the default. The factory reads its options from its
slot's entry (`slot_opts_option`).

A kind also DECLARES the options a user should be offered
(`pci_card_kind_t.options`), and `catalog.profile` publishes them, so a
frontend can render a control per option without knowing which card it is.
Declaring is separate from accepting: a card may still take keys it does
not advertise (the Mach64 GX accepts `monitor=` for debugging but offers
only `vram=`, because it senses its monitor for itself).

A PCI display card's video MODE is a different matter and still has no
path: `video_mode=` is NuBus sugar and a PCI slot entry takes no `mode=`
(the boot is rejected), so the web UI hides the Video Mode row for a PCI
pick and the card uses the mode its driver programs.

`pci_bus_is_populated()` exists for one caller: a family that has to pick
between two bridges claiming the same physical range, where the tie is
broken by which bus actually seated something.  See the TNT doc for the
`$90000000` case.

## See also

- `docs/internals/core/peripherals/pci/cards/` — per-card documents (the NuBus
  `cards/` convention, adopted for PCI with the Voodoo2)
- `docs/internals/machines/tnt/tnt.md` — the bridge adapter, slot tables and the
  interrupt map
- `docs/reference/hardware/pci/pci.md` — the PCI bus hardware reference:
  the address spaces, the config header, cycle types and the Open Firmware
  programming model
- `docs/reference/hardware/pci/expansion-rom.md` — expansion-ROM identity and
  provisioning, the FCode path's half of the story
- `docs/reference/hardware/nubus/declaration-rom.md` — the declaration-ROM sibling the
  PROM path mirrors
- `tests/unit/suites/pci/` — the config-cycle contract, pinned
