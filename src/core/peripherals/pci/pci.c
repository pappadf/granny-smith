// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pci.c
// The PCI bus controller: device tables, config dispatch, bridge-window
// decode, the card-kind registry, the slot walk and the lifecycle / interrupt fan-outs.  See pci.h and
// docs/internals/core/peripherals/pci.md.
//
// Nothing here knows about any machine: a family creates a bus per host
// bridge, hands the bus its decode windows, seats its own builtin devices
// and lets the slot walk seat the user's cards.  The two facts that make
// the whole model work are kept verbatim from the hand-rolled Bandit
// model this replaces:
//
//   * an IDSEL with no device registered reads ALL-ONES and swallows
//     writes — a probe must never hang, and
//   * PCI space no device decodes takes a RECOVERABLE transfer error, so
//     Open Firmware and the OSes can probe under a fault catcher.

#include "pci.h"

#include "checkpoint.h"
#include "config_space.h"
#include "log.h"
#include "machine_parts.h"
#include "machine_profile.h" // machine_substrate_t (slot-IRQ routing)
#include "prom.h"
#include "system_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("pci");

// A BUILTIN_FALLBACK stand-in's seat (below).
static void fallback_seat(pci_root_t *root, const pci_slot_decl_t *s);

#define PCI_MAX_BUSES   4
#define PCI_MAX_DEVICES 32 // IDSEL AD11..AD31 (0..10 never exist)
#define PCI_MAX_WINDOWS 4
#define PCI_MAX_SLOTS   16 // slot numbers are 1-based

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
    bool lane_reverse; // the bridge is reversing byte lanes (pci.h)
    // The bridge's master-abort policy (pci_bus_set_abort_policy): NULL =
    // every unclaimed access faults, the Bandit contract.
    bool (*abort_faults)(void *ctx, bool write);
    void *abort_ctx;
};

struct pci_root {
    config_t *cfg;
    const pci_slot_decl_t *slots; // the machine's topology (may be NULL)
    pci_bus_t *bus[PCI_MAX_BUSES];
    int bus_count;
    pci_device_t *slot_dev[PCI_MAX_SLOTS]; // device seated in slot N
    const pci_card_kind_t *slot_kind[PCI_MAX_SLOTS]; // and the kind that made it
    // What the document said about each slot (the card a socket seats, its
    // ROM and options): the root's checkpoint block, so a restore seats
    // exactly these.  A slot it says nothing about has slot == 0.
    slot_opts_t entry[PCI_MAX_SLOTS];
};

// === Card-kind registry =====================================================
//
// One explicit list (no linker-section magic).  Adding a card driver is one
// extern plus one entry.

extern const pci_card_kind_t tnt_control_kind; // machines/tnt/control.c
extern const pci_card_kind_t mach64_gx_kind; // peripherals/pci/cards/mach64gx.c
extern const pci_card_kind_t ati_rage_pro_kind; // ...the beige G3's on-board Rage Pro
extern const pci_card_kind_t cirrus_54m30_kind; // peripherals/pci/cards/cirrus54m30.c
// The Network Server's two fast/wide SCSI controllers.  Two kinds rather
// than one because a factory takes no channel argument and the board's two
// controllers are genuinely distinct devices — different IDSELs, different
// Grand Central lines, different drive bays (cards/sym53c825.c).
extern const pci_card_kind_t sym53c825_ch0_kind;
extern const pci_card_kind_t sym53c825_ch1_kind;
extern const pci_card_kind_t voodoo2_kind; // peripherals/pci/cards/voodoo2.c
extern const pci_card_kind_t voodoo2_webgpu_kind; // ...the same card, rasterised by the host GPU

static const pci_card_kind_t *const g_card_registry[] = {
    &tnt_control_kind,    &mach64_gx_kind,     &cirrus_54m30_kind,
    &sym53c825_ch0_kind,  &sym53c825_ch1_kind, &voodoo2_kind,
    &voodoo2_webgpu_kind, &ati_rage_pro_kind,  NULL,
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
    // A bridge that terminates master aborts quietly (Grackle with TEA/MCP
    // reporting off) still reads all-ones and drops the write; it just
    // does not take the machine check.
    if (w->bus->abort_faults && !w->bus->abort_faults(w->bus->abort_ctx, write))
        return;
    memory_signal_bus_error(w->map_base + offset, write);
}

// The lane reversal (pci.h): an N-byte access at offset o is the access at
// o ^ (8-N) with its bytes reversed.  Applied once, here, before decode, so
// no device model ever sees anything but PCI byte addresses and values.
static inline uint32_t lane_offset(const pci_window_t *w, uint32_t offset, uint32_t size) {
    return w->bus->lane_reverse ? (offset ^ (8u - size)) : offset;
}

static uint8_t window_read8(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 1);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFu;
    }
    return d.iface->read_uint8(d.ctx, d.sub);
}

static uint16_t window_read16(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 2);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFFFu;
    }
    uint16_t v = d.iface->read_uint16(d.ctx, d.sub);
    return w->bus->lane_reverse ? __builtin_bswap16(v) : v;
}

static uint32_t window_read32(void *ctx, uint32_t offset) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 4);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, false);
        return 0xFFFFFFFFu;
    }
    uint32_t v = d.iface->read_uint32(d.ctx, d.sub);
    return w->bus->lane_reverse ? __builtin_bswap32(v) : v;
}

static void window_write8(void *ctx, uint32_t offset, uint8_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 1);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint8(d.ctx, d.sub, value);
}

static void window_write16(void *ctx, uint32_t offset, uint16_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 2);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint16(d.ctx, d.sub, w->bus->lane_reverse ? __builtin_bswap16(value) : value);
}

static void window_write32(void *ctx, uint32_t offset, uint32_t value) {
    const pci_window_t *w = (const pci_window_t *)ctx;
    pci_decode_t d;
    offset = lane_offset(w, offset, 4);
    if (!window_locate(w, window_pci_addr(w, offset), &d)) {
        window_fault(w, offset, true);
        return;
    }
    d.iface->write_uint32(d.ctx, d.sub, w->bus->lane_reverse ? __builtin_bswap32(value) : value);
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

void pci_bus_set_lane_reverse(pci_bus_t *bus, bool on) {
    if (!bus || bus->lane_reverse == on)
        return;
    bus->lane_reverse = on;
    LOG(1, "%s: byte lanes %s", bus->name, on ? "REVERSED (little-endian client)" : "straight (big-endian)");
}

void pci_bus_set_abort_policy(pci_bus_t *bus, bool (*faults)(void *ctx, bool write), void *ctx) {
    if (!bus)
        return;
    bus->abort_faults = faults;
    bus->abort_ctx = ctx;
}

bool pci_bus_lane_reverse(const pci_bus_t *bus) {
    return bus && bus->lane_reverse;
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
                                     // already decode (command_reset)
}

pci_device_t *pci_bus_device(pci_bus_t *bus, int device_num) {
    if (!bus || device_num < 0 || device_num >= PCI_MAX_DEVICES)
        return NULL;
    return bus->dev[device_num];
}

bool pci_bus_is_populated(const pci_bus_t *bus) {
    if (!bus)
        return false;
    for (int i = 0; i < PCI_MAX_DEVICES; i++)
        if (bus->dev[i])
            return true;
    return false;
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

// The card a SOCKET seats: the boot document's entry for it (machine_slots.c
// validated that the card fits), else the declared default_card (NULL = the
// socket ships empty).
static const char *socket_card_id(const pci_slot_decl_t *s, const slot_opts_t *e) {
    if (e && e->empty)
        return NULL;
    if (e && e->card[0])
        return e->card;
    return s->default_card;
}

// Format a refusal into the caller's buffer and say no.
__attribute__((format(printf, 3, 4))) static bool refuse(char *why, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
    return false;
}

bool pci_slot_entry_check(const pci_slot_decl_t *slots, const char *model, const slot_opts_t *e, bool roms_final,
                          char *why, size_t why_len) {
    const pci_slot_decl_t *d = NULL;
    for (const pci_slot_decl_t *s = slots; s && s->slot; s++) {
        if (s->slot == e->slot)
            d = s;
    }
    if (!d || d->kind == PCI_SLOT_ABSENT)
        return refuse(why, why_len, "PCI slot %d on model '%s' takes no card", e->slot, model);
    const pci_card_kind_t *k = NULL;
    bool named = false;
    if (e->empty) {
        if (d->kind != PCI_SLOT_SOCKET)
            return refuse(why, why_len, "PCI slot %d on model '%s' is built in and cannot be emptied", e->slot, model);
    } else if (e->card[0]) {
        k = pci_card_find(e->card);
        if (!k) {
            const char *near = pci_card_suggest(e->card);
            if (near)
                return refuse(why, why_len, "unknown card id '%s' — did you mean '%s'? (see catalog.pci_cards)",
                              e->card, near);
            return refuse(why, why_len, "unknown card id '%s' (see catalog.pci_cards)", e->card);
        }
        bool fits = d->kind == PCI_SLOT_SOCKET ? pci_card_fits_socket(d, k)
                                               : (d->builtin_card_id && strcmp(d->builtin_card_id, k->id) == 0);
        if (!fits)
            return refuse(why, why_len, "card '%s' fits no slot on model '%s' (see catalog.profile(\"%s\").pci_slots)",
                          e->card, model, model);
        named = d->kind == PCI_SLOT_SOCKET;
    } else {
        k = pci_card_find(d->kind == PCI_SLOT_SOCKET ? d->default_card : d->builtin_card_id);
    }
    if (e->video_mode[0] || e->custom_mode[0])
        return refuse(why, why_len, "PCI slot %d takes no video mode (a PCI display card senses its monitor)", e->slot);
    for (int i = 0; i < e->n_options; i++) {
        if (!k || !k->accepts_option || !k->accepts_option(e->options[i].key, e->options[i].value))
            return refuse(why, why_len, "slot %d's card '%s' does not take option %s=%s", e->slot, k ? k->id : "(none)",
                          e->options[i].key, e->options[i].value);
    }
    if (e->rom[0]) {
        prom_id_t pid;
        if (!prom_identify_card(e->rom, &pid))
            return refuse(why, why_len,
                          "prom '%s' is not a recognised PCI expansion ROM (see catalog.proms.identify for what it is "
                          "instead)",
                          e->rom);
        if (!k || strcmp(pid.card_id, k->id) != 0)
            return refuse(why, why_len, "prom '%s' is for card '%s', not slot %d's '%s'", e->rom, pid.card_id, e->slot,
                          k ? k->id : "(none)");
    }
    if (roms_final && named && k && k->requires_prom && !prom_card_resolvable(k->id, e->rom[0] ? e->rom : NULL))
        return refuse(why, why_len,
                      "card '%s' (PCI slot %d) needs a PCI expansion ROM but no offered .prom file provides it", k->id,
                      e->slot);
    return true;
}

// A device's checkpoint part, opened before it is built and closed after
// (defined with the part's save, below).
static void device_part_open(config_t *cfg, checkpoint_t *cp, const char *name, rom_image_t *rom);
static void device_part_close(config_t *cfg, checkpoint_t *cp, pci_device_t *dev, const char *name);

// A restore's slot table is a file the user supplied: every entry that seats
// a card goes through the checks machine.boot applies to a document's, bar
// the PROM file (the card's PROM comes from its own block).  A bad entry
// fails the restore, naming the slot.
static bool slot_table_valid(slot_opts_t *entry, const pci_slot_decl_t *slots, const char *model) {
    for (int n = 0; n < PCI_MAX_SLOTS; n++) {
        slot_opts_t *e = &entry[n];
        if (!slot_opts_sanitize(e, n)) {
            LOG(0, "Error: the checkpoint's PCI slot %d entry is malformed", n);
            return false;
        }
        if (!e->card[0])
            continue;
        slot_opts_t check = *e;
        check.rom[0] = '\0';
        char why[256];
        if (!pci_slot_entry_check(slots, model, &check, false, why, sizeof why)) {
            LOG(0, "Error: the checkpoint's PCI slot %d: %s", n, why);
            return false;
        }
    }
    return true;
}

static void pci_slots_part_save(void *obj, checkpoint_t *cp) {
    pci_root_t *root = obj;
    system_write_checkpoint_data(cp, root->entry, sizeof(root->entry), "pci");
}

// The document's entry for `slot`, or NULL when it said nothing about it.
static const slot_opts_t *slot_entry(const pci_root_t *root, int slot) {
    if (slot < 0 || slot >= PCI_MAX_SLOTS || root->entry[slot].slot != slot || !slot)
        return NULL;
    return &root->entry[slot];
}

void pci_seat_slots(pci_root_t *root, checkpoint_t *cp) {
    if (!root)
        return;
    // A boot takes the slot entries from the document; a restore, from the
    // root's own block.
    machine_part_begin(root->cfg, cp, "pci");
    if (cp) {
        system_read_checkpoint_data(cp, root->entry, sizeof(root->entry), "pci");
        if (!slot_table_valid(root->entry, root->slots, root->cfg->machine->id))
            checkpoint_set_error(cp);
    } else {
        for (int i = 0; i < root->cfg->build_opts.n_slots; i++) {
            const slot_opts_t *e = &root->cfg->build_opts.slots[i];
            if (e->slot > 0 && e->slot < PCI_MAX_SLOTS)
                root->entry[e->slot] = *e;
        }
        // A built-in video's monitor is an argument of its construction, so
        // its slot entry carries the build's sense and connection (as the
        // NuBus seats do), and the table's block gives them back on a restore.
        for (const pci_slot_decl_t *s = root->slots; s && s->slot != 0; s++) {
            if (s->kind != PCI_SLOT_BUILTIN || s->slot <= 0 || s->slot >= PCI_MAX_SLOTS)
                continue;
            root->entry[s->slot].slot = s->slot;
            root->entry[s->slot].sense = root->cfg->build_opts.builtin_sense;
            root->entry[s->slot].connected = root->cfg->build_opts.builtin_connected;
        }
    }
    machine_part(root->cfg, cp, "pci", pci_slots_part_save, root);
    if (root->slots) {
        // Which card classes the SOCKETS will supply.  Resolved in a first
        // pass so a BUILTIN_FALLBACK can stand down before it is built —
        // the machine's slot table lists the fallback last, but a socket
        // card must win regardless of declaration order.
        const char *socket_classes[PCI_MAX_SLOTS];
        int n_socket_classes = 0;
        for (const pci_slot_decl_t *s = root->slots; s->slot != 0; s++) {
            if (s->kind != PCI_SLOT_SOCKET)
                continue;
            const pci_card_kind_t *k = pci_card_find(socket_card_id(s, slot_entry(root, s->slot)));
            if (k && k->card_class && n_socket_classes < PCI_MAX_SLOTS)
                socket_classes[n_socket_classes++] = k->card_class;
        }

        for (const pci_slot_decl_t *s = root->slots; s->slot != 0; s++) {
            const pci_card_kind_t *kind = NULL;
            const slot_opts_t *entry = slot_entry(root, s->slot);
            switch (s->kind) {
            case PCI_SLOT_BUILTIN:
                kind = pci_card_find(s->builtin_card_id);
                break;
            case PCI_SLOT_BUILTIN_FALLBACK: {
                kind = pci_card_find(s->builtin_card_id);
                if (!kind || !kind->card_class)
                    break;
                for (int i = 0; i < n_socket_classes; i++) {
                    if (strcmp(socket_classes[i], kind->card_class) != 0)
                        continue;
                    LOG(1, "slot %d: '%s' stands down — a socket supplies a '%s' card", s->slot, kind->id,
                        kind->card_class);
                    kind = NULL;
                    break;
                }
                if (kind && !cp)
                    fallback_seat(root, s);
                break;
            }
            case PCI_SLOT_SOCKET:
                kind = pci_card_find(socket_card_id(s, entry));
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
            // Each seated card is a part of its own, after the slot table: on a
            // restore its block gives the expansion ROM it runs, which the
            // factory takes, then its config header and state.
            char part[32];
            snprintf(part, sizeof part, "pci.slot.%d", s->slot);
            rom_image_t rom;
            device_part_open(root->cfg, cp, part, &rom);
            // A slot the document says nothing about builds with the card's
            // defaults.
            slot_opts_t none = {.slot = s->slot};
            pci_device_t *dev = kind->factory(s->slot, root->cfg, rom.data ? &rom : NULL, entry ? entry : &none);
            free((void *)rom.data);
            if (!dev) {
                LOG(1, "slot %d card factory '%s' returned NULL", s->slot, kind->id);
                if (cp) {
                    LOG(0, "Error: the checkpoint's PCI slot %d card '%s' could not be built", s->slot, kind->id);
                    checkpoint_set_error(cp);
                }
                machine_part_cancel(root->cfg);
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
                machine_part_cancel(root->cfg);
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
            device_part_close(root->cfg, cp, dev, part);
        }
    }
    // machine.pci's tree is built by the swap step (system_swap_in), like
    // machine.nubus's.
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

// The largest expansion ROM a device's block may carry.
#define DEVICE_ROM_MAX (256u * 1024u)

// A device's block: its expansion ROM (none for most), its config header,
// then its own state.  The ROM is always in the block, so a checkpoint
// restores the card it was saved with whatever PROM files the host offers now.
static void pci_device_part_save(void *obj, checkpoint_t *cp) {
    pci_device_t *dev = obj;
    uint32_t size = dev->rom ? (uint32_t)dev->rom_size : 0;
    system_write_checkpoint_data(cp, &size, sizeof size, "pci.rom");
    if (size)
        system_write_checkpoint_data(cp, dev->rom, size, "pci.rom");
    system_write_checkpoint_data(cp, &dev->cfg, sizeof(dev->cfg), "pci.cfg");
    if (dev->ops && dev->ops->checkpoint_save)
        dev->ops->checkpoint_save(dev, cp);
}

// Open the part `name` and, on a restore, read the expansion ROM its block
// begins with into *rom (data owned by the caller; size 0 for none).
static void device_part_open(config_t *cfg, checkpoint_t *cp, const char *name, rom_image_t *rom) {
    *rom = (rom_image_t){0};
    machine_part_begin(cfg, cp, name);
    if (!cp)
        return;
    uint32_t size = 0;
    system_read_checkpoint_data(cp, &size, sizeof size, "pci.rom");
    if (checkpoint_has_error(cp) || !size)
        return;
    if (size > DEVICE_ROM_MAX) {
        LOG(0, "Error: the checkpoint's '%s' expansion ROM is %u bytes (at most %u)", name, size, DEVICE_ROM_MAX);
        checkpoint_set_error(cp);
        return;
    }
    uint8_t *data = malloc(size);
    if (!data) {
        checkpoint_set_error(cp);
        return;
    }
    system_read_checkpoint_data(cp, data, size, "pci.rom");
    *rom = (rom_image_t){.data = data, .size = size};
}

// Read the rest of a built device's block -- its config header and state --
// and register it as the open part.
static void device_part_close(config_t *cfg, checkpoint_t *cp, pci_device_t *dev, const char *name) {
    if (cp) {
        system_read_checkpoint_data(cp, &dev->cfg, sizeof(dev->cfg), "pci.cfg");
        if (dev->ops && dev->ops->checkpoint_restore)
            dev->ops->checkpoint_restore(dev, cp);
    }
    machine_part(cfg, cp, name, pci_device_part_save, dev);
}

void pci_device_part(config_t *cfg, checkpoint_t *cp, pci_device_t *dev, const char *name) {
    rom_image_t rom;
    device_part_open(cfg, cp, name, &rom);
    if (rom.data) {
        // A board chip carries no expansion ROM; a block that says otherwise
        // was not written by this machine.
        LOG(0, "Error: the checkpoint gives board device '%s' an expansion ROM", name);
        checkpoint_set_error(cp);
        free((void *)rom.data);
    }
    device_part_close(cfg, cp, dev, name);
}

void pci_replay_decode(pci_root_t *root) {
    if (!root)
        return;
    pci_device_t *devs[PCI_MAX_TOTAL_DEVICES];
    int n = pci_collect(root, devs, PCI_MAX_TOTAL_DEVICES);
    for (int i = 0; i < n; i++)
        pci_device_regions_changed(devs[i]);
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

// A stand-in that seats takes the place of the display card it stands in for:
// that card's monitor (the first socket's default card's first monitor), and
// the connection when no other display device has it.
static void fallback_seat(pci_root_t *root, const pci_slot_decl_t *s) {
    slot_opts_t *e = &root->entry[s->slot];
    bool taken = root->cfg->build_opts.builtin_connected;
    for (int i = 0; i < PCI_MAX_SLOTS; i++)
        taken |= i != s->slot && root->entry[i].connected;
    const pci_card_kind_t *stood_for = NULL;
    for (const pci_slot_decl_t *x = root->slots; x->slot && !stood_for; x++)
        if (x->kind == PCI_SLOT_SOCKET && x->default_card)
            stood_for = pci_card_find(x->default_card);
    e->slot = s->slot;
    e->sense = (stood_for && stood_for->monitors && stood_for->monitors->id) ? stood_for->monitors->sense_code
                                                                             : MACHINE_SENSE_NONE;
    e->connected = !taken;
}

pci_device_t *pci_connected_display_card(pci_root_t *root) {
    // A pass-through 3D card (the Voodoo2) sits between the display card and
    // the monitor and answers display() only while it holds the output:
    // then the monitor shows it, whichever device it is connected to.
    for (int i = 0; root && i < PCI_MAX_SLOTS; i++) {
        pci_device_t *dev = root->slot_dev[i];
        const pci_card_kind_t *k = pci_slot_kind(root, i);
        if (dev && k && k->card_class && strcmp(k->card_class, "3d") == 0 && dev->ops && dev->ops->display &&
            dev->ops->display(dev))
            return dev;
    }
    for (int i = 0; root && i < PCI_MAX_SLOTS; i++) {
        pci_device_t *dev = root->slot_dev[i];
        if (dev && root->entry[i].connected && dev->ops && dev->ops->display)
            return dev;
    }
    return NULL;
}

display_t *pci_connected_display(pci_root_t *root) {
    pci_device_t *dev = pci_connected_display_card(root);
    return dev ? dev->ops->display(dev) : NULL;
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

// Like the NuBus side, the PCI root keeps NO aggregate of asserted slot
// lines: the chipset owns the OR, and the mask here was maintained and
// checkpointed but read by nothing.  See the note above nubus_assert_irq
// before reintroducing it.
void pci_assert_irq(pci_device_t *dev) {
    if (!dev || !dev->bus || dev->slot_index <= 0 || dev->slot_index >= PCI_MAX_SLOTS)
        return;
    pci_route_slot_irq(dev->bus->cfg, dev->slot_index, /*active*/ true);
}

void pci_deassert_irq(pci_device_t *dev) {
    if (!dev || !dev->bus || dev->slot_index <= 0 || dev->slot_index >= PCI_MAX_SLOTS)
        return;
    pci_route_slot_irq(dev->bus->cfg, dev->slot_index, /*active*/ false);
}
