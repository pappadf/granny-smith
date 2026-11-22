// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu.c
// The one MDU+RBV-family substrate — see mdu.h.  IIci and IIsi share this; the
// reset/teardown/checkpoint/VBL lifecycle is the former per-machine clones,
// unified (the only IIci/IIsi delta is the IIsi's Egret, handled by an
// `if (st->egret)` — IIci leaves egret NULL in the unified mdu_state_t).

#include "mdu.h"
#include "appletalk.h"
#include "log.h"

#include "config_seed.h"
#include "m68k_board.h" // shared core/finish/reset/irq/build_mmu + board desc
#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "machine_teardown.h" // the shared config_t-owned delete chain
#include "mdu_io.h" // mdu_state_t + mdu_io_bind

#include "adb.h"
#include "asc.h"
#include "builtin_rbv_video.h"
#include "checkpoint.h"
#include "cpu.h"
#include "debug.h"
#include "egret.h"
#include "floppy.h"
#include "image.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "mmu.h"
#include "mmu_checkpoint.h"
#include "nubus.h"
#include "rbv.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>

// The machine-level category every family substrate logs under (915a7ea).
LOG_USE_CATEGORY_NAME("board");

static inline const mdu_board_t *mdu_board(config_t *cfg) {
    return (const mdu_board_t *)cfg->machine->board;
}

static inline mdu_state_t *mdu_st(config_t *cfg) {
    return (mdu_state_t *)cfg->machine_context;
}

int mdu_init(config_t *cfg, checkpoint_t *cp, const mdu_board_t *board) {
    mdu_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    // Shared II-family core (memory_map, cpu-from-profile, scheduler) + RTC + SCC +
    // VIA1.  Note: no VIA2 (the RBV replaces it), and rtc_set_via is left to the
    // machine (IIci bit-bangs the RTC on VIA1; the IIsi drives it via Egret).
    m68k_board_build_core(cfg, board->desc, cp);
    machine_part_irq(cfg, cp);

    m68k_board_build_lowspeed(cfg, cp, NULL); // NULL: the family-default SCC IRQ

    // Derived from the CPU clock, not hardcoded: this substrate serves the
    // 25 MHz IIci and the 20 MHz IIsi, so a single literal is wrong for one of
    // them.  It used to be 20 — correct for the 16 MHz IIcx this code was
    // adapted from, and 1.6x too fast on a IIci, which is what made MacTest's
    // VIA timer test overshoot its interrupt-count window.
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_freq_factor_for_clock(cfg->machine->freq), "via1",
                         board->via1_output, board->via1_shift_out, m68k_board_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    // Exact-rational phi2: the integer divisor above rounds, and on this
    // substrate that rounding is not negligible -- the IIsi lands 1.80% slow, the IIci 0.27%.  via_set_exact_clock
    // installs ticks = cycles x 783360/cpu_hz reduced, which is what the
    // PowerPC families already do.
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // Everything machine-specific (straps, ADB/Egret, SCSI, ASC, SWIM, RBV, MMU,
    // NuBus video, mdu_io_bind, bus-error, memory layout, checkpoint restore).
    if (board->build_devices(cfg, cp) != 0)
        return -1;

    m68k_board_finish(cfg, cp, &st->mdu_io);
    return 0;
}

MACHINE_PART_SAVE_EXTERN(rbv, rbv_checkpoint, rbv_t)

static int mdu_substrate_init(config_t *cfg, checkpoint_t *cp) {
    if (mdu_init(cfg, cp, mdu_board(cfg)) != 0)
        return -1;
    return 0;
}

// The MDU board's /RESET net: the overlay and the common devices (the
// shared GLUE-family half), plus the RBV, which stands where VIA2 does.
static void mdu_bus_reset(config_t *cfg) {
    mdu_state_t *st = mdu_st(cfg);
    m68k_board_bus_reset(cfg, &st->overlay);
    rbv_reset(st->rbv);
}

// A power cycle's power-on-only half (machine_profile.h): the ASC's Power On
// Clear.
static void mdu_power_on(config_t *cfg) {
    asc_power_on(mdu_st(cfg)->asc);
}

// MDU delete-chain (no VIA2; RBV instead; Egret on the IIsi): egret → rbv →
// mmu → asc, then the shared chain from floppy and adb on (machine_teardown.h).
// Egret goes before the adb it was handed.
static void mdu_teardown(config_t *cfg) {
    mdu_state_t *st = mdu_st(cfg);
    if (st) {
        if (st->egret) {
            egret_delete(st->egret);
            st->egret = NULL;
        }
        if (st->rbv) {
            rbv_delete(st->rbv);
            st->rbv = NULL;
        }
        if (st->mmu) {
            mmu_delete(st->mmu);
            st->mmu = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
    }
    // The config_t-owned devices, in the family-shared canonical order
    // (machine_teardown.h).  Was a byte-identical copy in five families.
    machine_teardown_config_devices(cfg);
}

// substrate.nubus_slot_irq — the RBV aggregates NuBus slot interrupts itself
// (RvSInt & RvSEnb -> RvAnySlot -> the chip's combined interrupt -> IPL 2), so a
// slot source has to go to the chip, not to a generic IPL setter.
//
// Worth keeping as history, because it is why the generic path no longer
// exists.  nubus.c once dispatched families like this one through a shared
// shim that passed `1 << (slot - 9)` as the machine's IRQ SOURCE mask -- but
// on this family every one of those bits is already spoken for:
// MDU_IRQ_VIA1/RBV/SCC/NMI are 1<<0 .. 1<<3.  So a card in slot $C asserted
// the NMI source and the machine took a level-7 autovector every few
// instructions forever -- which is what "a 24AC beside the live built-in RBV
// hangs the boot at Welcome" actually was.  Slot numbering only
// coincidentally matches a machine's interrupt-source numbering; every family
// now converts it itself.
//
// RvSInt numbering is logical: 0 is the built-in video (RvIRQ0, bit 6) and
// 1..6 are RvIRQ1..6, so NuBus $9..$E map to 1..6.
static void mdu_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    mdu_state_t *st = mdu_st(cfg);
    if (!st || !st->rbv || slot < 0x9 || slot > 0xE)
        return;
    int logical = slot - 0x8;
    if (active)
        rbv_assert_slot_irq(st->rbv, logical);
    else
        rbv_clear_slot_irq(st->rbv, logical);
}

// ============================================================
// RBV / SCSI / sound wiring (IIci and IIsi)
// ============================================================

// RBV combined interrupt -> 68030 IPL 2 (replaces the GLUE VIA2 source).
static void mdu_rbv_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, MDU_IRQ_RBV, active);
}

void mdu_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "%s soft power-off", cfg->machine->name);
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

// RBV video on/off (RvMonP bit 6) -> blank or unblank the raster.
static void mdu_rbv_blank(void *context, bool video_off) {
    builtin_rbv_video_set_blank(mdu_st((config_t *)context)->video_card, video_off);
}

// RBV depth change (RvMonP bits 0-2) -> reshape the built-in video display.
static void mdu_rbv_mode(void *context, int depth_code) {
    builtin_rbv_video_set_depth(mdu_st((config_t *)context)->video_card, depth_code);
}

void mdu_scsi_irq(void *context, bool irq, bool drq) {
    mdu_state_t *st = mdu_st((config_t *)context);
    // The 5380 is built before the RBV: nothing to flag until it exists
    if (!st->rbv)
        return;
    rbv_set_scsi_irq(st->rbv, irq);
    rbv_set_scsi_drq(st->rbv, drq);
}

// ASC interrupt output -> RBV sound flag (RvSndIRQ, RvIFR bit 4).  The RBV
// emulates the VIA2-CB1 dispatch slot the shared OS interrupt code expects
// (jASCInt = Via2DT+4*ifCB1); level 2 like every VIA2/RBV sound source.  The
// V8 mirrors the RBV's VsSndIRQ bit layout.
static void mdu_asc_irq(void *context, bool active) {
    rbv_set_snd_irq((rbv_t *)context, active);
}

void mdu_wire_rbv(config_t *cfg) {
    mdu_state_t *st = mdu_st(cfg);
    rbv_set_irq_callback(st->rbv, mdu_rbv_irq, cfg);
    rbv_set_power_off_callback(st->rbv, mdu_power_off, cfg);
    rbv_set_mode_callback(st->rbv, mdu_rbv_mode, cfg);
    rbv_set_blank_callback(st->rbv, mdu_rbv_blank, cfg);
    asc_set_irq_handler(st->asc, mdu_asc_irq, st->rbv); // sound IRQ -> RvIFR bit 4
}

// MDU VBL: single VIA1 CA1 pulse (no VIA2), then fan out to NuBus.
static void mdu_trigger_vbl(config_t *cfg) {
    via_pulse_c(cfg->via1, 0, 0); // VBL: CA1 low then high
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// The RBV's built-in video as a display device: the BUILTIN pseudo-slot's
// card supplies its monitors.  Its startup modes are its pseudo-slot's PRAM
// record, as each ROM writes it -- BoardID ($001F IIci, $0028 IIsi),
// savedMode, the monitor's sResource ($86 for the 13" RGB, $81 for the 15"
// Portrait) -- and honours a seeded one at 1, 2, 4 and 8 bpp on the 13" RGB
// and 1, 2 and 4 on the Portrait, which has no 8 bpp mode (measured).  With
// no Portrait row the New Machine dialog offered no mode for it (S1018-15).
#define RBV_STARTUP(board)                                                                                             \
    {                                                                                                                  \
        {.monitor = "13in_rgb",                                                                                        \
         .width = 640,                                                                                                 \
         .height = 480,                                                                                                \
         .record = {0x00, (board), 0x80, 0x86, 0, 0, 0, 0},                                                            \
         .modes = {{1, 0x80}, {2, 0x81}, {4, 0x82}, {8, 0x83}}},                                                       \
        {.monitor = "15in_portrait",                                                                                   \
         .width = 640,                                                                                                 \
         .height = 870,                                                                                                \
         .record = {0x00, (board), 0x80, 0x81, 0, 0, 0, 0},                                                            \
         .modes = {{1, 0x80}, {2, 0x81}, {4, 0x82}}},                                                                  \
        {0},                                                                                                           \
}
static const builtin_startup_t iici_startup[] = RBV_STARTUP(0x1F);
static const builtin_startup_t iisi_startup[] = RBV_STARTUP(0x28);

const builtin_video_desc_t mdu_builtin_video_iici = {
    .detail = "RBV",
    .slot_monitors = true,
    .startup_slot = 0xB,
    .startup = iici_startup,
};

const builtin_video_desc_t mdu_builtin_video_iisi = {
    .detail = "RBV",
    .slot_monitors = true,
    .startup_slot = 0xE,
    .startup = iisi_startup,
};

const machine_substrate_t mdu_substrate = {
    .init = mdu_substrate_init,
    .bus_reset = mdu_bus_reset,
    .power_on = mdu_power_on,
    .teardown = mdu_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = mdu_trigger_vbl,
    .nubus_slot_irq = mdu_nubus_slot_irq, // straight to the RBV's slot-interrupt register
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
};
