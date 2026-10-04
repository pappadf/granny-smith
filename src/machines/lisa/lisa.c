// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa.c
// Apple Lisa 2 machine implementation (and, later, the Macintosh XL variant).
//
// The Lisa is the first non-Mac machine: a 68000 with a custom segment MMU
// (lisa_mmu.c), the COPS keyboard/mouse/clock microcontroller, an intelligent
// 6504A floppy controller, and a parallel-port hard disk — none of which are
// Mac architecture.  See docs/reference/machines/lisa/lisa.md for the hardware reference.

#include "machine.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h"
#include "system_config.h"

#include "cops.h"
#include "cpu.h"
#include "debug.h"
#include "display.h"
#include "image.h"
#include "io_leaf.h"
#include "lisa_fdc.h"
#include "lisa_keymap.h"
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
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_USE_CATEGORY_NAME("board");

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
    // (docs/reference/machines/lisa/lisa.md §7.1/§12).  Track each sub-source so deasserting one does
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
    // Which keys the host holds down, by ADB raw code.  A repeated down (a
    // host's auto-repeat) or an up with no down is not a key transition, and
    // the COPS must not report one -- the ADB and Plus keyboards suppress the
    // same.  Host-side state, so not checkpointed.
    bool key_held[128];
} lisa_state_t;

// Video geometry, 1 bpp MSB-first (docs/reference/machines/lisa/lisa.md §8).  The unmodified Lisa 2 has
// a 720x364 rectangular-pixel raster; the Macintosh XL screen modification that
// MacWorks XL targets is a 608x431 square-pixel raster.  Same framebuffer (~32
// KB at the $E800 video latch base) — only the scan geometry differs.
#define LISA_SCREEN_W  720
#define LISA_SCREEN_H  364
#define MACXL_SCREEN_W 608
#define MACXL_SCREEN_H 431

// The Lisa-family board descriptor: the four facts that differ between the
// Lisa 2 and the Macintosh XL, which is the same board sold with the "3A"
// boot ROM and MacWorks.  Data only, no hooks, so hw_profile_t.board names
// this directly -- the tnt/pdm shape rather than the mcu/av one, which
// wraps its desc in a struct of function pointers.
typedef struct lisa_board_desc {
    uint32_t screen_w, screen_h; // raster: 720x364 Lisa 2, 608x431 XL
    // Pixel aspect ratio (display.h).  The XL's 608x431 raster is square
    // (1:1); the Lisa 2's native 720x364 raster has taller-than-wide pixels,
    // so it takes a 2:3 pixel (2 host px wide, 3 high) -- at the 200% default
    // zoom every Lisa-2 pixel maps to an exact 2x3 host block, which is sharp
    // integer scaling and a close match to the true ~0.71 ratio.
    uint8_t par_w, par_h;
    // Lisa 2 DRAM is based high ($80000); the Macintosh XL keeps it low (0).
    bool ram_high;
    // LisaOS addresses VIA2 over the full $D800-$D9FF window (its ProFile
    // driver uses base $D801); MacWorks XL uses only the $D901 alias and
    // depends on the rest of that window staying unmapped, so the XL gets
    // the narrow region.
    uint32_t via2_base, via2_len;
    // FDC diskrom byte the boot ROM senses.  $A0 => iob_sony => the SONY
    // driver (boot-ROM SYSTYPE 1); left at 0 LisaOS mis-drives the floppy as
    // a Twiggy and never completes boot.  The Macintosh XL path (MacWorks
    // XL, iob_pepsi) keeps its empirically-correct 0: its loader-disk eject
    // sequence only matches with SYSTYPE 0 (revisit when MacWorks's own
    // machine-id handling is investigated).
    uint8_t fdc_diskrom;
} lisa_board_desc_t;

static const lisa_board_desc_t lisa_board = {
    .screen_w = LISA_SCREEN_W,
    .screen_h = LISA_SCREEN_H,
    .par_w = 2,
    .par_h = 3,
    .ram_high = true,
    .via2_base = 0xD800,
    .via2_len = 0x200,
    .fdc_diskrom = 0xA0,
};

static const lisa_board_desc_t macxl_board = {
    .screen_w = MACXL_SCREEN_W,
    .screen_h = MACXL_SCREEN_H,
    .par_w = 1,
    .par_h = 1,
    .ram_high = false,
    .via2_base = 0xD901,
    .via2_len = 16 * 8,
    .fdc_diskrom = 0, // iob_pepsi
};

static const lisa_board_desc_t *lisa_board_of(const config_t *cfg) {
    return (const lisa_board_desc_t *)cfg->machine->board;
}

static inline lisa_state_t *lisa_state(config_t *cfg) {
    return (lisa_state_t *)cfg->machine_context;
}

// ============================================================
// Video (direct 1bpp framebuffer in main RAM, base from the $E800 latch)
// ============================================================

// Point the display at the current framebuffer, which the Video Address Latch
// relocates anywhere in RAM (docs/reference/machines/lisa/lisa.md §8).  Re-read each frame so a latch
// write (the ROM moves the screen during sizing) takes effect.  Marks the
// framebuffer dirty only when the base actually moves.
static void lisa_refresh_framebuffer(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    const lisa_board_desc_t *board = lisa_board_of(cfg);
    const uint8_t *prev = ls->display.bits;
    uint32_t base = lisa_mmu_video_base(ls->mmu);
    // The latch can point the raster anywhere in RAM, so the base is checked
    // against installed RAM here rather than resting on lisa_mmu_video_base's
    // `base & (ram_size - 1)` fallback, which is only a bound because every
    // Lisa RAM size happens to be a power of two and the raster happens to be
    // smaller than the alignment.  A base the RAM cannot back
    // scans nothing -- lisa_display() then reports no display for that frame,
    // and the next latch write that lands in range brings it back.
    display_set_scanout(&ls->display, ram_native_pointer(cfg->mem_map, 0), memory_ram_size(cfg->mem_map), base,
                        board->screen_w / 8u, board->screen_w, board->screen_h, NULL, 0);
    ls->display.fb_dirty = true; // contents change every frame
    if (ls->display.bits != prev)
        ls->display.shape_dirty = true;
}

static void lisa_display_init(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    const lisa_board_desc_t *board = lisa_board_of(cfg);
    ls->display.width = board->screen_w;
    ls->display.height = board->screen_h;
    ls->display.stride = ls->display.width / 8;
    ls->display.format = PIXEL_1BPP_MSB;
    ls->display.par_w = board->par_w;
    ls->display.par_h = board->par_h;
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
// Interrupt routing (fixed 68000 IPL levels — docs/reference/machines/lisa/lisa.md §7.1)
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
    cpu_reschedule(cfg->scheduler);
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
// keyboard.press / down / up -> the Lisa COPS, by ADB keycode.
//
// lisa.md §11.3 documents the COPS byte as `d rrr nnnn` with d=1 down / d=0
// up, and the boot ROM's ReadKey does `TST.B D0 / BPL.S ReadKey` to SKIP up
// transitions -- which only makes sense because they arrive.  Until
// 2026-09-21 the up leg was a no-op and only a raw wire byte was accepted, so
// `keyboard.press("return")` failed with "unknown key" and no chord could
// hold a modifier.
static int lisa_input_key(config_t *cfg, int adb_code, bool down) {
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->cops)
        return -1;
    uint8_t code = lisa_keycode_for_adb(adb_code);
    if (code == LISA_NO_KEY)
        return -1; // a key this keyboard does not have
    if (ls->key_held[adb_code] == down)
        return 0; // no transition: a repeat, or an up without its down
    ls->key_held[adb_code] = down;
    cops_inject_key(ls->cops, (uint8_t)(down ? (code | 0x80) : (code & 0x7F)));
    return 0;
}

// A raw COPS byte, direction bit included -- `raw 0xC8` sends $C8 and nothing
// else.  The boot-menu and Xenix-install rows drive the wire this way because
// that is what they are testing; it is not a portable key press, and the
// substrate hook is NULL on every other machine.
static int lisa_input_key_raw(config_t *cfg, uint8_t byte) {
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->cops)
        return -1;
    cops_inject_key(ls->cops, byte);
    return 0;
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
// AND a level-1 interrupt (docs/reference/machines/lisa/lisa.md §7.1/§13: the floppy shares IPL 1 with
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
        return false; // only drive 0 exists; the others hold nothing
    return ls && ls->fdc && lisa_fdc_disk_present(ls->fdc);
}

// hw_profile_t.media_attach.  The Lisa has no cfg->floppy/cfg->scsi, so the
// std core implementation covers nothing here: the Sony disk lives in the
// 6504A FDC (owned by cfg->images) and the hard disk is the parallel ProFile
// (which owns its image itself, hence attach_image).
static int lisa_media_attach(config_t *cfg, const media_slot_t *slot) {
    lisa_state_t *ls = lisa_state(cfg);
    switch (slot->bus) {
    case MEDIA_BUS_FLOPPY:
        if (lisa_fd_insert(cfg, slot->unit, slot->img) != 0)
            return -1;
        add_image(cfg, slot->img);
        return 0;
    case MEDIA_BUS_PROFILE:
        if (!ls || !ls->profile || !lisa_profile_attach_image(ls->profile, slot->img))
            return -1;
        lisa_profile_update_lines(cfg); // drive OCD//BSY so the ROM sees the drive
        return 0;
    default:
        return -1; // no SCSI bus on a Lisa
    }
}

// The runtime attach/eject verbs' view: the one Sony drive and the ProFile.
static bool lisa_media_present(config_t *cfg, media_bus_t bus, int unit) {
    lisa_state_t *ls = lisa_state(cfg);
    switch (bus) {
    case MEDIA_BUS_FLOPPY:
        return unit == 0 && ls && ls->fdc && lisa_fdc_disk_present(ls->fdc);
    case MEDIA_BUS_PROFILE:
        return ls && ls->profile && lisa_profile_attached(ls->profile);
    default:
        return false;
    }
}

static int lisa_media_eject(config_t *cfg, media_bus_t bus, int unit) {
    lisa_state_t *ls = lisa_state(cfg);
    if (!lisa_media_present(cfg, bus, unit))
        return -1;
    if (bus == MEDIA_BUS_FLOPPY) {
        lisa_fdc_eject(ls->fdc);
        return 0;
    }
    lisa_profile_detach(ls->profile);
    lisa_profile_update_lines(cfg);
    return 0;
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

static DEF_METHOD(lisa_fd_drive_insert) {
    bool writable = (argc >= 2) ? argv[1].b : false;
    return val_bool(system_fd_insert(argv[0].s, 0, writable) == 0);
}

static DEF_METHOD(lisa_fd_drive_eject) {
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

static DEF_GETTER(lisa_fd_drive_present) {
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return val_bool(ls && ls->fdc && lisa_fdc_disk_present(ls->fdc));
}

static DEF_GETTER(lisa_fd_drive_index) {
    return val_int(0);
}

static const value_t lisa_false = {.kind = V_BOOL, .b = false};
static const value_t lisa_true = {.kind = V_BOOL, .b = true};
static const arg_decl_t lisa_fd_insert_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .doc = "Host path or storage URI of the image to mount"},
    {.name = "writable",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lisa_false,
     .doc = "Mount writable"},
};

static const member_t lisa_fd_drive_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .doc = "Drive number on the Sony floppy controller (0 = upper, 1 = lower on a Lisa 2/10)",
     .attr = {.type = V_INT, .get = lisa_fd_drive_index}                                              },
    {.kind = M_ATTR,
     .name = "present",
     .doc = "True when a disk is clamped in this drive",
     .attr = {.type = V_BOOL, .get = lisa_fd_drive_present}                                           },
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Eject the disk (unclamp)",
     .method = {.result = V_NONE, .fn = lisa_fd_drive_eject}                                          },
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Mount a disk image into the Sony drive",
     .method = {.args = lisa_fd_insert_args, .nargs = 2, .result = V_BOOL, .fn = lisa_fd_drive_insert}},
};
static const class_desc_t lisa_fd_drive_class = {
    .name = "floppy_drive", .members = lisa_fd_drive_members, .n_members = 4};

static struct object *lisa_fd_drives_get(struct object *self, int index) {
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return (ls && index == 0) ? ls->fd_drive_obj : NULL;
}
static const collection_desc_t lisa_fd_drives_entries = {
    .entry = &lisa_fd_drive_class, .by_index = {.get = lisa_fd_drives_get, .slots = 1}
};

static const member_t lisa_fd_drives_members[] = {
    OBJ_ENTRIES(&lisa_fd_drives_entries, NULL),
};
static const class_desc_t lisa_fd_drives_class = {
    .name = "floppy_drives", .doc = "Floppy drives, by index", .members = lisa_fd_drives_members, .n_members = 1};

static const member_t lisa_fd_members[] = {0}; // container only; the drives collection is the child
static const class_desc_t lisa_fd_class = {
    .name = "floppy", .members = NULL, .n_members = 0, .doc = "Floppy controller and drive"};

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
    // (machine.floppy.drive[N]).
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

static DEF_METHOD(lisa_hd_attach) {
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    // Read by kind: `path` now has a V_NONE default so that
    // `profile.attach(writable=false)` -- a blank in-memory disk, mounted
    // read-only -- is expressible at all.
    const char *path = (argc >= 1 && argv[0].kind == V_STRING) ? argv[0].s : NULL; // NULL = blank in-memory disk
    bool writable = (argc >= 2 && argv[1].kind == V_BOOL) ? argv[1].b : true;
    if (!ls || !ls->profile)
        return val_err("profile: no controller");
    if (!lisa_profile_attach(ls->profile, path, writable))
        return val_err("profile.attach: cannot open '%s'", path ? path : "(blank)");
    lisa_profile_update_lines(cfg);
    return val_bool(true);
}

static DEF_METHOD(lisa_hd_detach) {
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->profile || !lisa_profile_attached(ls->profile))
        return val_err("profile: no disk attached");
    lisa_profile_detach(ls->profile);
    lisa_profile_update_lines(cfg);
    return val_none();
}

static DEF_GETTER(lisa_hd_present) {
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    return val_bool(ls && lisa_profile_attached(ls->profile));
}

static DEF_METHOD(lisa_hd_save) {
    config_t *cfg = (config_t *)object_data(self);
    lisa_state_t *ls = lisa_state(cfg);
    if (!ls || !ls->profile || !lisa_profile_attached(ls->profile))
        return val_err("profile: no disk attached");
    const char *path = (argc >= 1) ? argv[0].s : NULL;
    if (!path || !*path)
        return val_err("profile.save: a destination path is required");
    // An I/O job (io_leaf.h): the consolidated 532-bytes/block disk = base
    // merged with the delta, streamed on the I/O worker from a snapshot
    // taken here; image_export refuses to overwrite an existing file.
    return io_leaf_export_image(lisa_profile_image(ls->profile), path, "profile.save");
}

static const arg_decl_t lisa_hd_save_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .doc = "Destination path for the consolidated single-file ProFile image"},
};

// The Lisa's battery-backed parameter memory ($FCC181 in the FDC shared RAM)
// holds the OS's boot volume + device-configuration table.  The installer adds
// the ProFile to that table at clean shutdown ("Finished" -> turn off / start
// up); persisting it lets an installed system boot from the ProFile on a later
// (cold) launch.  Load before booting (the ROM reads PRAM during startup).
// Re-seed the parameter memory in the model, the way the owning device does
// at construction: factory defaults, an EMPTY device table, and a checksum
// computed by lisa_pram_checksum().  `boot_vol` is the BootVol nibble
// (pram.md §4): 1 = built-in Sony floppy, 2 = the parallel-port
// ProFile.
//
// This replaces loading a 64-byte image synthesised outside the emulator.
// The device table stays empty deliberately: the OS's INIT_CONFIG restores it
// from the boot volume's own MDDF snapshot, which is what real hardware does
// and which makes the boot depend on the disk image actually carrying a good
// clean-shutdown snapshot rather than on a pre-seeded hardware entry masking
// a broken one.
static DEF_METHOD(lisa_hd_pram_init) {
    lisa_state_t *ls = lisa_state((config_t *)object_data(self));
    if (!ls || !ls->fdc)
        return val_err("pram: no controller");
    uint64_t boot_vol = (argc >= 1) ? argv[0].u : 1;
    if (boot_vol > 15)
        return val_err("pram_init: boot_vol must be 0..15 (see pram.md §4)");
    bool valid = (argc >= 2) ? (argv[1].b != 0) : true;
    bool installed = (argc >= 3) ? (argv[2].b != 0) : false;
    lisa_fdc_pram_init(ls->fdc, (uint8_t)boot_vol, valid, installed);
    return val_bool(true);
}

static DEF_METHOD(lisa_hd_pram_save) {
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

static DEF_METHOD(lisa_hd_pram_load) {
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

// The documented defaults, declared rather than only written in the doc string
// and re-applied in the body -- without them, naming `valid` or `installed`
// failed with "missing argument 'boot_vol'".
static const value_t pram_def_boot_vol = {.kind = V_UINT, .u = 1};
static const value_t pram_def_valid = {.kind = V_BOOL, .width = 1, .b = true};

static const arg_decl_t lisa_hd_pram_init_args[] = {
    {.name = "boot_vol",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &pram_def_boot_vol,
     .doc = "BootVol nibble: 1 = built-in Sony floppy, 2 = parallel-port ProFile (pram.md §4)"},
    {.name = "valid",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &pram_def_valid,
     .doc = "true = a verifying checksum; false = a fresh battery, so the OS rebuilds the device table "
            "from the boot volume's MDDF snapshot"},
    {.name = "installed",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "true = also pack the LOS 3.1 installer's device table (ProFile as cd_paraport); needed only for a "
            "volume installed onto but not yet cleanly shut down"},
};

static const arg_decl_t lisa_hd_pram_args[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Parameter-memory (PRAM) file path"},
};

static const arg_decl_t lisa_hd_attach_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Host path of the ProFile image, created blank if missing (omit for a blank in-memory disk)"},
    {.name = "writable",
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lisa_true,
     .doc = "Mount writable"                                                                            },
};

static const member_t lisa_hd_members[] = {
    {.kind = M_ATTR,
     .name = "present",
     .doc = "True when a ProFile image is attached to the parallel port",
     .attr = {.type = V_BOOL, .get = lisa_hd_present}                                                                                                                                                            },
    {.kind = M_METHOD,
     .name = "detach",
     .doc = "Flush and disconnect the ProFile",
     .method = {.result = V_NONE, .fn = lisa_hd_detach}                                                                                                                                                          },
    {.kind = M_METHOD,
     .name = "attach",
     .doc = "Attach a ProFile image (created blank if missing; omit path for a blank in-memory disk)",
     .method = {.args = lisa_hd_attach_args, .nargs = 2, .result = V_BOOL, .fn = lisa_hd_attach}                                                                                                                 },
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Write the current ProFile contents to a new self-contained single-file image (consolidated; not a "
            "base+delta pair)",                                                                        .method = {.ui_flags = MM_IO, .args = lisa_hd_save_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_save}},
    {.kind = M_METHOD,
     .name = "pram_init",
     .doc = "Seed the parameter memory in the model: BootVol nibble, checksum validity, and optionally the LOS "
            "installer's device table",                                                                .method = {.args = lisa_hd_pram_init_args, .nargs = 3, .result = V_BOOL, .fn = lisa_hd_pram_init}         },
    {.kind = M_METHOD,
     .name = "pram_save",
     .doc = "Save the machine parameter memory (battery-backed NVRAM at $FCC181) to a file",
     .method = {.args = lisa_hd_pram_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_pram_save}                                                                                                                },
    {.kind = M_METHOD,
     .name = "pram_load",
     .doc = "Load the machine parameter memory from a file (call before booting)",
     .method = {.args = lisa_hd_pram_args, .nargs = 1, .result = V_BOOL, .fn = lisa_hd_pram_load}                                                                                                                },
};
static const class_desc_t lisa_hd_class = {.name = "profile",
                                           .doc = "The ProFile hard disk on the parallel port, and its PRAM",
                                           .members = lisa_hd_members,
                                           .n_members = 6};

static void lisa_register_profile_object(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    // Named "hd" (not "profile") under the machine node: a child named
    // "profile" would be shadowed by the machine class's `profile` *method*
    // (the resolver finds members before attached children). The ProFile is
    // the Lisa's hard disk, so machine.hd reads correctly.
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
static DEF_METHOD(lisa_power_off) {
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
static const class_desc_t lisa_power_class = {
    .name = "power", .doc = "The Lisa's soft power switch", .members = lisa_power_members, .n_members = 1};

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

MACHINE_PART_SAVE(lisa_mmu_checkpoint, lisa_mmu_t)
MACHINE_PART_SAVE(cops_checkpoint, cops_t)
MACHINE_PART_SAVE(lisa_fdc_checkpoint, lisa_fdc_t)
MACHINE_PART_SAVE(lisa_profile_checkpoint, lisa_profile_t)

static int lisa_init(config_t *cfg, checkpoint_t *checkpoint) {
    lisa_state_t *ls = (lisa_state_t *)malloc(sizeof(lisa_state_t));
    if (!ls) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    memset(ls, 0, sizeof(*ls));
    cfg->machine_context = ls;

    // 24-bit address space, configured RAM, 16 KB interleaved boot ROM.
    machine_part_begin(cfg, checkpoint, "memory");
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size,
                                   MEMORY_BUS_ERR_NONE, &cfg->build_opts.rom, checkpoint); // no bus-error watchdog
    machine_part(cfg, checkpoint, "memory", part_save_memory, cfg->mem_map);

    // The profile is the source of truth for the CPU model, as it is for the
    // clock below and as mac030_build_core states for the II families.  Both
    // profiles behind this substrate declare 68000, so this reads back exactly
    // what the constant said -- but system_create derives cfg->cpu_arch from
    // the profile unconditionally, so a profile that ever disagreed with a
    // hardcoded core here would tag the machine with an arch it is not running.
    machine_part_begin(cfg, checkpoint, "cpu");
    cfg->cpu = cpu_init(cfg->machine->cpu_model, checkpoint);
    machine_part(cfg, checkpoint, "cpu", part_save_cpu, cfg->cpu);
    machine_part_begin(cfg, checkpoint, "scheduler");
    sched_cpu_if_t cpu_if = cpu_sched_if(cfg->cpu); // the 68K main-CPU seam adapter
    cfg->scheduler = scheduler_init(&cpu_if, checkpoint);
    machine_part(cfg, checkpoint, "scheduler", part_save_scheduler, cfg->scheduler);
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

    machine_part_irq(cfg, checkpoint);

    // The segment MMU owns all translation; it reads/writes directly into the
    // flat RAM+ROM image the memory map allocated, ROM already in place.
    bool ram_high = lisa_board_of(cfg)->ram_high;
    machine_part_begin(cfg, checkpoint, "lisa_mmu");
    ls->mmu =
        lisa_mmu_init(ram_native_pointer(cfg->mem_map, 0), cfg->ram_size, (uint8_t *)memory_rom_bytes(cfg->mem_map),
                      memory_rom_size(cfg->mem_map), ram_high, checkpoint);
    machine_part(cfg, checkpoint, "lisa_mmu", lisa_mmu_checkpoint_part, ls->mmu);
    memory_map_set_lisa_mmu(cfg->mem_map, ls->mmu); // the slow path's g_lisa_mmu
    lisa_mmu_attach_object(ls->mmu, cfg->cpu); // machine.cpu.mmu, like every MMU kind
    lisa_mmu_set_nmi(ls->mmu, lisa_parity_nmi, cfg); // level-7 parity NMI (PARTST)
    lisa_mmu_set_clock(ls->mmu, cfg->scheduler); // cycle source for the retrace status bit
    lisa_mmu_set_vbl_ack(ls->mmu, lisa_vbl_ack, cfg); // Status-Register read acks the latched VBL

    // Two 6522 VIAs (reused unchanged).  map=NULL: the machine registers the
    // interface itself.  freq_factor 4 = 68000/4 ≈ 1.27 MHz (docs/reference/machines/lisa/lisa.md §10).
    //
    // Deliberately NOT via_freq_factor_for_clock(): that helper divides by the
    // 783.36 kHz φ2 every Macintosh 6522 runs at, and would return 7 here.  The
    // Lisa's VIAs are clocked from the CPU at /4, a different quantity, so this
    // is the one family where a literal is the correct answer rather than a
    // stale one.
    machine_part_begin(cfg, checkpoint, "via1");
    cfg->via1 =
        via_init(NULL, cfg->scheduler, 4, "via1", lisa_via1_output, lisa_via_shift_out, lisa_via1_irq, cfg, checkpoint);
    machine_part(cfg, checkpoint, "via1", part_save_via, cfg->via1);
    machine_part_begin(cfg, checkpoint, "via2");
    cfg->via2 =
        via_init(NULL, cfg->scheduler, 4, "via2", lisa_via2_output, lisa_via_shift_out, lisa_via2_irq, cfg, checkpoint);
    machine_part(cfg, checkpoint, "via2", part_save_via, cfg->via2);

    // Register each VIA at its Lisa physical I/O base through the stride
    // adapter.  16 registers: VIA1 spans 16*2=32 bytes from $DD81, VIA2 uses an
    // 8-byte register stride.  The canonical VIA2 base is $D901 (docs/reference/machines/lisa/lisa.md
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
    lisa_mmu_map_io(ls->mmu, lisa_board_of(cfg)->via2_base, lisa_board_of(cfg)->via2_len, &lisa_via_iface,
                    &ls->via2_map);

    // COPS keyboard/mouse/clock/power microcontroller on VIA1 port A.
    machine_part_begin(cfg, checkpoint, "cops");
    ls->cops = cops_init(cfg->via1, cfg->scheduler, checkpoint);
    machine_part(cfg, checkpoint, "cops", cops_checkpoint_part, ls->cops);

    // Intelligent floppy controller: 6504 + 1 KB shared RAM on the ODD bus
    // bytes of physical $00C000-$00C7FF (docs/reference/machines/lisa/lisa.md §13).  Mapped from the
    // even base $00C000 so word/long accesses at the even base (used by Xenix's
    // boot loader) reach the controller; the iface models the odd-byte RAM.
    // FDIR completion is signalled on VIA1 PB4.
    // The image list lands before the FDC and the ProFile, both of which
    // reference it.
    machine_part_images(cfg, checkpoint);

    machine_part_begin(cfg, checkpoint, "lisa_fdc");
    ls->fdc = lisa_fdc_init(cfg->scheduler, lisa_fdc_fdir, cfg, checkpoint, CONFIG_IMAGES(cfg));
    machine_part(cfg, checkpoint, "lisa_fdc", lisa_fdc_checkpoint_part, ls->fdc);
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
    if (lisa_board_of(cfg)->fdc_diskrom)
        lisa_fdc_set_diskrom(ls->fdc, lisa_board_of(cfg)->fdc_diskrom);

    // Expose the Sony drive so disks can be inserted/ejected at runtime
    // (floppy.drives[0].insert / .eject), e.g. swapping the MacWorks loader disk
    // for its system disk.
    lisa_register_floppy_object(cfg);

    // ProFile parallel hard disk on VIA2: control lines via the port-B output
    // callback (lisa_via2_output), data via the port-A hooks, BSY back to PB1/CA1.
    machine_part_begin(cfg, checkpoint, "profile");
    ls->profile = lisa_profile_init(cfg->scheduler, lisa_profile_bsy, cfg, checkpoint);
    machine_part(cfg, checkpoint, "profile", lisa_profile_checkpoint_part, ls->profile);
    via_set_porta_hooks(cfg->via2, lisa_profile_porta_read_cb, lisa_profile_porta_write_cb, cfg);
    lisa_profile_update_lines(cfg); // no disk yet → OCD/ high (disconnected)
    lisa_register_profile_object(cfg);
    lisa_register_power_object(cfg); // soft power-off switch (COPS) → `power.off`

    // Z8530 SCC (reused as-is).  Its chip select is the whole Serial Ports
    // Control block, physical $00D000-$00D3FF (Lisa Hardware Manual 1983,
    // Fig. 2-5), and only A1 (A/B) and A2 (D/C) reach the chip, so every
    // 8-byte mirror is the same four registers: the boot ROM uses
    // $00D241/43/45/47, the OS's RS-232 driver $00D201/03/05/07
    // (docs/reference/machines/lisa/lisa.md §15).  PCLK 4 MHz (chan A) / 3.6864 MHz
    // (chan B).  Autovectored at IPL 6.
    machine_part_begin(cfg, checkpoint, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, lisa_scc_irq, cfg, checkpoint);
    machine_part(cfg, checkpoint, "scc", part_save_scc, cfg->scc);
    scc_set_clocks(cfg->scc, 4000000, 3686400);
    lisa_mmu_map_io(ls->mmu, 0xD000, 0x400, (memory_interface_t *)scc_get_memory_interface(cfg->scc), cfg->scc);

    // Serial A's handshake: the OS's RS-232 driver holds port A output
    // until DSR, which the Lisa wires to the SCC's /SYNC input and reads as
    // RR0 bit 4 set (source-rs232: xmtrr0 := $10 for channel 0).  A device
    // on the cable -- `machine.scc.a.output` set to a host file -- raises
    // it; with nothing attached the driver reports the printer not ready.
    // Port B (AppleBus, and the boot ROM's loopback self-test) is left as
    // it was.
    scc_set_port_ready_line(cfg->scc, 0, SCC_PIN_SYNC, true);
    // The ImageWriter the Office System prints to, on Serial A or B
    machine_part_imagewriter(cfg, checkpoint, true);

    lisa_display_init(cfg);
    scheduler_new_event_type(cfg->scheduler, "lisa", cfg, "vbl_off", &lisa_vbl_off);
    scheduler_new_event_type(cfg->scheduler, "lisa", cfg, "nmi_off", &lisa_nmi_off);

    cfg->debugger = debug_init();

    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
    return 0;
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
    lisa_state_t *ls = lisa_state(cfg);
    if (ls && ls->mmu) {
        lisa_mmu_delete(ls->mmu);
        ls->mmu = NULL;
    }

    // via1, via2, scc, the scheduler, the CPU, the memory map and the
    // debugger, in the one order documented once.
    // The Lisa builds no scsi and no rtc; the chain NULL-checks its way past
    // both.  Note it also deletes the scheduler LAST of those, which is what
    // keeps the device destructors' scheduler_forget_source calls valid.
    machine_teardown_config_devices(cfg);

    if (ls) {
        free(ls);
        cfg->machine_context = NULL;
    }
}

// ============================================================
// Checkpoint
// ============================================================

// ============================================================
// VBL
// ============================================================

// Pulse the Status Register vertical-retrace bit each frame so the ROM video
// test and the OS VBL handler observe a retrace.
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

// ~90 µs retrace window (docs/reference/machines/lisa/lisa.md §8) at 5.09375 MHz ≈ 458 cycles.  Holding
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
        // VBL is an IPL-1 interrupt source (docs/reference/machines/lisa/lisa.md §8) gated by the VTMSK
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

// One Sony 3.5" mechanism on the 6504A intelligent controller.  Lisa 1's
// Twiggy drives are out of scope.
//
// FLOPPY_800K, not FLOPPY_400K: `kind` names the HIGHEST format the drive
// serves and readers derive the rest (machine_profile.h), and this one serves
// both.  lisa_fdc_insert sizes the media itself -- num_sides = 2 above
// 500000 bytes -- and reports the geometry the boot loader reads from the
// controller's disk-type byte, which has an encoding for each: docs/internals/machines/
// lisa/lisa.md 13.2 records $FCC015 as "bit 0 set = Sony 400 KB single-sided
// (800 blocks); bit 0 clear = Sony 800 KB double-sided (1600 blocks)".  So
// 800 KB media is something the machine's own firmware protocol contemplates,
// not something the model invented.
//
// Untested, though: every Lisa image in the tree is 400 KB, so the two-sided
// branch has never run under a test.  Declaring 400K was the stronger claim
// to have wrong -- it understated a drive the model demonstrably serves.
static const struct floppy_slot lisa_floppy_slots[] = {
    {.label = "Internal floppy drive", .kind = FLOPPY_800K},
    {0},
};

// The Lisa hard disk is parallel-port ProFile/Widget, NOT SCSI: one unit on
// the parallel port, the device lisa_profile.c models.  On the Lisa 2 it is
// the external ProFile; the Macintosh XL carries its disk inside.
static const storage_bay_decl_t macxl_profile_bay[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {0},
};

#define LISA_PROFILE_PORT(bays_, external_)                                                                            \
    {.id = "profile",                                                                                                  \
     .label = "ProFile port",                                                                                          \
     .kind = STORAGE_KIND_PROFILE,                                                                                     \
     .media_bus = MEDIA_BUS_PROFILE,                                                                                   \
     .units = 0x1u,                                                                                                    \
     .external_connector = (external_),                                                                                \
     .bays = (bays_),                                                                                                  \
     .accepts = STORAGE_DEV_HD,                                                                                        \
     .startup_ok = true}

static const storage_bus_decl_t lisa_storage[] = {
    LISA_PROFILE_PORT(NULL, true),
    {0},
};

static const storage_bus_decl_t macxl_storage[] = {
    LISA_PROFILE_PORT(macxl_profile_bay, false),
    {0},
};

static const storage_device_decl_t lisa_default_storage[] = {
    {.bus = "profile", .unit = 0, .type = STORAGE_DEV_HD},
    {0},
};

// The built-in 12" screen: one monitor, nothing to choose.
static bool lisa_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "lisa_12in";
    return true;
}

static const builtin_video_desc_t lisa_builtin_video = {
    .detail = "frame buffer in main RAM",
    .monitor_at = lisa_monitor_at,
    .default_monitor = "lisa_12in",
};

// The seeding step: with the startup device on the ProFile, parameter memory
// says BootVol = 2 (the parallel-port ProFile) with the checksum left NOT
// verifying -- a Lisa whose battery was just replaced.  The boot ROM then
// goes to the ProFile instead of stopping at its startup-device screen, and
// the OS restores its device table from the boot volume's own snapshot.
// "No default" leaves the factory content: BootVol = the floppy.
static void lisa_seed(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    const machine_startup_t *st = &cfg->build_opts.startup;
    if (!ls || !ls->fdc || !cfg->build_opts.storage_given || st->none || strcmp(st->bus, "profile") != 0)
        return;
    lisa_fdc_pram_init(ls->fdc, 2, false, false);
}

// A power cycle's power-on-only half (machine_profile.h): the MMU's START
// latch comes back set, which is how the 68000's vector fetch reaches the boot
// ROM, and the descriptor RAM loses its contents -- without that the ROM's
// warm-start check (segment 126 still reading $x901) would take the reset path
// into the ROM monitor instead of a cold start.  A reset touches neither.
static void lisa_power_on(config_t *cfg) {
    lisa_state_t *ls = lisa_state(cfg);
    if (ls)
        lisa_mmu_power_on(ls->mmu);
}

// Apple Lisa 2 hardware profile.
static const machine_substrate_t lisa_substrate = {
    .init = lisa_init,
    .power_on = lisa_power_on,
    .teardown = lisa_teardown,
    .seed = lisa_seed,
    .trigger_vbl = lisa_trigger_vbl,
    .display = lisa_display,
    .fd_insert = lisa_fd_insert,
    .fd_present = lisa_fd_present,
    .input_key = lisa_input_key,
    .input_key_raw = lisa_input_key_raw,
    // The COPS response FIFO is 32 bytes (cops.c COPS_FIFO) and carries mouse
    // reports as well as keys, so keyboard.type gets a quarter of the ADB
    // budget and leaves the rest as headroom.
    .key_queue_bytes = 24,
    .input_mouse_move = lisa_input_mouse_move,
    .input_mouse_button = lisa_input_mouse_button,
    .media_attach = lisa_media_attach,
    .media_present = lisa_media_present,
    .media_eject = lisa_media_eject,
};

const hw_profile_t machine_lisa = {
    .name = "Lisa 2",
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
    .storage = lisa_storage,
    .default_storage = lisa_default_storage,
    .builtin_video = &lisa_builtin_video,

    .board = &lisa_board,
    .substrate = &lisa_substrate,
};

// Macintosh XL: the same Lisa 2 hardware sold with the "3A" boot ROM and the
// screen-mod (square-pixel) kit, running MacWorks XL.  For the emulator it is
// the Lisa 2 profile with a different ROM-compatibility id and name — the chip
// models, callbacks, and 720×364 framebuffer are identical (the square-pixel
// kit only changes the dot clock, which a frame-accurate model ignores).
// See docs/reference/machines/lisa/lisa.md §1.1.
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
    .storage = macxl_storage,
    .default_storage = lisa_default_storage,
    .builtin_video = &lisa_builtin_video,

    .board = &macxl_board,
    .substrate = &lisa_substrate,
};
