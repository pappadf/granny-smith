// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pci.h
// PCI subsystem — the bus controller, the per-machine slot table, the
// card-kind registry, staged per-slot configuration and the lifecycle
// hooks every PCI family uses.  See proposal-pci-architecture.md.
//
// The shape is deliberately the proven NuBus one (nubus.h), upgraded
// where PCI is genuinely different:
//
//   * A machine has ONE pci_root_t and one pci_bus_t per HOST BRIDGE.
//     Bridges are chipset, so the FAMILY creates the buses (tnt/bandit.c)
//     and registers its own builtin devices; the core never includes a
//     machine header.
//   * A device answers config cycles.  Registered device → the generic
//     type-0 header (config_space.c) plus whatever quirks its ops claim;
//     unregistered IDSEL → all-ones, which is the whole "empty slot"
//     model.  A probe must never hang.
//   * Unlike NuBus, a device's REGIONS are not geographic: Open Firmware
//     sizes the BARs and assigns them at boot.  Each bridge hands its
//     PCI memory / I/O windows to the bus (pci_bus_add_window); the bus
//     claims them with a fault interface (empty space faults recoverably
//     — the BART contract) and dispatches an access inside a window to
//     whichever seated device currently decodes that address.
//   * Machines declare TOPOLOGY (pci_slot_decl_t), cards declare
//     ATTACHMENT (pci_attach_t), and compatibility is COMPUTED
//     (pci_card_fits_socket) — nobody enumerates (machine, card) pairs.

#ifndef PCI_H
#define PCI_H

#include "card.h"
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
struct object;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

struct pci_root;
typedef struct pci_root pci_root_t;

// === Machine topology (proposal §6.1) =======================================

typedef enum pci_slot_kind {
    PCI_SLOT_ABSENT = 0, // not declared (an unpopulated IDSEL is
                         // electrically identical to absent — PCI has no
                         // NuBus-style EMPTY/ABSENT distinction)
    PCI_SLOT_BUILTIN, // soldered device the machine names (Control)
    PCI_SLOT_SOCKET, // physical connector — user-populatable
    // A builtin that stands in only while no SOCKET supplies a card of the
    // same class.  The Power Macintosh 9500 shipped with no onboard video
    // at all — it "requires a display card in a PCI slot" — so the emulated
    // machine fakes a Control/Chaos display purely so a cardless boot has
    // somewhere to draw.  Seating a real display card must retire the fake,
    // or the guest sees TWO monitors where the hardware has one.
    //
    // The test is by CLASS, not by name: the generic layer compares the
    // fallback's card_class against the classes the sockets resolved, so it
    // never learns any card's identity.
    PCI_SLOT_BUILTIN_FALLBACK,
} pci_slot_kind_t;

// One entry in a machine's PCI slot table.  Sentinel-terminated arrays end
// at the entry whose `slot` is 0.  TOPOLOGY ONLY: no window addresses
// (bridge truth, owned by the family), no card whitelists (computed), no
// interrupt-pin facts (INTA-D are strapped together per slot on these
// machines, so a slot has exactly one line).
typedef struct pci_slot_decl {
    int slot; // 1-based logical slot; 0 ends the array
    pci_slot_kind_t kind;
    const char *label; // "A1" — display string (AAPL,slot-name-ish)
    int bus; // family bus index (0 = Bandit 1, 1 = Bandit 2, 2 = VCI)
    int device; // PCI device number on that bus (the IDSEL AD line)
    int int_line; // the interrupt-controller line the strapped INTA-D reaches
    const char *builtin_card_id; // BUILTIN: resolved via pci_card_find()
    const char *default_card; // SOCKET: NULL = ships empty
} pci_slot_decl_t;

// True iff card kind `kind` may be seated in slot `s`.  COMPUTED, and the
// one predicate shared by the machine.profile encoder and boot validation.
bool pci_card_fits_socket(const pci_slot_decl_t *s, const pci_card_kind_t *kind);

// === Root + buses ===========================================================

// Create the machine's PCI subsystem.  Called by the family BEFORE it
// builds its host bridges (the bridges create the buses).
pci_root_t *pci_root_create(config_t *cfg);
void pci_root_delete(pci_root_t *root);

// One bus per host bridge.  `index` is the family's own bus numbering and
// is what a slot declaration's `.bus` field names.
pci_bus_t *pci_bus_create(pci_root_t *root, const char *name, int index);
pci_bus_t *pci_bus_by_index(pci_root_t *root, int index);

// pci_space_t — which address space a window forwards / a region decodes —
// is declared in card.h, beside the region types that use it.

// Hand the bus one of the bridge's decode windows.  The bus claims
// `map_base .. map_base+size-1` on the physical map: an access there is
// dispatched to whichever seated device decodes the corresponding PCI
// address, and takes a recoverable transfer error when none does.  For a
// memory window the PCI address IS the physical address; for an I/O
// window only the low `pci_mask` bits are driven (Bandit drives 16).
void pci_bus_add_window(pci_bus_t *bus, pci_space_t space, uint32_t map_base, uint32_t size, uint32_t pci_base,
                        uint32_t pci_mask, const char *what);

// Introspection for a bridge window: the interface the bus claimed the
// window's physical range with, and its context.  The family gets these
// for free through memory_map_add; they are exported so a test (or a
// future debug surface) can drive a window's decode directly.
const memory_interface_t *pci_bus_window_iface(pci_bus_t *bus, int window);
void *pci_bus_window_ctx(pci_bus_t *bus, int window);

// Little-endian LANE REVERSAL on a bus.  A PowerPC in little-endian mode
// does not reorder bytes; it XORs the low three address bits of every
// access with 8-size (7/6/4 for a byte/halfword/word).  A host bridge that
// wants PCI to look byte-address-invariant to such a client undoes that by
// reversing its eight byte lanes: processor-bus byte n is PCI byte n^7.
// The bridge adapter flips it from the chipset register the firmware
// writes (Bandit mode-select, bandit.c); the window dispatch applies it —
// an N-byte access at window offset o reaches PCI offset o^(8-N) with its
// bytes reversed — and a bus master consults it (pci_bus_lane_reverse) so
// the same reversal is applied on its way to host memory.
void pci_bus_set_lane_reverse(pci_bus_t *bus, bool on);
bool pci_bus_lane_reverse(const pci_bus_t *bus);

// Seat a device at `device_num` (its IDSEL AD line).  The bus does NOT
// take ownership of devices registered this way by the family — only of
// the ones its own slot walk created through a card factory.
void pci_bus_add_device(pci_bus_t *bus, pci_device_t *dev, int device_num);
pci_device_t *pci_bus_device(pci_bus_t *bus, int device_num);

// Does this bus have anything seated on it at all?  A family asks after
// pci_seat_slots() has run, when a decode decision depends on whether a
// conditional device actually materialised.
bool pci_bus_is_populated(const pci_bus_t *bus);

// The whole config protocol, from a bridge adapter's point of view.
// Absent (device, function) reads all-ones; writes vanish.
uint32_t pci_bus_cfg_read(pci_bus_t *bus, int dev, uint32_t fn, uint32_t reg);
void pci_bus_cfg_write(pci_bus_t *bus, int dev, uint32_t fn, uint32_t reg, uint32_t byte, uint8_t value);

// === Region backing (proposal §5.4) =========================================
//
// A device declares WHAT backs each BAR; the bus decides WHERE and WHEN it
// appears.  v1 offers the dispatcher backing only: the region answers
// through the owning bridge window, which costs one linear probe per
// access and buys correct fault semantics, no memory-map churn (so
// goldens and determinism are safe) and support for non-linear apertures
// like Control's banked VRAM view.  The host-overlay fast path the
// proposal sketches for a framebuffer needs a memory-map primitive that
// does not exist yet (there is no removal counterpart to
// memory_map_host_region), so it lands with the first card that wants it.
void pci_bar_backing_iface(pci_device_t *dev, int bar, const memory_interface_t *iface, void *ctx);

// Declare a region this device decodes WITHOUT a BAR — a legacy or
// strapped decode, as on parts that predate BAR-based I/O.  See
// pci_fixed_region_t (card.h) for the match semantics; in short, the
// region answers `pci_addr` in [base, base+span) whose masked bits equal
// match_value, which expresses both an ordinary contiguous claim
// (match_mask 0) and ISA-style SPARSE decoding.  The handler is passed
// `pci_addr - base`, so a card does its own sub-decode and the bus needs
// no knowledge of the part's addressing.
//
// Faking an I/O BAR instead would be worse, not simpler: a BAR the card's
// own `reg` property does not mention is one Open Firmware sizes, finds
// and assigns — inventing an address the card does not decode and
// consuming I/O space its firmware expects to own outright.
void pci_device_add_fixed_region(pci_device_t *dev, pci_space_t space, uint32_t base, uint32_t span,
                                 uint32_t match_mask, uint32_t match_value, const memory_interface_t *iface, void *ctx);

// A device's decoded regions may have moved: re-derive them from the
// header state.  Called by config_space.c on every BAR / command write and
// by the bus after a checkpoint restore.
void pci_device_regions_changed(pci_device_t *dev);

// === Staged per-slot configuration (proposal §7) ============================
//
// User picks for the NEXT machine.boot live in a small staged table keyed
// by slot number, consumed (and cleared) by pci_seat_slots.  Slot 0 is the
// WILDCARD entry meaning "the machine's first SOCKET" — the
// machine-independent channel behind machine.boot's `pci_card=`.
#define PCI_STAGED_WILDCARD 0
void pci_staged_card_set(int slot, const char *id); // NULL/"" clears
const char *pci_staged_card_get(int slot);
// One keyed option per slot, routed through the resolved kind's
// stage_option() hook — the generic layer never learns a card's identity.
void pci_staged_option_set(int slot, const char *key, const char *value);
const char *pci_staged_option_get(int slot, const char *key);
void pci_staged_clear_all(void);

// === Lifecycle ==============================================================

// Adopt the machine's slot table.  Called by the family right after
// pci_root_create, before the bridges exist, so the topology is available
// to everything the bridge/device construction touches.
void pci_init(pci_root_t *root, const pci_slot_decl_t *slots);

// Walk the slot table, resolve each entry's card (concrete staged pick >
// wildcard > default_card; validated with pci_card_fits_socket), run the
// factories, seat the results, then build the object model.  Called by the
// family AFTER its bridges and builtin devices exist.
void pci_seat_slots(pci_root_t *root, checkpoint_t *cp);

// The slot declaration for `slot`, or NULL when the machine doesn't
// declare it.  Backs the object model's staged-attr validation.
const pci_slot_decl_t *pci_slot_decl_get(pci_root_t *root, int slot);
// The device seated in `slot`, or NULL, and the card kind that made it.
pci_device_t *pci_slot_device(pci_root_t *root, int slot);
const pci_card_kind_t *pci_slot_kind(pci_root_t *root, int slot);

void pci_checkpoint_save(pci_root_t *root, checkpoint_t *cp);
void pci_checkpoint_restore(pci_root_t *root, checkpoint_t *cp);

// PCI RST#: every seated device's header returns to power-on (command
// clear ⇒ regions undecoded) and its ops->reset runs.
void pci_reset(pci_root_t *root);
// VBL fan-out, for video cards' VSync (the NuBus shape, verbatim).
void pci_tick_vbl(pci_root_t *root);

// === Interrupts (proposal §5.5) =============================================
//
// One line per slot: INTA-D are strapped together on these machines, so a
// multi-function card collapses onto one line.  The bus keeps the
// aggregate and calls the family's delivery hook
// (machine_substrate_t.pci_slot_irq); pci.c never learns what a Grand
// Central is.
void pci_assert_irq(pci_device_t *dev);
void pci_deassert_irq(pci_device_t *dev);

// === Object model (proposal §7) =============================================

// The `machine.pci` class lives in pci_class.c; root.c attaches it (the
// declaration is beside nubus_class's there, not in this header, so core
// files that only drive hardware need not pull in the object model).

void pci_objects_build(pci_root_t *root);
void pci_objects_teardown(void);
// Teardown only if the trees describe `root` (checkpoint-restore ordering:
// the new machine's tree is built before the old machine is destroyed).
void pci_objects_teardown_owned(pci_root_t *root);

// The primary display among the seated PCI devices — first slot in
// declared order whose ops->display() returns non-NULL — or NULL.
display_t *pci_primary_display(pci_root_t *root);
pci_device_t *pci_primary_display_card(pci_root_t *root);

// A display card may nominate one of the object nodes its kind attached as
// the FRAMEBUFFER node — what `machine.screen.source` resolves to.  The
// generic layer stores the nomination and hands back whichever belongs to
// the current primary display, so it never has to test a card's identity
// or guess which child is the framebuffer.
void pci_card_set_framebuffer_object(pci_device_t *dev, struct object *obj);
struct object *pci_active_framebuffer_object(void);

#endif // PCI_H
