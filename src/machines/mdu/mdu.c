// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu.c
// The one MDU+RBV-family substrate — see mdu.h.  IIci and IIsi share this; the
// reset/teardown/checkpoint/VBL lifecycle is the former per-machine clones,
// unified (the only IIci/IIsi delta is the IIsi's Egret, handled by an
// `if (st->egret)` — IIci leaves egret NULL in the unified mac030_mdu_state_t).

#include "mdu.h"

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

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", board->via1_output, board->via1_shift_out,
                         mac030_glue_via1_irq, cfg, cp);

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
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(st->adb, cp);
    if (st->egret) // IIsi only; IIci leaves egret NULL
        egret_checkpoint(st->egret, cp);
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    rbv_checkpoint(st->rbv, cp);
    mmu_checkpoint_save(st->mmu, cp);
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
    .nubus_slot_irq = mac030_nubus_slot_irq_via_ipl, // RBV aggregates slots → update_ipl
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
};
