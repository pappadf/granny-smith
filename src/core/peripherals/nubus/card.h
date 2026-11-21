// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// card.h
// NuBus card abstraction shared by every card driver in cards/.

#ifndef NUBUS_CARD_H
#define NUBUS_CARD_H

#include "checkpoint.h"
#include "common.h"
#include "gsvrom.h" // gsvrom_personality_t, for the substitute the bus seats
#include "machine_build_opts.h" // slot_opts_t
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

struct nubus_bus;
struct nubus_card;
struct object;
typedef struct nubus_bus nubus_bus_t;
typedef struct nubus_card nubus_card_t;

// Per-card vtable.  Cards implement whichever entry-points they need;
// NULL hooks are safe and skipped by the bus controller.
typedef struct nubus_card_ops {
    // Called once during machine init.  Returns 0 on success.  The card
    // may allocate VRAM, register host-backed regions on the bus map, and
    // populate any internal state.  `opts` is what the boot document says
    // about this slot (never NULL; empty fields mean the card's defaults):
    // its video mode, custom geometry, declaration-ROM file, whether it runs
    // the substitute ROM, and the monitor on its connector.
    int (*init)(nubus_card_t *card, config_t *cfg, checkpoint_t *cp, const slot_opts_t *opts);

    // Called during machine teardown, in inverse init order.
    void (*teardown)(nubus_card_t *card, config_t *cfg);

    // Called when the CPU asserts the bus /RESET line (the 68k RESET
    // instruction; see nubus_reset / system_reset_devices).  The card
    // returns its externally-visible registers to their power-on state,
    // exactly as the /RESET pin does in silicon.  VRAM / declaration-ROM
    // contents and the host memory-map regions persist (they are NOT
    // re-allocated or re-registered).  NULL hooks are safe and skipped.
    void (*reset)(nubus_card_t *card, config_t *cfg);

    // Called on a power cycle (machine.restart), before the /RESET that
    // follows it: the card loses what losing power loses -- VRAM -- and
    // comes up showing what a cold card shows.  A warm /RESET (above) keeps
    // VRAM.  NULL takes the bus default (nubus_power_on): /RESET, zero the
    // VRAM `vram` reports, and blank the raster -- a card overrides it only
    // for state beyond that.
    void (*power_on)(nubus_card_t *card, config_t *cfg);

    // The card's own VRAM, for the power-cycle default: its size, with the
    // buffer in *base.  NULL (or 0) for a card with none, or whose
    // framebuffer is the machine's main RAM.
    size_t (*vram)(nubus_card_t *card, uint8_t **base);

    // Called from the family VBL trigger (via nubus_tick_vbl) once per
    // VBL.  Cards that drive their own VSync IRQ call nubus_assert_irq()
    // here.
    void (*on_vbl)(nubus_card_t *card, config_t *cfg);

    // Optional: present this card's framebuffer as the primary display
    // surface.  Returns NULL if the card has no display.  The pointer is
    // stable across the card's lifetime; the descriptor's contents are
    // live-mutable (see display.h for the dirty-flag contract).
    display_t *(*display)(nubus_card_t *card);

    // Checkpoint hooks; the bus controller calls these for every populated
    // slot in canonical slot order.
    void (*checkpoint_save)(nubus_card_t *card, checkpoint_t *cp);
    void (*checkpoint_restore)(nubus_card_t *card, checkpoint_t *cp);
} nubus_card_ops_t;

// Concrete card instance.  Per-card private state hangs off `.private`;
// the bus controller never reads it.  `declrom` / `declrom_size` are
// optional — a card without a declaration ROM (e.g. a hypothetical SCSI
// expansion card) leaves them NULL/0.
struct nubus_card {
    const nubus_card_ops_t *ops;
    int slot; // $9..$E
    nubus_bus_t *bus; // owning bus (back-pointer)
    void *priv; // card-private state (named "priv"
                // because `private` is a C++ keyword
                // that snags some tooling).
    uint8_t *declrom; // 8 KB or larger declaration ROM
    size_t declrom_size;
    // Where the declaration ROM came from: its file, or "builtin:<kind>" for
    // a generated one (NULL: none loaded), and its Format-Block CRC.  Set by
    // declrom_load_vrom_card / declrom_install_builtin; freed by the bus.
    char *rom_path;
    uint32_t rom_crc;
    // The chip image of a file-backed ROM (NULL for a generated one), kept so
    // the card's checkpoint part carries the ROM it runs.  Freed by the bus.
    uint8_t *rom_chip;
    size_t rom_chip_size;
    // On a restore, the declaration ROM the card's checkpoint part carries,
    // set by the bus before ops->init: declrom_load_vrom_card takes it rather
    // than looking the card up again (the file may be gone, or another
    // revision offered).  Borrowed for init only.
    rom_image_t restored_rom;
    // The card runs the emulator's substitute declaration ROM (its seat's
    // `substitute`, set by the bus before ops->init).
    bool substitute;
};

// (The per-card factory is gone.  The bus controller allocates the
// nubus_card_t itself and calls ops->init on it -- see
// nubus_card_kind_t.ops.)

// One monitor a card advertises (resolution + supported depths).  Used
// by the card-kind registry so the dialog can populate a monitor / depth
// dropdown without knowing about the card driver.  Sentinel-terminated
// arrays end at the entry whose `id` is NULL.
//
// `sense_code` is the value the card's sense lines report when this
// monitor is plugged in: the bus gives a seat the row of the monitor the
// document plugs into the card (slot_opts_t.monitor) and its code, and the
// card models that monitor.  `srsrc_sister` is the top-level "Ax" sister
// sResource ID that the card's driver picks up from its slot PRAM record for
// this monitor, which the seeding step writes from a chosen video mode
// (nubus_card_kind_t.startup_record).
//
// The same rows serve a PCI display card (pci_card_kind_t.monitors) and the
// built-in video of a PCI machine: one row per monitor the card drives, its
// id the monitor_catalog id, carrying what the card's sense hardware needs
// (`ext_sense`, `ddc`).  slot_opts_t.monitor names a row (or "none"), and
// card_monitor_check (card_registry.h) is the one check of it for both buses.
typedef struct nubus_monitor {
    const char *id; // "13in_rgb" -- the card's own token, what a mode id names
    const char *monitor; // the shared catalogue id (monitor_catalog.h)
    uint32_t width; // pixels
    uint32_t height; // pixels
    const int *depths; // 0-terminated array of supported bpp values
    // 0..7 — the 3-bit code the card's monitor-sense lines read for this
    // monitor (Apple's standard sense codes).  7 (all lines high) is the
    // "nothing attached / extended sense" code, so no real monitor row uses
    // it; uint8_t because only the low three bits exist.
    uint8_t sense_code;
    uint8_t srsrc_sister; // top-level Ax sister sResource ID (savedSRsrcID)
    // Apple's 6-bit extended sense code, for a card that walks the extended
    // probe (drive one line, read the other two): (pin1,pin0) << 4 |
    // (pin2,pin0) << 2 | (pin2,pin1).  0 means the monitor has no tie
    // resistors and answers with its strap alone (card_monitor_ext_sense
    // derives it -- for sense code 0 that is $00 too).
    uint8_t ext_sense;
    // The monitor answers DDC with an EDID, which a VGA-cable card publishes.
    bool ddc;
    // CRT response curve (physical phosphor / gamma response) that the
    // monitor on the far end of the cable would apply.  Mac System 7's
    // video driver gamma-pre-corrects CLUT writes for THIS specific
    // monitor; on real hardware the CRT's response cancels the
    // pre-correction, so the user sees a neutral image.  In software
    // we apply the response ourselves at display time.  NULL means
    // "identity" (display the card's CLUT output directly — fine for
    // monitors whose gamma table happens to be near-identity, like
    // the 13" and 12" RGB Apple-Color displays).  Non-NULL is a
    // pointer to a 3 × 256 byte table, R/G/B order; see display_t::
    // crt_response for the on-display contract.
    const uint8_t (*crt_response)[256];
} nubus_monitor_t;

// What physical connector (if any) a card kind attaches through.  This is
// a card-intrinsic fact: motherboard circuitry is BUILTIN wherever it
// exists; a genuine NuBus card fits any NuBus socket on any machine, per
// the bus standard.  Machine × card compatibility is COMPUTED by matching
// this against the machine's slot table (nubus_card_fits_socket) — machines
// never enumerate cards.
// BUILTIN is deliberately 0 so a kind that forgets to declare its
// attachment is conservatively excluded from every socket rather than
// wrongly offered everywhere.
typedef enum card_attach {
    CARD_ATTACH_BUILTIN = 0, // soldered-down pseudo-slot device (SE/30 / RBV
                             // video); only instantiable via a BUILTIN slot
                             // decl naming it — never offered on sockets
    CARD_ATTACH_NUBUS, // standard NuBus connector — universal
} card_attach_t;

// Per-card driver descriptor — one static instance per registered driver.
// The dialog reads this via catalog.nubus_cards; the bus controller reads it
// via nubus_card_find() to resolve a card id to a factory.
typedef struct nubus_card_kind {
    const char *id; // "mdc_8_24"
    const char *display_name; // "Apple Macintosh Display Card 8•6 / 8•24"
    card_attach_t attach; // physical attachment; drives socket matching
    bool requires_vrom; // needs its real declaration ROM (a .vrom file)
    // The emulator can generate a declaration ROM for this card (the GS
    // vROM, docs/internals/core/peripherals/nubus_generic_vrom.md), which the
    // card runs when Apple's is not offered, when the slot asks for it
    // (`rom=substitute`), or for a custom geometry.  Same card, same model;
    // only the ROM differs.
    bool substitute;
    const nubus_monitor_t *monitors; // sentinel-terminated; NULL for non-display cards
    // The monitors the substitute ROM drives, when fewer than the card's
    // (the 8•24 GC's carries only its 640 × 480 configuration); NULL: all.
    const nubus_monitor_t *substitute_monitors;
    // The substitute's monitors when the slot changes them (JMFB's custom
    // geometry): fills `scratch` (NUBUS_SUBST_MONITORS_MAX entries, sentinel
    // included) and returns it, or returns NULL for the lists above.
    const nubus_monitor_t *(*substitute_monitors_for)(const slot_opts_t *opts, nubus_monitor_t *scratch);

    // The declaration ROM the bus seats before ops->init (declrom_seat):
    // the size of its bus window, 0 for a card that seats none.  The bus
    // allocates card->declrom, fills it (Apple's ROM, or the substitute
    // generated with `gsvrom`), and frees it; the card only reads it.
    uint32_t declrom_bus_size;
    gsvrom_personality_t gsvrom;
    // Apple's ROM failing to load runs the substitute instead of leaving the
    // slot empty: a board's built-in video (the SE/30's) cannot be unseated.
    bool declrom_falls_back;
    // The card's vtable.  The bus controller allocates the nubus_card_t,
    // fills in ops / bus / slot, and calls ops->init once per populated slot.
    //
    // This used to be a per-card `factory` that allocated the card itself --
    // which meant `bus` could only be assigned AFTER the factory returned, so
    // nubus_assert_irq / nubus_deassert_irq reached from card_init were a
    // silent no-op (they early-return on !card->bus).  No card did that, but
    // card_reset legitimately does, and the two call sites look identical.
    // Nine kinds also carried five byte-identical `factory_common` bodies to
    // do the allocation.
    const nubus_card_ops_t *ops;
    // Can the kind build at a w x h x d custom geometry (custom_mode=)?  A
    // kind whose substitute ROM can carry one answers; NULL for a kind with
    // no custom geometry.  The boot document is checked against it
    // before the running machine is touched, so init never sees a geometry
    // the card cannot build.  On false *why is a static reason.
    bool (*custom_mode_fits)(uint32_t w, uint32_t h, uint32_t d, const char **why);

    // The startup-mode record for the seeding step: the 8-byte slot
    // PRAM record (sPRAMRec, at $46 + (slot - 9) * 8) a Monitors control
    // panel would have saved for entry `e`'s video mode -- the card knows its
    // format, the machine writes it.  False when the entry chose no mode.
    // NULL for a kind with no startup mode.
    bool (*startup_record)(const slot_opts_t *e, uint8_t rec[8]);

    // Attach this kind's OWN object children under the generic card node.
    // The same seam PCI has: a card's private nodes belong to the card, not
    // to a core file testing `is_card()` on every seated slot.  NULL for a
    // kind with nothing card-specific to expose.  Children attached here are
    // freed with the slot's tree; the callee keeps no handle.
    void (*attach_objects)(struct nubus_card *card, struct object *card_node);
} nubus_card_kind_t;

// Parse a "monitor_Nbpp" video-mode id against a kind's monitor catalogue.
// One body for what were three byte-identical copies in jmfb.c, 24ac.c and
// 824gc.c.  The monitor portion is matched case-sensitively
// against `list`; N is decimal and must appear in that monitor's depths[].
// Returns false (leaving the outputs untouched) on any mismatch.
bool nubus_monitor_mode_lookup(const nubus_monitor_t *list, const char *id, const nubus_monitor_t **out_monitor,
                               int *out_depth_bpp);

// The Slot Manager's spDepth for a pixel depth: $80 + log2(bpp) for the
// 1/2/4/8/16/32 bpp modes, as the startup-mode record's savedMode byte
// carries it (docs/reference/formats/mac-pram.md §6).  `fallback` answers
// any other depth (no catalogue row offers one).  The 24AC numbers its modes
// its own way (savedmode_for_bpp) and does not use this.
static inline uint8_t nubus_spdepth_for_bpp(int bpp, uint8_t fallback) {
    switch (bpp) {
    case 1:
        return 0x80;
    case 2:
        return 0x81;
    case 4:
        return 0x82;
    case 8:
        return 0x83;
    case 16:
        return 0x84;
    case 32:
        return 0x85;
    default:
        return fallback;
    }
}

// The widest video-mode id any catalogue can name, plus room for the "_32bpp"
// suffix and the terminator.  One size for what were a 32-byte buffer in one
// card and 40-byte buffers in the other two.
#define NUBUS_VIDEO_MODE_ID_MAX 40

// Most entries a substitute_monitors_for scratch list holds, sentinel included.
#define NUBUS_SUBST_MONITORS_MAX 8

// Registry accessors.  The registry itself is an explicit, NULL-terminated
// list in nubus.c -- no linker constructors.  nubus_card_find returns NULL
// for an unknown id.
const nubus_card_kind_t *nubus_card_find(const char *id);
const nubus_card_kind_t *const *nubus_card_registry(void);

// For unknown-id error messages: the registered id that differs from `id`
// only by underscores (e.g. "824gc" for a mistyped "8_24_gc"), or NULL.
const char *nubus_card_suggest(const char *id);

#endif // NUBUS_CARD_H
