// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu.c
// The one MDU+RBV-family substrate — see mdu.h.  IIci and IIsi share this; the
// reset/teardown/checkpoint/VBL lifecycle is the former per-machine clones,
// unified (the only IIci/IIsi delta is the IIsi's Egret, handled by an
// `if (st->egret)` — IIci leaves egret NULL in the unified mac030_mdu_state_t).

#include "mdu.h"
#include "appletalk.h"

#include "mac030_glue.h" // shared core/finish/reset/irq/build_mmu + board desc
#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "mdu_io.h" // mac030_mdu_state_t + mdu_io_bind

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "debug.h"
#include "egret.h"
#include "floppy.h"
#include "image.h"
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

static inline const mac030_mdu_board_t *mdu_board(config_t *cfg) {
    return (const mac030_mdu_board_t *)cfg->machine->board;
}

static inline mac030_mdu_state_t *mdu_st(config_t *cfg) {
    return (mac030_mdu_state_t *)cfg->machine_context;
}

void mac030_mdu_init(config_t *cfg, checkpoint_t *cp, const mac030_mdu_board_t *board) {
    mac030_mdu_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    // Shared II-family core (mem_map, cpu-from-profile, scheduler) + RTC + SCC +
    // VIA1.  Note: no VIA2 (the RBV replaces it), and rtc_set_via is left to the
    // machine (IIci bit-bangs the RTC on VIA1; the IIsi drives it via Egret).
    mac030_build_core(cfg, cp);
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, mac030_glue_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    // AppleTalk rides the SCC's LocalTalk channel, so it is built as soon as
    // the SCC exists — and, because the checkpoint stream is positional, in
    // the same relative place the save writes it (right after scc_checkpoint).
    appletalk_init(cfg->scheduler, cfg->scc, cp);

    // Derived from the CPU clock, not hardcoded: this substrate serves the
    // 25 MHz IIci and the 20 MHz IIsi, so a single literal is wrong for one of
    // them.  It used to be 20 — correct for the 16 MHz IIcx this code was
    // adapted from, and 1.6x too fast on a IIci, which is what made MacTest's
    // VIA timer test overshoot its interrupt-count window (ledger §9).
    cfg->via1 = via_init(NULL, cfg->scheduler, via_freq_factor_for_clock(cfg->machine->freq), "via1",
                         board->via1_output, board->via1_shift_out, mac030_glue_via1_irq, cfg, cp);
    // Exact-rational phi2: the integer divisor above rounds, and on this
    // substrate that rounding is not negligible -- the IIsi lands 1.80% slow, the IIci 0.27%.  via_set_exact_clock
    // installs ticks = cycles x 783360/cpu_hz reduced, which is what the
    // PowerPC families already do.
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // Everything machine-specific (straps, ADB/Egret, SCSI, ASC, SWIM, RBV, MMU,
    // NuBus video, mdu_io_bind, bus-error, memory layout, checkpoint restore).
    board->build_devices(cfg, cp);

    mac030_glue_finish(cfg, cp);
}

static void mdu_init(config_t *cfg, checkpoint_t *cp) {
    mac030_mdu_init(cfg, cp, mdu_board(cfg));
}

static void mdu_reset(config_t *cfg) {
    mac030_mdu_state_t *st = mdu_st(cfg);
    mac030_glue_reset(cfg, &st->rom_overlay, mdu_board(cfg)->desc->rom_base, st->mmu);
}

// MDU delete-chain (no VIA2; RBV instead; Egret on the IIsi).  Order matches
// the former iici/iisi teardowns: scheduler_stop → egret → rbv → mmu → floppy →
// asc → adb → scsi → via1 → scc → rtc → scheduler → cpu → mem_map → debugger.
static void mdu_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    mac030_mdu_state_t *st = mdu_st(cfg);
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
        if (st->floppy) {
            floppy_delete(st->floppy);
            st->floppy = NULL;
            cfg->floppy = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
    }
    // cfg->nubus is freed by system_destroy (nubus_delete runs before machine
    // teardown), matching the GLUE lifecycle.
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    // The AppleTalk stack is a client of the SCC's LocalTalk channel, so it
    // goes first — it holds the scc pointer it was given at init.
    appletalk_delete();
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
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
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

static void mdu_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    mac030_mdu_state_t *st = mdu_st(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    appletalk_checkpoint(cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(st->adb, cp);
    if (st->egret) // IIsi only; IIci leaves egret NULL
        egret_checkpoint(st->egret, cp);
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    rbv_checkpoint(st->rbv, cp);
    // The RBV chip's registers are above; this covers the display CARD behind
    // it — VRAM, palette and active mode (ledger §2).
    nubus_checkpoint_save(cfg->nubus, cp);
    mmu_checkpoint_save(st->mmu, cp);
}

// substrate.nubus_slot_irq — the RBV aggregates NuBus slot interrupts itself
// (RvSInt & RvSEnb -> RvAnySlot -> the chip's combined interrupt -> IPL 2), so a
// slot source has to go to the chip rather than straight to update_ipl.
//
// The shared mac030_nubus_slot_irq_via_ipl passes `1 << (slot - 9)` as the
// machine's IRQ SOURCE mask, and on this family every one of those bits is
// already spoken for: IICI_IRQ_VIA1/RBV/SCC/NMI are 1<<0 .. 1<<3.  So a card in
// slot $C asserted the NMI source and the machine took a level-7 autovector
// every few instructions forever — which is what "a 24AC beside the live
// built-in RBV hangs the boot at Welcome" actually was (ledger §8).
//
// RvSInt numbering is logical: 0 is the built-in video (RvIRQ0, bit 6) and
// 1..6 are RvIRQ1..6, so NuBus $9..$E map to 1..6.
static void mdu_nubus_slot_irq(config_t *cfg, int slot, bool active, bool umbrella_edge) {
    (void)umbrella_edge; // the RBV aggregates internally
    mac030_mdu_state_t *st = mdu_st(cfg);
    if (!st || !st->rbv || slot < 0x9 || slot > 0xE)
        return;
    int logical = slot - 0x8;
    if (active)
        rbv_assert_slot_irq(st->rbv, logical);
    else
        rbv_clear_slot_irq(st->rbv, logical);
}

// MDU VBL: single VIA1 CA1 pulse (no VIA2), then fan out to NuBus.
static void mdu_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

const machine_substrate_t mdu_substrate = {
    .init = mdu_init,
    .reset = mdu_reset,
    .teardown = mdu_teardown,
    .checkpoint_save = mdu_checkpoint_save,
    .update_ipl = mac030_glue_update_ipl,
    .trigger_vbl = mdu_trigger_vbl,
    .nubus_slot_irq = mdu_nubus_slot_irq, // straight to the RBV's slot-interrupt register
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_detach = system_media_detach_std,
    .media_attach = system_media_attach_std,
};
