// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa.c
// Apple Lisa 2 machine implementation (and, later, the Macintosh XL variant).
//
// The Lisa is the first non-Mac machine: a 68000 with a custom segment MMU
// (lisa_mmu.c), the COPS keyboard/mouse/clock microcontroller, an intelligent
// 6504A floppy controller, and a parallel-port hard disk — none of which are
// Mac architecture.  See docs/machines/lisa/lisa.md for the hardware reference.
//
// This first cut (Step 2) is intentionally minimal: 68000 + RAM + 16 KB boot
// ROM + the segment MMU, enough to run the power-on self-tests headlessly.
// Video, COPS, floppy, and the parallel disk arrive in later steps.

#include "machine.h"
#include "system_config.h"

#include "cops.h"
#include "cpu.h"
#include "debug.h"
#include "display.h"
#include "image.h"
#include "lisa_fdc.h"
#include "lisa_mmu.h"
#include "lisa_profile.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "scc.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"
#include "via.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("lisa");

// The two 6522 VIAs use Lisa register strides (VIA1 = 2, VIA2 = 8) rather than
// the Mac's 0x200; via.c selects its register from address bits 9-12, so this
// adapter remaps a Lisa byte offset into the (reg << 9) form via.c expects.
typedef struct lisa_via_port {
    via_t *via; // the VIA instance
    const memory_interface_t *vif; // via.c's own memory interface
    uint32_t reg_shift; // offset >> reg_shift = register number (1 for VIA1, 3 for VIA2)
} lisa_via_port_t;

// Lisa-specific peripheral state (reached via config_t.machine_context).
typedef struct lisa_state {
    lisa_mmu_t *mmu; // custom segment MMU
    cops_t *cops; // keyboard/mouse/clock/power microcontroller on VIA1 port A
    lisa_fdc_t *fdc; // intelligent 6504A floppy controller (shared RAM @ $C001)
    lisa_profile_t *profile; // ProFile parallel hard disk on VIA2
    bool via1_pb7; // last VIA1 PB7 (CRES/) level, for edge-detecting ProFile reset
    // Level 1 is shared by VIA2 (parallel/floppy demux) and the video VBL
    // (docs/machines/lisa/lisa.md §7.1/§12).  Track each sub-source so deasserting one does
    // not clear the other when recomputing the CPU IPL.
    bool l1_via2; // VIA2 IRQ line currently asserted
    bool l1_vbl; // video vertical-retrace interrupt currently asserted
    bool l1_floppy; // floppy FDIR (RWTS-complete / disk-insert / eject) asserted
    lisa_via_port_t via1_map; // VIA1 (keyboard / COPS), base $00DD81, stride 2
    lisa_via_port_t via2_map; // VIA2 (parallel disk / contrast), $D800-$D9FF window, stride 8
    display_t display; // 720x364 1bpp framebuffer (direct, in main RAM)
    struct object *fd_obj, *fd_drives_obj, *fd_drive_obj; // `floppy` object tree
    struct object *hd_obj; // `profile` object (parallel hard disk)
    struct object *power_obj; // `power` object (soft power-off switch)
} lisa_state_t;

// Video geometry, 1 bpp MSB-first (docs/machines/lisa/lisa.md §8).  The unmodified Lisa 2 has
// a 720x364 rectangular-pixel raster; the Macintosh XL screen modification that
// MacWorks XL targets is a 608x431 square-pixel raster.  Same framebuffer (~32
// KB at the $E800 video latch base) — only the scan geometry differs.
#define LISA_SCREEN_W  720
#define LISA_SCREEN_H  364
#define MACXL_SCREEN_W 608
#define MACXL_SCREEN_H 431

static inline lisa_state_t *lisa_state(config_t *cfg) {
    return (lisa_state_t *)cfg->machine_context;
}

// ============================================================
// Video (direct 1bpp framebuffer in main RAM, base from the $E800 latch)
// ============================================================

// Point the display at the current framebuffer, which the Video Address Latch
// relocates anywhere in RAM (docs/machines/lisa/lisa.md §8).  Re-read each frame so a latch
// write (the ROM moves the screen during sizing) takes effect.  Marks the
// framebuffer dirty only when the base actually moves.
static void lisa_refresh_framebuffer(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    uint32_t base = lisa_mmu_video_base(ls->mmu);
    const uint8_t *bits = ram_native_pointer(cfg->mem_map, base);
    ls->display.fb_dirty = true; // contents change every frame
    if (ls->display.bits != bits) {
        ls->display.bits = bits;
        ls->display.shape_dirty = true;
    }
}

static void lisa_display_init(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    bool macxl = cfg->machine && cfg->machine->id && strcmp(cfg->machine->id, "macxl") == 0;
    ls->display.width = macxl ? MACXL_SCREEN_W : LISA_SCREEN_W;
    ls->display.height = macxl ? MACXL_SCREEN_H : LISA_SCREEN_H;
    ls->display.stride = ls->display.width / 8;
    ls->display.format = PIXEL_1BPP_MSB;
    // Pixel aspect ratio (display.h).  The XL's 608x431 raster is square (1:1);
    // the Lisa 2's native 720x364 raster has taller-than-wide pixels.  Use a 2:3
    // pixel (2 host px wide, 3 high) so at the 200% default zoom every Lisa-2
    // pixel maps to an exact 2x3 host block — sharp integer scaling and a
    // close match to the true ~0.71 ratio of the unmodified raster.
    ls->display.par_w = macxl ? 1 : 2;
    ls->display.par_h = macxl ? 1 : 3;
    ls->display.bits = NULL;
    ls->display.clut = NULL;
    ls->display.clut_len = 0;
    ls->display.shape_dirty = true;
    lisa_refresh_framebuffer(cfg);
}

// hw_profile_t.display callback — surface the framebuffer on the non-NuBus path.
static display_t *lisa_display(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    return ls && ls->display.bits ? &ls->display : NULL;
}

// ============================================================
// Interrupt routing (fixed 68000 IPL levels — docs/machines/lisa/lisa.md §7.1)
// ============================================================

// Set the CPU IPL from the per-level interrupt bitmask.  Lisa sources sit on
// fixed levels: SCC=6, COPS(VIA1)=2, floppy/parallel(VIA2)/VBL=1.
static void lisa_update_ipl(config_t *cfg, int level, bool active) {
    if (active)
        cfg->irq |= (1u << level);
    else
        cfg->irq &= ~(1u << level);
    int ipl = 0;
    for (int l = 7; l >= 1; l--) {
        if (cfg->irq & (1u << l)) {
            ipl = l;
            break;
        }
    }
    cpu_set_ipl(cfg->cpu, ipl);
    cpu_reschedule();
}

// ============================================================
// Parity NMI (level 7)
// ============================================================

// Deassert the parity NMI a short time after asserting it.  The level-based IPL
// model would otherwise re-fire forever; the boot ROM's idempotent parity
// handler tolerates the few NMIs that occur in this window.
static void lisa_nmi_off(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    lisa_update_ipl(cfg, 7, false);
}

// MMU parity-error callback → CPU level-7 NMI (one-shot via scheduled clear).
static void lisa_parity_nmi(void *ctx, bool active) {
    config_t *cfg = (config_t *)ctx;
    if (active) {
        lisa_update_ipl(cfg, 7, true);
        scheduler_new_cpu_event(cfg->scheduler, &lisa_nmi_off, cfg, 0, 40, 0);
    } else {
        lisa_update_ipl(cfg, 7, false);
    }
}

// ============================================================
// VIA address-stride adapters + callbacks
// ============================================================

static uint8_t lisa_via_read8(void *dev, uint32_t off) {
    lisa_via_port_t *p = (lisa_via_port_t *)dev;
    uint32_t reg = (off >> p->reg_shift) & 15;
    return p->vif->read_uint8(p->via, reg << 9);
}
static void lisa_via_write8(void *dev, uint32_t off, uint8_t v) {
    lisa_via_port_t *p = (lisa_via_port_t *)dev;
    uint32_t reg = (off >> p->reg_shift) & 15;
    p->vif->write_uint8(p->via, reg << 9, v);
}
// The Lisa addresses its VIAs with byte accesses on odd addresses; word/long
// forms just defer to the byte register so an unexpected wide access is safe.
static uint16_t lisa_via_read16(void *dev, uint32_t off) {
    return lisa_via_read8(dev, off);
}
static uint32_t lisa_via_read32(void *dev, uint32_t off) {
    return lisa_via_read8(dev, off);
}
static void lisa_via_write16(void *dev, uint32_t off, uint16_t v) {
    lisa_via_write8(dev, off, (uint8_t)v);
}
static void lisa_via_write32(void *dev, uint32_t off, uint32_t v) {
    lisa_via_write8(dev, off, (uint8_t)v);
}

static memory_interface_t lisa_via_iface = {
    .read_uint8 = lisa_via_read8,
    .read_uint16 = lisa_via_read16,
    .read_uint32 = lisa_via_read32,
    .write_uint8 = lisa_via_write8,
    .write_uint16 = lisa_via_write16,
    .write_uint32 = lisa_via_write32,
};

// SCC aggregates to IPL 6 (autovectored).
static void lisa_scc_irq(void *ctx, bool active) {
    lisa_update_ipl((config_t *)ctx, 6, active);
}

// VIA1 (COPS) aggregates to IPL 2; VIA2 (parallel) to IPL 1.
static void lisa_via1_irq(void *ctx, bool active) {
    lisa_update_ipl((config_t *)ctx, 2, active);
}
// Level 1 is the OR of the VIA2 IRQ and the video VBL retrace interrupt.
// Recompute it from the tracked sub-sources so neither clobbers the other.
static void lisa_update_l1(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    lisa_update_ipl(cfg, 1, ls->l1_via2 || ls->l1_vbl || ls->l1_floppy);
}
static void lisa_via2_irq(void *ctx, bool active) {
    config_t *cfg = (config_t *)ctx;
    lisa_state(cfg)->l1_via2 = active;
    lisa_update_l1(cfg);
}
// VIA1 port output → COPS (port A command jam / port B reset line).  VIA1 PB7
// (CRES/, active low) also resets the ProFile controller (boot ROM DOCRES pulses
// it low on handshake-retry); act on the falling edge.
static void lisa_via1_output(void *ctx, uint8_t port, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    lisa_state_t *ls = lisa_state(cfg);
    cops_via_output(ls->cops, port, value);
    if (port == 1) { // port B
        bool pb7 = (value & 0x80) != 0;
        if (ls->via1_pb7 && !pb7) // 1→0: CRES/ asserted
            lisa_profile_reset(ls->profile);
        ls->via1_pb7 = pb7;
    }
}
// VIA2 port output → ProFile control lines (port B: CMD//DRW) / contrast DAC.
// Port-A data bytes go through the dedicated port-A hooks (lisa_profile_porta_*).
//
// `value` (output & direction) cannot tell a pin driven low from one floating
// (an undriven CMD/ reads 0 there but the open-collector line is pulled high).
// Recompute the true levels — driven pins reflect the output latch, undriven
// pins read high — so CMD/ is only "asserted" when actively driven low (i.e.
// after PROINIT makes PB4 an output), not during early VIA2 setup.
static void lisa_via2_output(void *ctx, uint8_t port, uint8_t value) {
    (void)value;
    if (port != 1)
        return;
    config_t *cfg = (config_t *)ctx;
    uint8_t dir = via_port_direction(cfg->via2, 1);
    uint8_t out = via_port_output(cfg->via2, 1);
    uint8_t level = (uint8_t)((out & dir) | ~dir); // undriven pins pull high
    lisa_profile_portb(lisa_state(cfg)->profile, level);
}
static void lisa_via_shift_out(void *ctx, uint8_t byte) {
    (void)ctx;
    (void)byte;
}

// ProFile BSY line → VIA2 PB1 (level, polled by the boot ROM) and CA1 (edge,
// used by the OS interrupt path).  BSY is active-low: busy → line low.
static void lisa_profile_bsy(void *ctx, bool busy) {
    config_t *cfg = (config_t *)ctx;
    bool level = !busy; // 0 = busy, 1 = not busy
    if (cfg->via2) {
        via_input(cfg->via2, 1, 1, level); // PB1
        via_input_c(cfg->via2, 0, 0, level); // CA1 (port A control line 0)
    }
}

// VIA2 port-A data hooks → ProFile (handshake = the CA2/PSTRB-strobed register).
static uint8_t lisa_profile_porta_read_cb(void *ctx, bool handshake) {
    return lisa_profile_porta_read(lisa_state((config_t *)ctx)->profile, handshake);
}
static void lisa_profile_porta_write_cb(void *ctx, uint8_t value, bool handshake) {
    lisa_profile_porta_write(lisa_state((config_t *)ctx)->profile, value, handshake);
}

// Drive the controller's static input lines: OCD/ (PB0, 0 = a disk is connected)
// and the idle BSY level.  Called at init and after any attach/detach.
static void lisa_profile_update_lines(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    bool connected = lisa_profile_connected(ls->profile);
    if (cfg->via2)
        via_input(cfg->via2, 1, 0, !connected); // PB0 = OCD/ (0 = connected)
    lisa_profile_bsy(cfg, false); // idle: not busy
}

// ============================================================
// Host input → COPS (keyboard scancodes, mouse deltas + button)
// ============================================================
//
// hw_profile_t.input_* hooks: the `keyboard`/`mouse` object methods route here
// (instead of the Mac ADB/Toolbox path) because the Lisa's input device is the
// COPS, which uses its own keycodes and a relative-delta mouse (§11.4).

// keyboard.press → a raw COPS scancode.  Accepts a "0xNN" keycode string (the
// boot-menu keys, e.g. $EB = 'H'/ProFile, $F2 = '3').  The COPS reports a key on
// its press edge, so we inject on `down` and treat the release as a no-op.
static int lisa_input_key(config_t *cfg, const char *key, bool down) {
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->cops)
        return -1;
    if (!down)
        return 0; // release: nothing to send (press edge already reported)
    if (key && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        char *end = NULL;
        long v = strtol(key, &end, 16);
        if (end && *end == '\0' && v >= 0 && v <= 0xFF) {
            cops_inject_key(ls->cops, (uint8_t)v);
            return 0;
        }
    }
    return -1; // unknown key (no Mac fallback on the Lisa)
}

// mouse.move → COPS mouse deltas (default/relative), or absolute screen-pixel
// positioning in "global" mode.  The Lisa mouse hardware is relative and the OS
// scales the deltas into screen pixels, so absolute placement uses a closed-loop
// "warp": the COPS reads the OS's live cursor globals ($CC00F0 = X, $CC00F2 = Y)
// each mouse report and emits a corrective delta toward the target, converging
// regardless of the scaling.  This lets a test place the cursor on a button by
// pixel coordinate.  mouse.click → the COPS button keycode, which the OS hit-tests
// against the cursor it has tracked there.
static int lisa_input_mouse_move(config_t *cfg, int x, int y, const char *mode) {
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->cops)
        return -1;
    if (mode && strcmp(mode, "global") == 0) {
        // Absolute: steer the OS cursor to screen pixel (x,y) via the COPS
        // closed-loop warp.  Converges over the next several mouse reports, so the
        // caller follows with scheduler.run.
        cops_set_warp(ls->cops, x, y);
        return 0;
    }
    cops_inject_mouse(ls->cops, x, y, -1);
    return 0;
}
static int lisa_input_mouse_button(config_t *cfg, bool down, const char *mode) {
    (void)mode;
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->cops)
        return -1;
    cops_inject_mouse(ls->cops, 0, 0, down ? 1 : 0);
    return 0;
}

// ============================================================
// Floppy controller (6504A) wiring
// ============================================================

// FDC I/O adapter: lisa_mmu dispatches with dev = the lisa_fdc_t and offset =
// physical address − $C001.
static uint8_t lisa_fdc_io_read8(void *dev, uint32_t off) {
    return lisa_fdc_read8((lisa_fdc_t *)dev, off);
}
static uint16_t lisa_fdc_io_read16(void *dev, uint32_t off) {
    return lisa_fdc_read16((lisa_fdc_t *)dev, off);
}
static uint32_t lisa_fdc_io_read32(void *dev, uint32_t off) {
    return lisa_fdc_read32((lisa_fdc_t *)dev, off);
}
static void lisa_fdc_io_write8(void *dev, uint32_t off, uint8_t v) {
    lisa_fdc_write8((lisa_fdc_t *)dev, off, v);
}
static void lisa_fdc_io_write16(void *dev, uint32_t off, uint16_t v) {
    lisa_fdc_write16((lisa_fdc_t *)dev, off, v);
}
static void lisa_fdc_io_write32(void *dev, uint32_t off, uint32_t v) {
    lisa_fdc_write32((lisa_fdc_t *)dev, off, v);
}
static memory_interface_t lisa_fdc_iface = {
    .read_uint8 = lisa_fdc_io_read8,
    .read_uint16 = lisa_fdc_io_read16,
    .read_uint32 = lisa_fdc_io_read32,
    .write_uint8 = lisa_fdc_io_write8,
    .write_uint16 = lisa_fdc_io_write16,
    .write_uint32 = lisa_fdc_io_write32,
};

// FDIR (drive interrupt request) → VIA1 PB4, which the boot ROM polls (CHKFIN),
// AND a level-1 interrupt (docs/machines/lisa/lisa.md §7.1/§13: the floppy shares IPL 1 with
// VIA2/video; it fires on RWTS completion, disk insertion, and eject).  The boot
// ROM masks IPL 1 and polls PB4; the OS Sony driver (SOURCE-SONYASM, WAIT_INT)
// blocks and is woken by this interrupt — without it the OS reader hangs forever
// after its first interrupt-driven read.  The level-1 handler reads $00C05F.
static void lisa_fdc_fdir(void *ctx, bool asserted) {
    config_t *cfg = (config_t *)ctx;
    lisa_state_t *ls = lisa_state(cfg);
    if (cfg->via1)
        via_input(cfg->via1, 1, 4, asserted); // PB4 = FDIR (polled by the ROM)
    ls->l1_floppy = asserted;
    lisa_update_l1(cfg); // floppy aggregates to IPL 1 (used by the OS driver)
}

// hw_profile_t.fd_insert / fd_present — the Lisa uses lisa_fdc, not cfg->floppy.
static int lisa_fd_insert(config_t *cfg, int drive, struct image *disk) {
    lisa_state_t *ls = lisa_state(cfg);
    if (drive != 0 || !ls || !ls->fdc)
        return -1; // one Sony drive
    lisa_fdc_insert(ls->fdc, (image_t *)disk);
    return 0;
}
static bool lisa_fd_present(config_t *cfg, int drive) {
    lisa_state_t *ls = lisa_state(cfg);
    if (drive != 0)
        return true; // only drive 0 exists; report others "occupied"
    return ls && ls->fdc && lisa_fdc_disk_present(ls->fdc);
}

// ============================================================
// `floppy` object surface (insert/eject the one Sony drive at runtime)
// ============================================================
//
// The Lisa's drive is the 6504A FDC, not the IWM, so it gets its own small
// object tree (the IWM `floppy` object in floppy.c is bound to a floppy_t).
// `insert` calls system_fd_insert (drive 0) — the same path every other
// machine uses; `eject` and `present` go straight to the FDC. Each
// object's instance_data is the config_t.

static value_t lisa_fd_drive_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool writable = (argc >= 2) ? argv[1].b : false;
    return val_bool(system_fd_insert(argv[0].s, 0, writable) == 0);
}

static value_t lisa_fd_drive_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    if (!ls || !ls->fdc)
        return val_err("floppy.drives.0: no controller");
    // Ejecting an empty drive is a harmless no-op (idempotent): a multi-disk
    // install script can eject-then-insert each floppy without first knowing
    // whether the OS already ejected the previous one (e.g. Xenix's hard-disk
    // boot ejects the boot floppy before firsttime asks for the first disk).
    if (lisa_fdc_disk_present(ls->fdc))
        lisa_fdc_eject(ls->fdc);
    return val_none();
}

static value_t lisa_fd_drive_present(struct object *self, const member_t *m) {
    (void)m;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return val_bool(ls && ls->fdc && lisa_fdc_disk_present(ls->fdc));
}

static value_t lisa_fd_drive_index(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_int(0);
}

static const arg_decl_t lisa_fd_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Host path or storage URI of the image to mount"},
    {.name = "writable", .kind = V_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Mount writable (default false)"},
};

static const member_t lisa_fd_drive_members[] = {
    {.kind = M_ATTR,   .name = "index",   .flags = VAL_RO, .attr = {.type = V_INT, .get = lisa_fd_drive_index}   },
    {.kind = M_ATTR,   .name = "present", .flags = VAL_RO, .attr = {.type = V_BOOL, .get = lisa_fd_drive_present}},
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Eject the disk (unclamp)",
     .method = {.result = V_NONE, .fn = lisa_fd_drive_eject}                                                     },
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Mount a disk image into the Sony drive",
     .method = {.args = lisa_fd_insert_args, .nargs = 2, .result = V_BOOL, .fn = lisa_fd_drive_insert}           },
};
static const class_desc_t lisa_fd_drive_class = {
    .name = "floppy_drive", .members = lisa_fd_drive_members, .n_members = 4};

static struct object *lisa_fd_drives_get(struct object *self, int index) {
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return (ls && index == 0) ? ls->fd_drive_obj : NULL;
}
static int lisa_fd_drives_count(struct object *self) {
    (void)self;
    return 1; // one Sony drive
}
static int lisa_fd_drives_next(struct object *self, int prev) {
    (void)self;
    return prev + 1 < 1 ? prev + 1 : -1;
}
static const member_t lisa_fd_drives_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &lisa_fd_drive_class,
               .indexed = true,
               .get = lisa_fd_drives_get,
               .count = lisa_fd_drives_count,
               .next = lisa_fd_drives_next}},
};
static const class_desc_t lisa_fd_drives_class = {
    .name = "floppy_drives", .members = lisa_fd_drives_members, .n_members = 1};

static const member_t lisa_fd_members[] = {0}; // container only; the drives collection is the child
static const class_desc_t lisa_fd_class = {.name = "floppy", .members = NULL, .n_members = 0};

// Attach the `floppy` → `drives` → `drives[0]` object tree for this machine.
static void lisa_register_floppy_object(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    (void)lisa_fd_members;
    ls->fd_obj = object_new(&lisa_fd_class, cfg, "floppy");
    if (!ls->fd_obj)
        return;
    object_set_label(ls->fd_obj, "Floppy");
    object_set_order(ls->fd_obj, 80);
    object_attach(machine_object(), ls->fd_obj);
    // Named "drive" (singular) to match the standard floppy collection
    // (machine.floppy.drive[N]) after the proposal-system-object-model rename.
    ls->fd_drives_obj = object_new(&lisa_fd_drives_class, cfg, "drive");
    if (ls->fd_drives_obj)
        object_attach(ls->fd_obj, ls->fd_drives_obj);
    ls->fd_drive_obj = object_new(&lisa_fd_drive_class, cfg, NULL); // returned by the indexed get
}

// ============================================================
// `profile` object surface (attach/detach the parallel hard disk)
// ============================================================
//
// The ProFile is not on a SCSI bus and not the IWM floppy, so it gets its own
// small object.  `attach` opens (or creates blank) a 532-bytes/block image and
// drives the OCD/ line; `detach` flushes and disconnects.  instance_data = cfg.

static value_t lisa_hd_attach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    const char *path = (argc >= 1) ? argv[0].s : NULL; // NULL = blank in-memory disk
    bool writable = (argc >= 2) ? argv[1].b : true;
    if (!ls || !ls->profile)
        return val_err("profile: no controller");
    if (!lisa_profile_attach(ls->profile, path, writable))
        return val_err("profile.attach: cannot open '%s'", path ? path : "(blank)");
    lisa_profile_update_lines(cfg);
    return val_bool(true);
}

static value_t lisa_hd_detach(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->profile || !lisa_profile_attached(ls->profile))
        return val_err("profile: no disk attached");
    lisa_profile_detach(ls->profile);
    lisa_profile_update_lines(cfg);
    return val_none();
}

static value_t lisa_hd_present(struct object *self, const member_t *m) {
    (void)m;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return val_bool(ls && lisa_profile_attached(ls->profile));
}

static value_t lisa_hd_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->profile || !lisa_profile_attached(ls->profile))
        return val_err("profile: no disk attached");
    const char *path = (argc >= 1) ? argv[0].s : NULL;
    if (!path || !*path)
        return val_err("profile.save: a destination path is required");
    if (!lisa_profile_save_as(ls->profile, path))
        return val_err("profile.save: cannot write '%s'", path);
    return val_bool(true);
}

static const arg_decl_t lisa_hd_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Destination path for the consolidated single-file ProFile image"},
};

// The Lisa's battery-backed parameter memory ($FCC181 in the FDC shared RAM)
// holds the OS's boot volume + device-configuration table.  The installer adds
// the ProFile to that table at clean shutdown ("Finished" -> turn off / start
// up); persisting it lets an installed system boot from the ProFile on a later
// (cold) launch.  Load before booting (the ROM reads PRAM during startup).
static value_t lisa_hd_pram_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    const char *path = (argc >= 1) ? argv[0].s : NULL;
    if (!ls || !ls->fdc)
        return val_err("pram: no controller");
    if (!path || !*path)
        return val_err("profile.pram_save: a destination path is required");
    if (!lisa_fdc_pram_save(ls->fdc, path))
        return val_err("profile.pram_save: cannot write '%s'", path);
    return val_bool(true);
}

static value_t lisa_hd_pram_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    const char *path = (argc >= 1) ? argv[0].s : NULL;
    if (!ls || !ls->fdc)
        return val_err("pram: no controller");
    if (!path || !*path)
        return val_err("profile.pram_load: a source path is required");
    if (!lisa_fdc_pram_load(ls->fdc, path))
        return val_err("profile.pram_load: cannot read '%s'", path);
    return val_bool(true);
}

static const arg_decl_t lisa_hd_pram_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Parameter-memory (PRAM) file path"},
};

static const arg_decl_t lisa_hd_attach_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Host path of the ProFile image, created blank if missing (omit for a blank in-memory disk)"             },
    {.name = "writable", .kind = V_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Mount writable (default true)"},
};

static const member_t lisa_hd_members[] = {
    {.kind = M_ATTR,   .name = "present", .flags = VAL_RO,                                             .attr = {.type = V_BOOL, .get = lisa_hd_present}                                       },
    {.kind = M_METHOD,
     .name = "detach",
     .doc = "Flush and disconnect the ProFile",
     .method = {.result = V_NONE, .fn = lisa_hd_detach}                                                                                                                                       },
    {.kind = M_METHOD,
     .name = "attach",
     .doc = "Attach a ProFile image (created blank if missing; omit path for a blank in-memory disk)",
     .method = {.args = lisa_hd_attach_args, .nargs = 2, .result = V_BOOL, .fn = lisa_hd_attach}                                                                                              },
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Write the current ProFile contents to a new self-contained single-file image (consolidated; not a "
            "base+delta pair)",                                                                        .method = {.args = lisa_hd_save_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_save}},
    {.kind = M_METHOD,
     .name = "pram_save",
     .doc = "Save the machine parameter memory (battery-backed NVRAM at $FCC181) to a file",
     .method = {.args = lisa_hd_pram_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_pram_save}                                                                                             },
    {.kind = M_METHOD,
     .name = "pram_load",
     .doc = "Load the machine parameter memory from a file (call before booting)",
     .method = {.args = lisa_hd_pram_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_pram_load}                                                                                             },
};
static const class_desc_t lisa_hd_class = {.name = "profile", .members = lisa_hd_members, .n_members = 6};

static void lisa_register_profile_object(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    // Named "hd" (not "profile") under the machine node: a child named
    // "profile" would be shadowed by the machine class's `profile` *method*
    // (the resolver finds members before attached children). The ProFile is
    // the Lisa's hard disk, so machine.hd reads correctly (proposal §2.2).
    ls->hd_obj = object_new(&lisa_hd_class, cfg, "hd");
    if (ls->hd_obj) {
        object_set_label(ls->hd_obj, "ProFile");
        object_set_order(ls->hd_obj, 90);
        object_attach(machine_object(), ls->hd_obj);
    }
}

// ============================================================
// `power` object — the Lisa's soft power-off switch (on the COPS)
// ============================================================

// Press the soft power-off switch.  The COPS reports it ($80 $FB) and LOS runs
// its orderly shutdown, cleanly unmounting the boot volume (mountinfo :=
// unmounted) — so a `profile.save` afterwards yields an image that cold-boots
// without the "startup disk was in use" scavenge prompt.  No-op pre-boot / on a
// machine whose OS isn't listening; harmless either way (the code just queues).
static value_t lisa_power_off(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    if (!ls || !ls->cops)
        return val_err("power: no COPS");
    cops_soft_power_off(ls->cops);
    return val_none();
}

static const member_t lisa_power_members[] = {
    {.kind = M_METHOD,
     .name = "off",
     .doc = "Press the soft power-off switch (COPS $FB); LOS does an orderly shutdown",
     .method = {.result = V_NONE, .fn = lisa_power_off}},
};
static const class_desc_t lisa_power_class = {.name = "power", .members = lisa_power_members, .n_members = 1};

static void lisa_register_power_object(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    ls->power_obj = object_new(&lisa_power_class, cfg, "power");
    if (ls->power_obj) {
        object_set_label(ls->power_obj, "Power");
        object_set_order(ls->power_obj, 130);
        object_attach(machine_object(), ls->power_obj);
    }
}

// ============================================================
// Init / Teardown
// ============================================================

static void lisa_vbl_off(void *source, uint64_t data); // defined in the VBL section
static void lisa_vbl_ack(void *source); // defined in the VBL section

static void lisa_init(config_t *cfg, checkpoint_t *checkpoint) {
    lisa_state_t *ls = (lisa_state_t *)malloc(sizeof(lisa_state_t));
    assert(ls != NULL);
    memset(ls, 0, sizeof(*ls));
    cfg->machine_context = ls;

    // 24-bit address space, configured RAM, 16 KB interleaved boot ROM.
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);

    cfg->cpu = cpu_init(CPU_MODEL_68000, checkpoint);
    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);
    // Run at the Lisa's real 5.09375 MHz, not the scheduler's Mac-Plus default
    // (7.8336 MHz).  Set before the VIAs init: their timer clock is CPU/4, so the
    // wrong CPU frequency would skew every VIA-timer-derived rate — including the
    // ~250 Hz tick MacWorks XL programs into VIA2 T1 — and inflate the real-time
    // CPU budget ~1.5x, which on a throughput-bound host shows up as a sluggish
    // "?" blink and a choppy cursor.
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // Keep the Lisa's long-standing effective CPI of 4 (the pre-two-modes
    // default-mode value every Lisa test budget was derived at). The authentic
    // 68000 average would be ~12; moving the Lisa to it is a separate decision
    // with its own test re-pin, out of scope for the two-modes change.
    scheduler_set_cpi(cfg->scheduler, 4);

    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    // The segment MMU owns all translation; it reads/writes directly into the
    // flat RAM+ROM image the memory map allocated.  The ROM region is filled
    // later by rom.load_lisa(); the host pointer stays valid (same buffer).
    // Lisa 2 DRAM is based high ($80000); the Macintosh XL keeps it low (0).
    bool ram_high = !(cfg->machine && cfg->machine->id && strcmp(cfg->machine->id, "macxl") == 0);
    ls->mmu =
        lisa_mmu_init(ram_native_pointer(cfg->mem_map, 0), cfg->ram_size, (uint8_t *)memory_rom_bytes(cfg->mem_map),
                      memory_rom_size(cfg->mem_map), ram_high, checkpoint);
    lisa_mmu_set_nmi(ls->mmu, lisa_parity_nmi, cfg); // level-7 parity NMI (PARTST)
    lisa_mmu_set_clock(ls->mmu, cfg->scheduler); // cycle source for the retrace status bit
    lisa_mmu_set_vbl_ack(ls->mmu, lisa_vbl_ack, cfg); // Status-Register read acks the latched VBL

    // Two 6522 VIAs (reused unchanged).  map=NULL: the machine registers the
    // interface itself.  freq_factor 4 = 68000/4 ≈ 1.27 MHz (docs/machines/lisa/lisa.md §10).
    cfg->via1 =
        via_init(NULL, cfg->scheduler, 4, "via1", lisa_via1_output, lisa_via_shift_out, lisa_via1_irq, cfg, checkpoint);
    cfg->via2 =
        via_init(NULL, cfg->scheduler, 4, "via2", lisa_via2_output, lisa_via_shift_out, lisa_via2_irq, cfg, checkpoint);

    // Register each VIA at its Lisa physical I/O base through the stride
    // adapter.  16 registers: VIA1 spans 16*2=32 bytes from $DD81, VIA2 uses an
    // 8-byte register stride.  The canonical VIA2 base is $D901 (docs/machines/lisa/lisa.md
    // §10.2, the boot ROM VIA2BASE), but the chip-select ignores address bit 8,
    // so the whole range $D800–$D9FF decodes to VIA2 (register = (addr>>3)&15).
    // The boot ROM and the OS clock use the $D9xx alias, but the LisaOS parallel
    // hard-disk driver (SYSTEM.CD_PROFILE, PROF_INIT) addresses its VIA2 registers
    // off base $D801 (IER at $D871) — so the device must be decoded over the full
    // $D800–$D9FF window or the driver's accesses are silently dropped and the
    // ProFile is never detected.  The &15 register decode makes the two aliases
    // identical, so widening the window leaves the existing $D9xx accesses
    // unchanged.
    ls->via1_map = (lisa_via_port_t){.via = cfg->via1, .vif = via_get_memory_interface(cfg->via1), .reg_shift = 1};
    ls->via2_map = (lisa_via_port_t){.via = cfg->via2, .vif = via_get_memory_interface(cfg->via2), .reg_shift = 3};
    lisa_mmu_map_io(ls->mmu, 0xDD81, 16 * 2, &lisa_via_iface, &ls->via1_map);
    // LisaOS addresses VIA2 over the full $D800-$D9FF window (its ProFile driver
    // uses base $D801); MacWorks XL uses only the $D901 alias and depends on the
    // rest of that window staying unmapped, so give macxl the narrow $D901 region.
    if (strcmp(cfg->machine->id, "macxl") == 0)
        lisa_mmu_map_io(ls->mmu, 0xD901, 16 * 8, &lisa_via_iface, &ls->via2_map);
    else
        lisa_mmu_map_io(ls->mmu, 0xD800, 0x200, &lisa_via_iface, &ls->via2_map);

    // COPS keyboard/mouse/clock/power microcontroller on VIA1 port A.
    ls->cops = cops_init(cfg->via1, cfg->scheduler, checkpoint);

    // Intelligent floppy controller: 6504 + 1 KB shared RAM on the ODD bus
    // bytes of physical $00C000-$00C7FF (docs/machines/lisa/lisa.md §13).  Mapped from the
    // even base $00C000 so word/long accesses at the even base (used by Xenix's
    // boot loader) reach the controller; the iface models the odd-byte RAM.
    // FDIR completion is signalled on VIA1 PB4.
    ls->fdc = lisa_fdc_init(cfg->scheduler, lisa_fdc_fdir, cfg, checkpoint);
    lisa_mmu_map_io(ls->mmu, 0xC000, 0x800, &lisa_fdc_iface, ls->fdc);
    // PB4 carries the FDC's FDIR (drive interrupt request) line.  The 6504A drives
    // it — it is not a floating/pulled-up input — and at reset there is no pending
    // interrupt, so FDIR is deasserted (low).  The 6522 powers port B up idle-high
    // (0xFF), which would leave PB4 reading a phantom "floppy interrupt asserted";
    // on a diskless boot (booting the ProFile with no floppy) nothing ever drives
    // PB4, so that phantom would trap the OS's level-1 interrupt handler forever
    // (it re-reads PB4 high every pass and never finishes its source scan, so the
    // floppy driver's own INITDISK — which would clear the line — never runs).
    // Establish FDIR's true deasserted reset level here.
    via_input(cfg->via1, 1, 4, false);
    // Disk-controller ROM id ($FCC031 = adr_ioboard) selects the I/O-board model
    // the boot ROM (SETTYPE/SYSTYPE) and LisaOS (SOURCE-STARTUP) detect.  LisaOS
    // reads it as a SIGNED byte: >=0 (bit7 clear) ⇒ iob_lisa (Lisa 1, Twiggy) ⇒
    // it installs the TWIGGY floppy driver — fatal on our Sony hardware.  A Lisa
    // 2/5 (old "Lisa Lite" board + Sony + external ProFile) reports $A0..$BF ⇒
    // iob_sony ⇒ the SONY driver (and boot-ROM SYSTYPE 1).  Left at 0 LisaOS
    // mis-drives the floppy as a Twiggy and never completes boot.  The Macintosh
    // XL path (MacWorks XL, iob_pepsi) keeps its empirically-correct 0: its
    // loader-disk eject sequence only matches with SYSTYPE 0 (revisit when
    // MacWorks's own machine-id handling is investigated).
    if (strcmp(cfg->machine->id, "macxl") != 0)
        lisa_fdc_set_diskrom(ls->fdc, 0xA0); // iob_sony — Lisa 2/5

    // Expose the Sony drive so disks can be inserted/ejected at runtime
    // (floppy.drives[0].insert / .eject), e.g. swapping the MacWorks loader disk
    // for its system disk.
    lisa_register_floppy_object(cfg);

    // ProFile parallel hard disk on VIA2: control lines via the port-B output
    // callback (lisa_via2_output), data via the port-A hooks, BSY back to PB1/CA1.
    ls->profile = lisa_profile_init(cfg->scheduler, lisa_profile_bsy, cfg, checkpoint);
    via_set_porta_hooks(cfg->via2, lisa_profile_porta_read_cb, lisa_profile_porta_write_cb, cfg);
    lisa_profile_update_lines(cfg); // no disk yet → OCD/ high (disconnected)
    lisa_register_profile_object(cfg);
    lisa_register_power_object(cfg); // soft power-off switch (COPS) → `power.off`

    // Z8530 SCC (reused as-is): physical $00D241/43/45/47 decode from base
    // $00D240 via the standard A1/A2 convention (docs/machines/lisa/lisa.md §15).  PCLK 4 MHz
    // (chan A) / 3.6864 MHz (chan B).  Autovectored at IPL 6.
    cfg->scc = scc_init(NULL, cfg->scheduler, lisa_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 4000000, 3686400);
    lisa_mmu_map_io(ls->mmu, 0xD240, 8, (memory_interface_t *)scc_get_memory_interface(cfg->scc), cfg->scc);

    lisa_display_init(cfg);
    scheduler_new_event_type(cfg->scheduler, "lisa", cfg, "vbl_off", &lisa_vbl_off);
    scheduler_new_event_type(cfg->scheduler, "lisa", cfg, "nmi_off", &lisa_nmi_off);

    cfg->debugger = debug_init();

    scheduler_start(cfg->scheduler);

    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

static void lisa_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);

    lisa_state_t *ls0 = lisa_state(cfg);
    if (ls0) {
        // Tear down the `floppy` object tree (entry is unattached, like the IWM
        // floppy object, so delete it directly).
        if (ls0->fd_drive_obj) {
            object_delete(ls0->fd_drive_obj);
            ls0->fd_drive_obj = NULL;
        }
        if (ls0->fd_drives_obj) {
            object_detach(ls0->fd_drives_obj);
            object_delete(ls0->fd_drives_obj);
            ls0->fd_drives_obj = NULL;
        }
        if (ls0->fd_obj) {
            object_detach(ls0->fd_obj);
            object_delete(ls0->fd_obj);
            ls0->fd_obj = NULL;
        }
        if (ls0->hd_obj) {
            object_detach(ls0->hd_obj);
            object_delete(ls0->hd_obj);
            ls0->hd_obj = NULL;
        }
        if (ls0->power_obj) {
            object_detach(ls0->power_obj);
            object_delete(ls0->power_obj);
            ls0->power_obj = NULL;
        }
    }
    if (ls0 && ls0->profile) {
        lisa_profile_delete(ls0->profile);
        ls0->profile = NULL;
    }
    if (ls0 && ls0->fdc) {
        lisa_fdc_delete(ls0->fdc);
        ls0->fdc = NULL;
    }
    if (ls0 && ls0->cops) {
        cops_delete(ls0->cops);
        ls0->cops = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->via2) {
        via_delete(cfg->via2);
        cfg->via2 = NULL;
    }
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    lisa_state_t *ls = lisa_state(cfg);
    if (ls && ls->mmu) {
        lisa_mmu_delete(ls->mmu);
        ls->mmu = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }
    if (ls) {
        free(ls);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

static void lisa_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    lisa_state_t *ls = lisa_state(cfg);
    lisa_mmu_checkpoint(ls ? ls->mmu : NULL, cp);
    // Same order as the restore path in lisa_init (via1, via2, cops, fdc).
    via_checkpoint(cfg->via1, cp);
    via_checkpoint(cfg->via2, cp);
    cops_checkpoint(ls ? ls->cops : NULL, cp);
    lisa_fdc_checkpoint(ls ? ls->fdc : NULL, cp);
    lisa_profile_checkpoint(ls ? ls->profile : NULL, cp);
    scc_checkpoint(cfg->scc, cp);
}

// ============================================================
// VBL
// ============================================================

// Pulse the Status Register vertical-retrace bit each frame so the ROM video
// test and (later) the OS VBL handler observe a retrace.  Video interrupt
// delivery is wired in Step 3 along with the display.
// End of the vertical-retrace window: clear the Status Register VBL bit.
static void lisa_vbl_off(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    lisa_state_t *ls = lisa_state(cfg);
    if (ls && ls->mmu) {
        lisa_mmu_set_vbl_active(ls->mmu, false);
        // Retrace window ended: drop the VBL contribution to level 1.
        ls->l1_vbl = false;
        lisa_update_l1(cfg);
    }
}

// ~90 µs retrace window (docs/machines/lisa/lisa.md §8) at 5.09375 MHz ≈ 458 cycles.  Holding
// the Status Register VBL bit this long lets the ROM's video self-test (VIDTST)
// observe the low→high retrace edge instead of timing out (boot error 42).
#define LISA_VBL_HOLD_CYCLES 458

// The OS's IPL-1 handler reads the Status Register to identify the VBL; that read
// is the VBL acknowledge.  Clear the latched VBL IRQ (the real hardware's edge-fired
// autovector is taken once per retrace).
static void lisa_vbl_ack(void *source) {
    config_t *cfg = (config_t *)source;
    lisa_state_t *ls = lisa_state(cfg);
    if (ls && ls->l1_vbl) {
        ls->l1_vbl = false;
        lisa_update_l1(cfg);
    }
}

static void lisa_trigger_vbl(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    if (ls && ls->mmu) {
        // VBL is an IPL-1 interrupt source (docs/machines/lisa/lisa.md §8) gated by the VTMSK
        // latch ($E01A on / $E018 off).  Real hardware FIREs the video
        // IRQ at the retrace edge and LATCHes it until the CPU services it, so a
        // kernel that is interrupt-masked through the retrace window still sees the
        // VBL on unmask.  We model that by holding the IRQ asserted (and forcing the
        // Status Register retrace bit via vbl_active) until the OS reads the Status
        // Register (lisa_vbl_ack).  The old fixed 458-cycle pulse dropped the VBL
        // whenever the kernel was masked through it — which left a freshly
        // dispatched user process to run its installer-segment trampoline before the
        // VBL-driven segment load, faulting on the not-yet-resident segment.
        if (lisa_mmu_vbl_enabled(ls->mmu)) {
            lisa_mmu_set_vbl_active(ls->mmu, true);
            ls->l1_vbl = true;
            lisa_update_l1(cfg);
        }
        lisa_refresh_framebuffer(cfg);
    }
}

// ============================================================
// Machine descriptor
// ============================================================

// Lisa 2 supports 512 KB / 1 MB / 2 MB (in 128 KB-granular increments the
// boot ROM's memory sizing walks); the ROM's MAXADR ceiling is 2 MB.
static const uint32_t lisa_ram_options_kb[] = {512, 1024, 2048, 0};

// One Sony 400 KB 3.5" mechanism (the intelligent 6504A controller arrives in
// Step 5).  Lisa 1's Twiggy drives are out of scope.
static const struct floppy_slot lisa_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_400K},
    {0},
};

// The Lisa hard disk is parallel-port ProFile/Widget, NOT SCSI, so this SCSI
// table stays empty: the parallel disk is its own device (lisa_profile.c), and
// the profile advertises it via `.hd_bus = HD_BUS_PROFILE`.  The config UI reads
// hd_bus to label the HD row "ProFile" and attach through profile.attach rather
// than scsi.attach_hd.
static const struct scsi_slot lisa_scsi_slots[] = {
    {0},
};

// Apple Lisa 2 hardware profile.
static const machine_substrate_t lisa_substrate = {
    .init = lisa_init,
    .teardown = lisa_teardown,
    .checkpoint_save = lisa_checkpoint_save,
    .trigger_vbl = lisa_trigger_vbl,
    .display = lisa_display,
    .fd_insert = lisa_fd_insert,
    .fd_present = lisa_fd_present,
    .input_key = lisa_input_key,
    .input_mouse_move = lisa_input_mouse_move,
    .input_mouse_button = lisa_input_mouse_button,
};

const hw_profile_t machine_lisa = {
    .name = "Apple Lisa 2",
    .id = "lisa",

    // 68000 at 5.09375 MHz (20.375 MHz crystal / 4).
    .cpu_model = 68000,
    .freq = 5093750,
    .mmu_kind = MMU_LISA_SEGMENT,

    .address_bits = 24,
    .ram_default = 0x100000, // 1 MB
    .ram_max = 0x200000, // 2 MB
    .rom_size = 0x004000, // 16 KB interleaved boot ROM

    .ram_options = lisa_ram_options_kb,
    .floppy_slots = lisa_floppy_slots,
    .scsi_slots = lisa_scsi_slots,
    .hd_bus = HD_BUS_PROFILE, // parallel-port ProFile, not SCSI
    .has_cdrom = false,
    .cdrom_id = 0,

    .substrate = &lisa_substrate,
};

// Macintosh XL: the same Lisa 2 hardware sold with the "3A" boot ROM and the
// screen-mod (square-pixel) kit, running MacWorks XL.  For the emulator it is
// the Lisa 2 profile with a different ROM-compatibility id and name — the chip
// models, callbacks, and 720×364 framebuffer are identical (the square-pixel
// kit only changes the dot clock, which a frame-accurate model ignores).
// docs/machines/lisa/lisa.md §1.1 / proposal-machine-lisa-xl.md §3.2.
const hw_profile_t machine_macxl = {
    .name = "Macintosh XL",
    .id = "macxl",

    .cpu_model = 68000,
    .freq = 5093750,
    .mmu_kind = MMU_LISA_SEGMENT,

    .address_bits = 24,
    .ram_default = 0x100000, // 1 MB
    .ram_max = 0x200000, // 2 MB
    .rom_size = 0x004000, // 16 KB interleaved "3A" boot ROM

    .ram_options = lisa_ram_options_kb,
    .floppy_slots = lisa_floppy_slots,
    .scsi_slots = lisa_scsi_slots,
    .hd_bus = HD_BUS_PROFILE, // parallel-port ProFile, not SCSI
    .has_cdrom = false,
    .cdrom_id = 0,

    .substrate = &lisa_substrate,
};
