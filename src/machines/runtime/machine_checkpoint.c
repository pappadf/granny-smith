// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_checkpoint.c — the checkpoint parts every family shares.  See the
// header.

#include "machine_checkpoint.h"

#include "adb.h"
#include "appletalk.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "cuda.h"
#include "egret.h"
#include "floppy.h"
#include "iop.h"
#include "keyboard.h"
#include "memory.h"
#include "mmu_checkpoint.h"
#include "mouse.h"
#include "nubus.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "sound.h"
#include "system.h"
#include "system_config.h"
#include "via.h"

void part_save_memory(void *obj, checkpoint_t *cp) {
    memory_map_checkpoint(obj, cp);
}
void part_save_cpu(void *obj, checkpoint_t *cp) {
    cpu_checkpoint(obj, cp); // on the 040 families this carries the MMU register file too
}
void part_save_ppc(void *obj, checkpoint_t *cp) {
    ppc_checkpoint(obj, cp);
}
void part_save_scheduler(void *obj, checkpoint_t *cp) {
    scheduler_checkpoint(obj, cp);
}
void part_save_rtc(void *obj, checkpoint_t *cp) {
    rtc_checkpoint(obj, cp);
}
void part_save_scc(void *obj, checkpoint_t *cp) {
    scc_checkpoint(obj, cp);
}
void part_save_atalk(void *obj, checkpoint_t *cp) {
    atalk_conn_checkpoint(obj, cp);
}
void part_save_via(void *obj, checkpoint_t *cp) {
    via_checkpoint(obj, cp);
}
void part_save_adb(void *obj, checkpoint_t *cp) {
    adb_checkpoint(obj, cp);
}
void part_save_scsi(void *obj, checkpoint_t *cp) {
    scsi_checkpoint(obj, cp);
}
void part_save_asc(void *obj, checkpoint_t *cp) {
    asc_checkpoint(obj, cp);
}
void part_save_floppy(void *obj, checkpoint_t *cp) {
    floppy_checkpoint(obj, cp);
}
void part_save_keyboard(void *obj, checkpoint_t *cp) {
    keyboard_checkpoint(obj, cp);
}
void part_save_mouse(void *obj, checkpoint_t *cp) {
    mouse_checkpoint(obj, cp);
}
void part_save_sound(void *obj, checkpoint_t *cp) {
    sound_checkpoint(obj, cp);
}
void part_save_mmu(void *obj, checkpoint_t *cp) {
    mmu_checkpoint_save(obj, cp);
}
void part_save_nubus_cards(void *obj, checkpoint_t *cp) {
    nubus_checkpoint_save(obj, cp);
}

void part_save_scsi96(void *obj, checkpoint_t *cp) {
    scsi_53c96_checkpoint(obj, cp);
}
void part_save_sonic(void *obj, checkpoint_t *cp) {
    sonic_checkpoint(obj, cp);
}
void part_save_egret(void *obj, checkpoint_t *cp) {
    egret_checkpoint(obj, cp);
}
void part_save_iop(void *obj, checkpoint_t *cp) {
    iop_checkpoint(obj, cp);
}
void part_save_cuda(void *obj, checkpoint_t *cp) {
    av_cuda_checkpoint(obj, cp);
}

static void part_save_irq(void *obj, checkpoint_t *cp) {
    const config_t *cfg = obj;
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
}

void machine_part_irq(config_t *cfg, checkpoint_t *cp) {
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    machine_part(cfg, cp, "irq", part_save_irq, cfg);
}

static void part_save_images(void *obj, checkpoint_t *cp) {
    mac_checkpoint_save_images(obj, cp);
}

void machine_part_images(config_t *cfg, checkpoint_t *cp) {
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);
    machine_part(cfg, cp, "images", part_save_images, cfg);
}
