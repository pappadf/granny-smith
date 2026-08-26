// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pci.c
// The PCI bus controller: device tables, config dispatch, bridge-window
// decode, the card-kind registry, staged per-slot configuration, the slot
// walk and the lifecycle / interrupt fan-outs.  See pci.h and
// proposal-pci-architecture.md §5.
//
// Nothing here knows about any machine: a family creates a bus per host
// bridge, hands the bus its decode windows, seats its own builtin devices
// and lets the slot walk seat the user's cards.  The two facts that make
// the whole model work are inherited from bandit-chaos-pci.md and kept
// verbatim from the hand-rolled model this replaces:
//
//   * an IDSEL with no device registered reads ALL-ONES and swallows
//     writes — a probe must never hang, and
//   * PCI space no device decodes takes a RECOVERABLE transfer error, so
//     Open Firmware and the OSes can probe under a fault catcher.

#include "pci.h"

#include "checkpoint.h"
#include "config_space.h"
#include "log.h"
#include "machine_config.h" // the built-from record's per-slot picks
#include "machine_profile.h" // machine_substrate_t (slot-IRQ routing)
#include "system_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("pci");

#define PCI_MAX_BUSES     4
#define PCI_MAX_DEVICES   32 // IDSEL AD11..AD31 (0..10 never exist)
#define PCI_MAX_WINDOWS   4
#define PCI_MAX_SLOTS     16 // slot numbers are 1-based; [0] is the wildcard
#define PCI_STAGED_OPTS   2 // keyed options staged per slot
#define PCI_OPT_KEY_MAX   24
#define PCI_OPT_VALUE_MAX 48

// One decode window a bridge forwards onto its bus.
typedef struct pci_window {
    pci_bus_t *bus;
    pci_space_t space;
    uint32_t map_base; // physical base claimed on the memory map
    uint32_t size;
    uint32_t pci_base; // PCI address of `map_base`
    uint32_t pci_mask; // address bits the bridge actually drives
    char what[32];
    memory_interface_t iface;
} pci_window_t;

struct pci_bus {
    pci_root_t *root;
    config_t *cfg;
    char name[24];
    int index; // the family's bus numbering (slot decls name it)
    pci_device_t *dev[PCI_MAX_DEVICES];
    bool owns[PCI_MAX_DEVICES]; // seated by our slot walk: we free it
    pci_window_t window[PCI_MAX_WINDOWS];
    int window_count;
};

struct pci_root {
    config_t *cfg;
    const pci_slot_decl_t *slots; // the machine's topology (may be NULL)
    pci_bus_t *bus[PCI_MAX_BUSES];
    int bus_count;
    pci_device_t *slot_dev[PCI_MAX_SLOTS]; // device seated in slot N
    const pci_card_kind_t *slot_kind[PCI_MAX_SLOTS]; // and the kind that made it
    uint32_t slot_irq_mask; // aggregate of asserted slot lines
};

// === Card-kind registry =====================================================
//
// One explicit list (no linker-section magic).  Adding a card driver is one
// extern plus one entry.

extern const pci_card_kind_t tnt_control_kind; // machines/tnt/control.c
extern const pci_card_kind_t mach64_gx_kind; // peripherals/pci/cards/mach64gx.c

static const pci_card_kind_t *const g_card_registry[] = {
    &tnt_control_kind,
    &mach64_gx_kind,
    NULL,
};

const pci_card_kind_t *const *pci_card_registry(void) {
    return g_card_registry;
}

const pci_card_kind_t *pci_card_find(const char *id) {
    if (!id || !*id)
        return NULL;
    for (const pci_card_kind_t *const *p = g_card_registry; *p; p++) {
        if (strcmp((*p)->id, id) == 0)
            return *p;
    }
    return NULL;
}

// Compare two ids ignoring underscores, so a near-miss typo earns a
// did-you-mean (the nubus_card_suggest rule).
static bool ids_match_sans_underscores(const char *a, const char *b) {
    while (*a == '_')
        a++;
    while (*b == '_')
        b++;
    while (*a && *b) {
        if (*a != *b)
            return false;
        a++;
        b++;
        while (*a == '_')
            a++;
        while (*b == '_')
            b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *pci_card_suggest(const char *id) {
    if (!id || !*id)
        return NULL;
    for (const pci_card_kind_t *const *p = g_card_registry; *p; p++) {
        if (ids_match_sans_underscores((*p)->id, id))
            return (*p)->id;
    }
    return NULL;
}

// Card ↔ socket compatibility, COMPUTED from the two declarations: the
// slot must be a user-configurable socket and the kind must attach through
// a genuine PCI connector.  Builtin devices (the conservative zero
// default) exist only where a BUILTIN slot decl names them.
bool pci_card_fits_socket(const pci_slot_decl_t *s, const pci_card_kind_t *kind) {
    if (!s || !kind)
        return false;
    if (s->kind != PCI_SLOT_SOCKET)
        return false;
    return kind->attach == PCI_ATTACH_PCI;
}

// === Staged per-slot configuration ==========================================

typedef struct pci_staged_opt {
    char key[PCI_OPT_KEY_MAX];
    char value[PCI_OPT_VALUE_MAX];
} pci_staged_opt_t;

typedef struct pci_staged_slot {
    char card[32];
    pci_staged_opt_t opt[PCI_STAGED_OPTS];
} pci_staged_slot_t;

static pci_staged_slot_t s_staged[PCI_MAX_SLOTS];

static bool staged_slot_valid(int slot) {
    return slot >= 0 && slot < PCI_MAX_SLOTS;
}

void pci_staged_card_set(int slot, const char *id) {
    if (!staged_slot_valid(slot))
        return;
    snprintf(s_staged[slot].card, sizeof s_staged[slot].card, "%s", (id && *id) ? id : "");
}

const char *pci_staged_card_get(int slot) {
    if (!staged_slot_valid(slot))
        return NULL;
    return s_staged[slot].card[0] ? s_staged[slot].card : NULL;
}

void pci_staged_option_set(int slot, const char *key, const char *value) {
    if (!staged_slot_valid(slot) || !key || !*key)
        return;
    pci_staged_opt_t *free_slot = NULL;
    for (int i = 0; i < PCI_STAGED_OPTS; i++) {
        pci_staged_opt_t *o = &s_staged[slot].opt[i];
        if (strcmp(o->key, key) == 0) {
            snprintf(o->value, sizeof o->value, "%s", (value && *value) ? value : "");
            if (!o->value[0])
                o->key[0] = '\0'; // "" clears the entry
            return;
        }
        if (!free_slot && !o->key[0])
            free_slot = o;
    }
    if (!value || !*value)
        return; // clearing an option that was never staged
    if (!free_slot) {
        LOG(0, "staged-option table full; slot %d option '%s' dropped", slot, key);
        return;
    }
    snprintf(free_slot->key, sizeof free_slot->key, "%s", key);
    snprintf(free_slot->value, sizeof free_slot->value, "%s", value);
}

const char *pci_staged_option_get(int slot, const char *key) {
    if (!staged_slot_valid(slot) || !key)
        return NULL;
    for (int i = 0; i < PCI_STAGED_OPTS; i++) {
        const pci_staged_opt_t *o = &s_staged[slot].opt[i];
        if (o->key[0] && strcmp(o->key, key) == 0)
            return o->value;
    }
    return NULL;
}

void pci_staged_clear_all(void) {
    memset(s_staged, 0, sizeof s_staged);
}

// === Bridge-window decode ===================================================
//
// A window's job is the BART contract, generalised: find the seated device
// whose enabled BAR covers this PCI address, or fault recoverably.  The
// linear probe is deliberate — see pci.h's note on why v1 has no overlay
// fast path.

// What answered a window access: the handler, its context and the offset
// into the region.  BAR-derived and fixed regions both resolve to this, so
// the six accessors below don't care which kind decoded.
typedef struct pci_decode {
    const memory_interface_t *iface;
    void *ctx;
    uint32_t sub;
} pci_decode_t;

// Locate whatever decodes `pci_addr` in `w`'s space.  False when nothing
// does — which is the fault path, and the whole BART contract.
static bool window_locate(const pci_window_t *w, uint32_t pci_addr, pci_decode_t *out) {
    const pci_bus_t *bus = w->bus;
    for (int d = 0; d < PCI_MAX_DEVICES; d++) {
        const pci_device_t *dev = bus->dev[d];
        if (!dev)
            continue;
        for (int b = 0; b < PCI_BAR_SLOTS; b++) {
            const pci_bar_backing_t *bk = &dev->backing[b];
            if (bk->kind == PCI_BACKING_NONE || !bk->mapped)
                continue;
            // The ROM BAR and every memory BAR live in memory space; only
            // an I/O-kind BAR answers an I/O window.
            bool is_io = (b < PCI_NUM_BARS && dev->decl && dev->decl->bar[b].kind == PCI_BAR_IO);
            if ((w->space == PCI_SPACE_IO) != is_io)
                continue;
            uint32_t size = pci_cfg_bar_size(dev, b);
            if (pci_addr - bk->base < size) {
                out->iface = bk->iface;
                out->ctx = bk->ctx;
                out->sub = pci_addr - bk->base;
                return true;
            }
        }
        // Then the non-BAR regions: parts that predate BAR-based I/O decode
        // at strapped addresses, and a sparse decoder answers only its own
        // congruence class inside the span (card.h).
        for (int r = 0; r < PCI_FIXED_REGIONS; r++) {
            const pci_fixed_region_t *fr = &dev->fixed[r];
            if (!fr->iface || !fr->mapped || fr->space != w->space)
                continue;
            if (pci_addr - fr->base >= fr->span)
                continue;
            if ((pci_addr & fr->match_mask) != fr->match_value)
                continue;
            out->iface = fr->iface;
            out->ctx = fr->ctx;
            out->sub = pci_addr - fr->base;
            return true;
        }
    }
    return false;
}

// PCI address of a window offset, and the fault reporter for offsets no
// device claims.
static uint32_t window_pci_addr(const pci_window_t *w, uint32_t offset) {
    return w->pci_base + (offset & w->pci_mask);
}

static void window_fault(const pci_window_t *w, uint32_t offset, bool write) {
    LOG(4, "%s: unclaimed %s $%08X", w->what, write ? "write" : "read", w->map_base + offset);
    memory_signal_bus_error(w->map_base + offset, write);
}

static uint8_t window_read8(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFu;
    }
    return d.iface->read_uint8(d.ctx, d.sub);
}

static uint16_t window_read16(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFFFu;
    }
    return d.iface->read_uint16(d.ctx, d.sub);
}

static uint32_t window_read32(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFFFFFFFu;
    }
    return d.iface->read_uint32(d.ctx, d.sub);
}

static void window_write8(void *ctx, uint32_t offset, uint8_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint8(d.ctx, d.sub, value);
}

static void window_write16(void *ctx, uint32_t offset, uint16_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint16(d.ctx, d.sub, value);
}

static void window_write32(void *ctx, uint32_t offset, uint32_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint32(d.ctx, d.sub, value);
}

void pci_bus_add_window(pci_bus_t *bus, pci_space_t space, uint32_t map_base, uint32_t size, uint32_t pci_base,
                        uint32_t pci_mask, const char *what) {
    if (!bus || bus->window_count >= PCI_MAX_WINDOWS)
        return;
    pci_window_t *w = &bus->window[bus->window_count++];
    w->bus = bus;
    w->space = space;
    w->map_base = map_base;
    w->size = size;
    w->pci_base = pci_base;
    w->pci_mask = pci_mask;
    snprintf(w->what, sizeof w->what, "%s", what ? what : "PCI window");
    w->iface.read_uint8 = window_read8;
    w->iface.read_uint16 = window_read16;
    w->iface.read_uint32 = window_read32;
    w->iface.write_uint8 = window_write8;
    w->iface.write_uint16 = window_write16;
    w->iface.write_uint32 = window_write32;
    memory_map_add(bus->cfg->mem_map, map_base, size, w->what, &w->iface, w);
}

const memory_interface_t *pci_bus_window_iface(pci_bus_t *bus, int window) {
    if (!bus || window < 0 || window >= bus->window_count)
        return NULL;
    return &bus->window[window].iface;
}

void *pci_bus_window_ctx(pci_bus_t *bus, int window) {
    if (!bus || window < 0 || window >= bus->window_count)
        return NULL;
    return &bus->window[window];
}

// === Region backing =========================================================

void pci_bar_backing_iface(pci_device_t *dev, int bar, const memory_interface_t *iface, void *ctx) {
    if (!dev || bar < 0 || bar >= PCI_BAR_SLOTS)
        return;
    pci_bar_backing_t *bk = &dev->backing[bar];
    bk->kind = iface ? PCI_BACKING_IFACE : PCI_BACKING_NONE;
    bk->iface = iface;
    bk->ctx = ctx;
    bk->mapped = false;
    bk->base = 0;
}

void pci_device_add_fixed_region(pci_device_t *dev, pci_space_t space, uint32_t base, uint32_t span,
                                 uint32_t match_mask, uint32_t match_value, const memory_interface_t *iface,
                                 void *ctx) {
    if (!dev || !iface || !span)
        return;
    for (int r = 0; r < PCI_FIXED_REGIONS; r++) {
        pci_fixed_region_t *fr = &dev->fixed[r];
        if (fr->iface)
            continue;
        fr->iface = iface;
        fr->ctx = ctx;
        fr->space = space;
        fr->base = base;
        fr->span = span;
        fr->match_mask = match_mask;
        fr->match_value = match_value;
        fr->mapped = false; // pci_device_regions_changed applies the gate
        return;
    }
    LOG(0, "%s: no free fixed-region slot (PCI_FIXED_REGIONS = %d)",
        (dev->ops && dev->ops->name) ? dev->ops->name(dev) : "device", PCI_FIXED_REGIONS);
}

// Re-derive every decoded region of `dev` from its header state.  This is
// the ONE place a BAR transition happens, so a device driver never has to
// track where it currently answers.
void pci_device_regions_changed(pci_device_t *dev) {
    if (!dev)
        return;
    // Non-BAR regions have no latch to move: their address is strapped, so
    // the only thing that changes is whether the command register enables
    // the space they sit in.
    for (int r = 0; r < PCI_FIXED_REGIONS; r++) {
        pci_fixed_region_t *fr = &dev->fixed[r];
        if (!fr->iface)
            continue;
        uint16_t gate = (fr->space == PCI_SPACE_IO) ? PCI_CMD_IO_SPACE : PCI_CMD_MEM_SPACE;
        bool on = (dev->cfg.command & gate) != 0;
        if (on == fr->mapped)
            continue;
        fr->mapped = on;
        LOG(2, "%s: fixed %s region $%08X+$%X %s", (dev->ops && dev->ops->name) ? dev->ops->name(dev) : "device",
            (fr->space == PCI_SPACE_IO) ? "I/O" : "memory", fr->base, fr->span, on ? "decodes" : "no longer decodes");
    }
    for (int b = 0; b < PCI_BAR_SLOTS; b++) {
        pci_bar_backing_t *bk = &dev->backing[b];
        if (bk->kind == PCI_BACKING_NONE)
            continue;
        bool on = pci_cfg_bar_enabled(dev, b);
        uint32_t base = on ? pci_cfg_bar_base(dev, b) : 0;
        if (on == bk->mapped && base == bk->base)
            continue;
        uint32_t was = bk->base;
        bk->mapped = on;
        bk->base = base;
        LOG(2, "%s: BAR %d %s $%08X", (dev->ops && dev->ops->name) ? dev->ops->name(dev) : "device", b,
            on ? "decodes at" : "no longer decodes, was at", on ? base : was);
        if (dev->ops && dev->ops->bar_map)
            dev->ops->bar_map(dev, b, base, on);
    }
}

// === Buses and devices ======================================================

pci_root_t *pci_root_create(config_t *cfg) {
    if (!cfg)
        return NULL;
    pci_root_t *root = calloc(1, sizeof(*root));
    if (!root)
        return NULL;
    root->cfg = cfg;
    return root;
}

pci_bus_t *pci_bus_create(pci_root_t *root, const char *name, int index) {
    if (!root || root->bus_count >= PCI_MAX_BUSES)
        return NULL;
    pci_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus)
        return NULL;
    bus->root = root;
    bus->cfg = root->cfg;
    bus->index = index;
    snprintf(bus->name, sizeof bus->name, "%s", name ? name : "PCI");
    root->bus[root->bus_count++] = bus;
    return bus;
}

pci_bus_t *pci_bus_by_index(pci_root_t *root, int index) {
    if (!root)
        return NULL;
    for (int i = 0; i < root->bus_count; i++) {
        if (root->bus[i]->index == index)
            return root->bus[i];
    }
    return NULL;
}

void pci_bus_add_device(pci_bus_t *bus, pci_device_t *dev, int device_num) {
    if (!bus || !dev || device_num < 0 || device_num >= PCI_MAX_DEVICES)
        return;
    if (bus->dev[device_num]) {
        LOG(0, "%s: device %d already seated by '%s'", bus->name, device_num,
            (bus->dev[device_num]->ops && bus->dev[device_num]->ops->name)
                ? bus->dev[device_num]->ops->name(bus->dev[device_num])
                : "?");
        return;
    }
    dev->bus = bus;
    dev->device_num = device_num;
    bus->dev[device_num] = dev;
    pci_device_regions_changed(dev); // a hardwired command register may
                                     // already decode (proposal §6.2)
}

pci_device_t *pci_bus_device(pci_bus_t *bus, int device_num) {
    if (!bus || device_num < 0 || device_num >= PCI_MAX_DEVICES)
        return NULL;
    return bus->dev[device_num];
}

uint32_t pci_bus_cfg_read(pci_bus_t *bus, int dev, uint32_t fn, uint32_t reg) {
    pci_device_t *d = pci_bus_device(bus, dev);
    // Absent device, or a function no multi-function device implements:
    // all-ones.  This is the entire empty-slot model.
    if (!d || fn != 0)
        return 0xFFFFFFFFu;
    return pci_cfg_read(d, reg & 0xFCu);
}

void pci_bus_cfg_write(pci_bus_t *bus, int dev, uint32_t fn, uint32_t reg, uint32_t byte, uint8_t value) {
    pci_device_t *d = pci_bus_device(bus, dev);
    if (!d || fn != 0)
        return; // writes to an absent device vanish
    pci_cfg_write(d, reg & 0xFCu, byte, value);
}

// === Slot table =============================================================

void pci_init(pci_root_t *root, const pci_slot_decl_t *slots) {
    if (!root)
        return;
    root->slots = slots;
}

const pci_slot_decl_t *pci_slot_decl_get(pci_root_t *root, int slot) {
    if (!root || !root->slots)
        return NULL;
    for (const pci_slot_decl_t *s = root->slots; s->slot != 0; s++) {
        if (s->slot == slot)
            return s;
    }
    return NULL;
}

pci_device_t *pci_slot_device(pci_root_t *root, int slot) {
    if (!root || slot < 0 || slot >= PCI_MAX_SLOTS)
        return NULL;
    return root->slot_dev[slot];
}

const pci_card_kind_t *pci_slot_kind(pci_root_t *root, int slot) {
    if (!root || slot < 0 || slot >= PCI_MAX_SLOTS)
        return NULL;
    return root->slot_kind[slot];
}

// Resolve a SOCKET's card id: a staged pick for this exact slot beats the
// wildcard (honoured only on the machine's FIRST socket); both are
// honoured only if the named kind physically fits; the fallback is the
// declared default_card.  Rejections log at level 0 so a bad pick is
// visible by default instead of silently booting the wrong thing.
// *out_explicit reports whether the USER named the winner — the
// built-from record keeps the two apart (machine_config_slot_card_t).
static const char *socket_card_id(const pci_slot_decl_t *s, bool is_first_socket, bool *out_explicit) {
    *out_explicit = false;
    const char *staged = pci_staged_card_get(s->slot);
    if (!staged && is_first_socket)
        staged = pci_staged_card_get(PCI_STAGED_WILDCARD);
    if (staged) {
        if (pci_card_fits_socket(s, pci_card_find(staged))) {
            *out_explicit = true;
            return staged;
        }
        LOG(0, "staged card '%s' does not fit slot %d; using default '%s'", staged, s->slot,
            s->default_card ? s->default_card : "(none)");
    }
    return s->default_card;
}

// Route this slot's staged options into the resolved kind through its own
// stage_option() hook.  A kind that rejects the key logs and the option is
// dropped — the generic layer never learns a card's identity (the fix for
// the NuBus stage_mode_for_kind wart, proposal §5.1).
static void stage_options_for_kind(int slot, const pci_card_kind_t *kind) {
    for (int i = 0; i < PCI_STAGED_OPTS; i++) {
        const char *key = s_staged[slot].opt[i].key;
        const char *value = s_staged[slot].opt[i].value;
        if (!key[0])
            continue;
        if (kind->stage_option && kind->stage_option(key, value))
            continue;
        LOG(0, "staged option '%s'='%s' is not understood by slot %d card '%s' — ignored", key, value, slot, kind->id);
    }
}

void pci_seat_slots(pci_root_t *root, checkpoint_t *cp) {
    if (!root)
        return;
    if (root->slots) {
        // The machine's first SOCKET — the slot the WILDCARD staged entry
        // (machine.boot's pci_card=) applies to.
        int first_socket = -1;
        for (const pci_slot_decl_t *s = root->slots; s->slot != 0; s++) {
            if (s->kind == PCI_SLOT_SOCKET) {
                first_socket = s->slot;
                break;
            }
        }
        for (const pci_slot_decl_t *s = root->slots; s->slot != 0; s++) {
            const pci_card_kind_t *kind = NULL;
            bool explicit_pick = false; // did the USER name this card?
            switch (s->kind) {
            case PCI_SLOT_BUILTIN:
                kind = pci_card_find(s->builtin_card_id);
                break;
            case PCI_SLOT_SOCKET:
                kind = pci_card_find(socket_card_id(s, s->slot == first_socket, &explicit_pick));
                break;
            case PCI_SLOT_ABSENT:
                continue;
            }
            if (!kind || !kind->factory)
                continue;
            if (s->slot < 0 || s->slot >= PCI_MAX_SLOTS) {
                LOG(0, "slot %d out of range; ignored", s->slot);
                continue;
            }
            // Resolve the bus BEFORE building anything, so a mis-declared
            // slot cannot leak a device nothing owns.
            pci_bus_t *bus = pci_bus_by_index(root, s->bus);
            if (!bus) {
                LOG(0, "slot %d names bus %d, which this machine does not have", s->slot, s->bus);
                continue;
            }
            stage_options_for_kind(s->slot, kind);
            pci_device_t *dev = kind->factory(s->slot, root->cfg, cp);
            if (!dev) {
                LOG(1, "slot %d card factory '%s' returned NULL", s->slot, kind->id);
                continue;
            }
            dev->slot_index = s->slot;
            pci_bus_add_device(bus, dev, s->device);
            if (pci_bus_device(bus, s->device) != dev) {
                // The IDSEL was already taken (a slot table that names one
                // twice): the device is unreachable, so drop it rather than
                // leave a phantom nobody frees.
                LOG(0, "slot %d: device %d on bus %d is already seated; '%s' dropped", s->slot, s->device, s->bus,
                    kind->id);
                if (dev->ops && dev->ops->teardown)
                    dev->ops->teardown(dev, root->cfg);
                free(dev->rom);
                free(dev);
                continue;
            }
            bus->owns[s->device] = true; // our factory made it; we free it
            root->slot_dev[s->slot] = dev;
            root->slot_kind[s->slot] = kind;
            // Seed the interrupt LINE register: early Apple OF publishes
            // AAPL,interrupts and the OSes copy the number into $3C, so
            // the slot table and the header cannot disagree.
            dev->cfg.interrupt_line = (uint8_t)s->int_line;
            // The built-from record captures the RESOLVED pick, so
            // machine.restart rebuilds a multi-card machine faithfully
            // (the staged table is cleared below).
            machine_config_note_slot_card(MC_BUS_PCI, s->slot, kind->id, explicit_pick);
        }
    }
    // Consume the whole staged table so a stale selection can't leak into
    // the next machine.boot (the NuBus rule).
    pci_staged_clear_all();
    pci_objects_build(root);
}

void pci_root_delete(pci_root_t *root) {
    if (!root)
        return;
    // Drop the object-model trees before the devices they read go away
    // (ownership-checked: on checkpoint restore this root may already have
    // been superseded by the new machine's tree).
    pci_objects_teardown_owned(root);
    for (int i = 0; i < root->bus_count; i++) {
        pci_bus_t *bus = root->bus[i];
        for (int d = 0; d < PCI_MAX_DEVICES; d++) {
            pci_device_t *dev = bus->dev[d];
            if (!dev)
                continue;
            if (dev->ops && dev->ops->teardown)
                dev->ops->teardown(dev, bus->cfg);
            if (bus->owns[d]) {
                free(dev->rom);
                free(dev);
            }
            bus->dev[d] = NULL;
        }
        free(bus);
        root->bus[i] = NULL;
    }
    free(root);
}

// === Lifecycle fan-outs =====================================================

// Collect every seated device in canonical (bus index, device number)
// order — the order the positional checkpoint stream and every fan-out
// below walk, so save and restore can never fall out of step.
static int pci_collect(pci_root_t *root, pci_device_t **out, int max) {
    int n = 0;
    for (int b = 0; b < root->bus_count; b++) {
        for (int d = 0; d < PCI_MAX_DEVICES && n < max; d++) {
            if (root->bus[b]->dev[d])
                out[n++] = root->bus[b]->dev[d];
        }
    }
    return n;
}

#define PCI_MAX_TOTAL_DEVICES (PCI_MAX_BUSES * PCI_MAX_DEVICES)

void pci_checkpoint_save(pci_root_t *root, checkpoint_t *cp) {
    if (!root || !cp)
        return;
    pci_device_t *devs[PCI_MAX_TOTAL_DEVICES];
    int n = pci_collect(root, devs, PCI_MAX_TOTAL_DEVICES);
    for (int i = 0; i < n; i++) {
        system_write_checkpoint_data(cp, &devs[i]->cfg, sizeof(devs[i]->cfg));
        if (devs[i]->ops && devs[i]->ops->checkpoint_save)
            devs[i]->ops->checkpoint_save(devs[i], cp);
    }
}

void pci_checkpoint_restore(pci_root_t *root, checkpoint_t *cp) {
    if (!root || !cp)
        return;
    pci_device_t *devs[PCI_MAX_TOTAL_DEVICES];
    int n = pci_collect(root, devs, PCI_MAX_TOTAL_DEVICES);
    for (int i = 0; i < n; i++) {
        system_read_checkpoint_data(cp, &devs[i]->cfg, sizeof(devs[i]->cfg));
        if (devs[i]->ops && devs[i]->ops->checkpoint_restore)
            devs[i]->ops->checkpoint_restore(devs[i], cp);
        // Replay the BAR transitions from the restored latches so the
        // decode is rebuilt without any card code (proposal §5.8).
        pci_device_regions_changed(devs[i]);
    }
}

void pci_reset(pci_root_t *root) {
    if (!root)
        return;
    pci_device_t *devs[PCI_MAX_TOTAL_DEVICES];
    int n = pci_collect(root, devs, PCI_MAX_TOTAL_DEVICES);
    for (int i = 0; i < n; i++) {
        pci_cfg_reset(devs[i]);
        pci_device_regions_changed(devs[i]);
        if (devs[i]->ops && devs[i]->ops->reset)
            devs[i]->ops->reset(devs[i], root->cfg);
    }
    root->slot_irq_mask = 0;
}

void pci_tick_vbl(pci_root_t *root) {
    if (!root)
        return;
    pci_device_t *devs[PCI_MAX_TOTAL_DEVICES];
    int n = pci_collect(root, devs, PCI_MAX_TOTAL_DEVICES);
    for (int i = 0; i < n; i++) {
        if (devs[i]->ops && devs[i]->ops->on_vbl)
            devs[i]->ops->on_vbl(devs[i], root->cfg);
    }
}

display_t *pci_primary_display(pci_root_t *root) {
    pci_device_t *dev = pci_primary_display_card(root);
    return dev ? dev->ops->display(dev) : NULL;
}

pci_device_t *pci_primary_display_card(pci_root_t *root) {
    if (!root)
        return NULL;
    // First slot in declared order whose ops->display() answers.
    for (int i = 0; i < PCI_MAX_SLOTS; i++) {
        pci_device_t *dev = root->slot_dev[i];
        if (dev && dev->ops && dev->ops->display && dev->ops->display(dev))
            return dev;
    }
    return NULL;
}

// === Slot interrupts ========================================================

// Drive a slot's strapped INTA-D line through the machine substrate: the
// bus owns the aggregate, the chipset owns HOW the line reaches the CPU
// (TNT → Grand Central externals 23-25 / 27-29).  pci.c stays
// machine-agnostic — no cfg->machine chipset pokes here.
static void pci_route_slot_irq(config_t *cfg, int slot, bool active) {
    if (cfg && cfg->machine && cfg->machine->substrate->pci_slot_irq)
        cfg->machine->substrate->pci_slot_irq(cfg, slot, active);
}

void pci_assert_irq(pci_device_t *dev) {
    if (!dev || !dev->bus || dev->slot_index <= 0 || dev->slot_index >= PCI_MAX_SLOTS)
        return;
    pci_root_t *root = dev->bus->root;
    root->slot_irq_mask |= (1u << dev->slot_index);
    pci_route_slot_irq(dev->bus->cfg, dev->slot_index, /*active*/ true);
}

void pci_deassert_irq(pci_device_t *dev) {
    if (!dev || !dev->bus || dev->slot_index <= 0 || dev->slot_index >= PCI_MAX_SLOTS)
        return;
    pci_root_t *root = dev->bus->root;
    root->slot_irq_mask &= ~(1u << dev->slot_index);
    pci_route_slot_irq(dev->bus->cfg, dev->slot_index, /*active*/ false);
}
