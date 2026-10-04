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
#include "debug.h"
#include "egret.h"
#include "floppy.h"
#include "iop.h"
#include "iw_printer.h"
#include "keyboard.h"
#include "macroman.h"
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

static void part_save_imagewriter(void *obj, checkpoint_t *cp) {
    iw_printer_checkpoint(obj, cp);
}

// A Macintosh print job's title: the foreground application's name, the
// Pascal string in the low-memory global CurApName ($910, 32 bytes).
static bool mac_print_title(char *out, size_t cap) {
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    bool xl = dif && dif->translate_mac;
    uint8_t name[32];
    for (uint32_t i = 0; i < sizeof(name); i++)
        name[i] = memory_debug_read_uint8(xl ? debug_mac_xlate(0x910 + i) : 0x910 + i);
    size_t len = name[0];
    if (len == 0 || len > 31)
        return false;
    // Not a name (uninitialised memory at the boot prompt): no title
    for (size_t i = 1; i <= len; i++)
        if (name[i] < 0x20 || name[i] == 0x7F)
            return false;
    macroman_to_utf8(name + 1, len, out, cap);
    return true;
}

void machine_part_imagewriter(config_t *cfg, checkpoint_t *cp, bool lisa) {
    // HSKi reaches the SCC's /CTS uninverted (Guide to the Macintosh Family
    // Hardware, 2nd ed., Table 10-1): a ready printer holds its DTR high, so
    // /CTS reads high -- RR0's CTS bit clear.  The Lisa wires Serial A's DSR
    // to /SYNC itself (lisa.c) and has no handshake on Serial B.
    static const iw_port_wiring_t mac[2] = {
        {true, SCC_PIN_CTS, false},
        {true, SCC_PIN_CTS, false}
    };
    static const iw_port_wiring_t lisa_wiring[2] = {
        {false, SCC_PIN_SYNC, true },
        {false, SCC_PIN_CTS,  false}
    };
    machine_part_begin(cfg, cp, "imagewriter");
    cfg->imagewriter = iw_printer_new(cfg->scheduler, cfg->scc, lisa ? lisa_wiring : mac, cp);
    if (!cfg->imagewriter) {
        machine_part_cancel(cfg);
        return;
    }
    if (!lisa)
        iw_printer_set_title_source(cfg->imagewriter, mac_print_title);
    machine_part(cfg, cp, "imagewriter", part_save_imagewriter, cfg->imagewriter);
}

static void part_save_irq(void *obj, checkpoint_t *cp) {
    const config_t *cfg = obj;
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
}

void machine_part_irq(config_t *cfg, checkpoint_t *cp) {
    machine_part_begin(cfg, cp, "irq");
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    machine_part(cfg, cp, "irq", part_save_irq, cfg);
}

static void part_save_images(void *obj, checkpoint_t *cp) {
    mac_checkpoint_save_images(obj, cp);
}

void machine_part_images(config_t *cfg, checkpoint_t *cp) {
    machine_part_begin(cfg, cp, "images");
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);
    machine_part(cfg, cp, "images", part_save_images, cfg);
}
