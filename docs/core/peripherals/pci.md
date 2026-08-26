# PCI

`src/core/peripherals/pci/` is the platform-agnostic PCI substrate: the bus
controller, the generic type-0 configuration header, the card-kind
registry, staged per-slot configuration and the `machine.pci.*` object
model. It is the PCI sibling of `nubus/`, and deliberately a separate
module — see "Why not one expansion-bus abstraction" below.

| File | What it holds |
|---|---|
| `pci.h` / `pci.c` | bus controller: buses, devices, decode windows, config dispatch, slot table, kind registry, staged config, lifecycle and interrupt fan-outs |
| `card.h` | `pci_device_t` / `pci_device_ops_t` / `pci_card_kind_t` — the only header a card driver under `cards/` needs |
| `config_space.h` / `config_space.c` | the generic type-0 header: IDs, class, command/status, BAR latch + sizing, expansion-ROM BAR, interrupt line |
| `pci_class.c` | the `machine.pci.slot[N]` object surface |
| `cards/` | pluggable card drivers (empty today) |

Host **bridges** are chipset, so they live with their family
(`machines/tnt/bandit.c`). The core never includes a machine header; the
`core-layering` CI row enforces it.

## The model in five sentences

A machine has one `pci_root_t` and one `pci_bus_t` per host bridge. The
family creates the buses, hands each one the decode **windows** its bridge
forwards, and seats its own builtin devices; the slot walk then seats
whatever the user staged into a socket. A **device** answers config
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

The host-overlay fast path the architecture proposal sketches for a
framebuffer is **not** implemented, and the reason is concrete: the memory
map has no removal counterpart to `memory_map_host_region` (its fill list
is a fixed 16-entry table built for init-time registration), so a *movable*
host-backed BAR cannot be unmapped today. That primitive lands with the
first card that wants the fast path.

## Topology, attachment, and computed fit

The same three-party split NuBus uses:

- **Machines declare topology** — `hw_profile_t.pci_slots`, a
  sentinel-terminated `pci_slot_decl_t[]` naming each socket's bus, IDSEL
  and interrupt line. One pointer feeds both `machine.profile` and
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

## Staged configuration

User picks for the next `machine.boot` live in a staged table keyed by
slot, consumed **and cleared** by `pci_seat_slots`. Slot 0 is the wildcard
— "the machine's first socket" — which is what `machine.boot`'s
`pci_card=` writes; concrete slots are staged through
`machine.pci.slot[N].card_id`. Precedence is concrete > wildcard >
`default_card`, and a staged pick is honoured only if it fits
(`pci_card_fits_socket`); a rejection logs at a visible level rather than
silently booting something else.

Per-slot **options** go through one keyed channel routed to the resolved
kind's `stage_option()` hook, and card-specific object children through
its `attach_objects()` hook. Both exist so the generic layer never learns
a card's identity — the two places `nubus.c` had to include card headers.

The **resolved** picks are captured in the built-from record
(`machine_config_note_slot_card`) for *both* buses, so `machine.restart`
re-seats every populated slot instead of only the wildcard one.

## Interrupts

One line per slot: `/INTA`-`/INTD` are strapped together on these
machines, so there is no swizzle and a multi-function card collapses onto
one line. `pci_assert_irq` / `pci_deassert_irq` keep the aggregate and
call `machine_substrate_t.pci_slot_irq`; the family owns delivery. There
is no umbrella edge (unlike NuBus's CA1 pulse) because each slot has its
own source bit in the machine's interrupt controller.

## Object model

`machine.pci.slot[N]` carries `number`, `label`, `bus`, `device`, `irq`
and the read/write staged `card_id`. A node exists for **every declared
socket and builtin, populated or not** — an empty socket's staged
attribute is exactly how the next boot gets configured. A populated slot
grows a `card` subtree with the identity attributes and a `config` child
(Advanced) exposing the live header and one `bar[i]` node per declared BAR
plus the expansion-ROM BAR. `machine.pci.cards()` lists the registry.

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

## Status

Phase 1 of `proposal-pci-architecture`: the generic core, the TNT family
migrated onto it (Bandit/Chaos as adapters, Control as a registered
BUILTIN card kind, Grand Central's config presence at device 16), slot
topology for all three TNT models, the object model, staged configuration
and the profile surface. Sockets exist and probe correctly but no
pluggable card kind is registered yet — that is Phase 2 (expansion-ROM /
FCode plumbing and the Apple Mach64 GX display card).

Not done, with reasons: the host-overlay BAR fast path (above); the PCI
I/O window on TNT (nothing decodes I/O space there yet — see
`docs/machines/tnt/tnt.md`); Bandit 2's memory window (its forwarded range
is unresolved); PCI-PCI bridges (type-1 cycles keep returning all-ones —
no subordinate buses exist on these machines); bus mastering (no modelled
device masters, and the DBDMA hooks are the precedent when one does).

## See also

- `docs/machines/tnt/tnt.md` — the bridge adapter, slot tables and the
  interrupt map
- `docs/core/peripherals/nubus_vrom.md` — the declaration-ROM identity
  machinery the FCode expansion-ROM path will mirror
- `tests/unit/suites/pci/` — the config-cycle contract, pinned
