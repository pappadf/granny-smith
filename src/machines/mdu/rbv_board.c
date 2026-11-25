// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rbv_board.c
// The RBV-lineage device spine and glue -- see rbv_board.h.

#include "rbv_board.h"

#include "m68k_board.h" // m68k_board_via2_irq
#include "machine_checkpoint.h"
#include "machine_teardown.h"

#include "adb.h"
#include "asc.h"
#include "cuda.h"
#include "egret.h"
#include "floppy.h"
#include "log.h"
#include "mmu.h"
#include "scheduler.h"
#include "scsi.h"
#include "via.h"

LOG_USE_CATEGORY_NAME("board");

MACHINE_PART_SAVE_EXTERN(rbv, rbv_checkpoint, rbv_t)

// The 5380's IRQ/DRQ, routed through the RBV flags (RvSCSIRQ / RvSCSIDRQ)
// rather than a VIA2.  Context: the slot the RBV is built into.
static void rbv_board_scsi_irq(void *context, bool irq, bool drq) {
    rbv_t *rbv = *(rbv_t **)context;
    // The 5380 is built before the RBV: nothing to flag until it exists
    if (!rbv)
        return;
    rbv_set_scsi_irq(rbv, irq);
    rbv_set_scsi_drq(rbv, drq);
}

// The sound block's interrupt -> the RBV sound flag (RvSndIRQ, RvIFR bit 4).
// The RBV emulates the VIA2-CB1 dispatch slot the shared OS interrupt code
// expects (jASCInt = Via2DT+4*ifCB1); the V8 mirrors the bit layout.
static void rbv_board_asc_irq(void *context, bool active) {
    rbv_set_snd_irq((rbv_t *)context, active);
}

void rbv_board_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "%s soft power-off", cfg->machine->name);
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

int rbv_board_build_devices(config_t *cfg, checkpoint_t *cp, const rbv_board_spec_t *spec,
                            const rbv_board_slots_t *slots) {
    // With no companion the ADB transceiver is VIA1's shift register; Egret
    // and Cuda drive ADB themselves (adb_iop_transact), so no VIA.
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(spec->companion == RBV_BOARD_NO_COMPANION ? cfg->via1 : NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    machine_part_images(cfg, cp);

    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    scsi_5380_attach(cfg->scsi, cp); // the NCR 5380 (the V8 boards': the Combo's 53C80)
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    scsi_set_irq_callback(cfg->scsi, rbv_board_scsi_irq, slots->rbv); // through the RBV, not a VIA2

    machine_part_begin(cfg, cp, "asc");
    struct asc *asc = *slots->asc = asc_init(NULL, cfg->scheduler, cp);
    if (!asc)
        return -1;
    if (spec->asc_version)
        asc_set_version(asc, spec->asc_version);
    if (spec->asc_fifo_only)
        asc_set_fifo_only(asc); // no wavetable, always draining
    machine_part(cfg, cp, "asc", part_save_asc, asc);
    asc_set_mix(asc, ASC_MIX_CH_A); // the internal speaker takes the left channel

    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);

    // The companion owns ADB / RTC / PRAM / the 1-second tick / soft
    // power-off over VIA1's shift register; built after via1/rtc/adb exist.
    if (spec->companion == RBV_BOARD_CUDA) {
        machine_part_begin(cfg, cp, "cuda");
        *slots->cuda = cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/false);
        if (!*slots->cuda) {
            LOG(0, "Error: out of memory constructing the Cuda");
            return -1;
        }
        machine_part(cfg, cp, "cuda", part_save_cuda, *slots->cuda);
    } else if (spec->companion == RBV_BOARD_EGRET) {
        machine_part_begin(cfg, cp, "egret");
        *slots->egret = egret_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp);
        if (!*slots->egret) {
            LOG(0, "Error: out of memory constructing the Egret");
            return -1;
        }
        machine_part(cfg, cp, "egret", part_save_egret, *slots->egret);
        egret_set_power_off_callback(*slots->egret, rbv_board_power_off, cfg);
    }
    return 0;
}

int rbv_board_build_rbv(config_t *cfg, checkpoint_t *cp, rbv_variant_t variant, uint8_t sense,
                        const rbv_board_slots_t *slots) {
    machine_part_begin(cfg, cp, "rbv");
    rbv_t *rbv = *slots->rbv = rbv_init(variant, sense, cp);
    if (!rbv) {
        LOG(0, "Error: out of memory constructing the RBV");
        return -1;
    }
    machine_part(cfg, cp, "rbv", part_save_rbv, rbv);
    rbv_set_irq_callback(rbv, m68k_board_via2_irq, cfg); // the combined interrupt -> IPL 2, VIA2's level
    asc_set_irq_handler(*slots->asc, rbv_board_asc_irq, rbv); // sound IRQ -> RvIFR bit 4
    return 0;
}

void rbv_board_teardown(config_t *cfg, const rbv_board_slots_t *slots, struct mmu_state **mmu) {
    if (slots) {
        // The companion goes before the adb it was handed.
        if (slots->egret && *slots->egret) {
            egret_delete(*slots->egret);
            *slots->egret = NULL;
        }
        if (slots->cuda && *slots->cuda) {
            cuda_delete(*slots->cuda);
            *slots->cuda = NULL;
        }
        if (slots->rbv && *slots->rbv) {
            rbv_delete(*slots->rbv);
            *slots->rbv = NULL;
        }
        if (mmu && *mmu) {
            mmu_delete(*mmu);
            *mmu = NULL;
        }
        if (slots->asc && *slots->asc) {
            asc_delete(*slots->asc);
            *slots->asc = NULL;
        }
    }
    // The config_t-owned devices, in the family-shared canonical order.
    machine_teardown_config_devices(cfg);
}
