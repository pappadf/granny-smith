// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_checkpoint.c — the checkpoint parts every family shares.  See the
// header.

#include "machine_checkpoint.h"

#include "adb.h"
#include "appletalk.h"
#include "asc.h"
#include "checkpoint.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "cuda.h"
#include "dbdma.h"
#include "debug.h"
#include "egret.h"
#include "floppy.h"
#include "iop.h"
#include "iw_printer.h"
#include "keyboard.h"
#include "macroman.h"
#include "memory.h"
#include "mmu.h"
#include "mmu_checkpoint.h"
#include "mouse.h"
#include "nubus.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "scsi_mesh.h"
#include "sonic.h"
#include "sound.h"
#include "system.h"
#include "system_internal.h"
#include "via.h"

// The shared part-save adapters (machine_checkpoint.h): one typed call each.
MACHINE_PART_SAVE_EXTERN(memory, memory_map_checkpoint, memory_map_t)
// The CPU: on the 040 families its block carries the MMU register file too
MACHINE_PART_SAVE_EXTERN(cpu, cpu_checkpoint, cpu_t)
MACHINE_PART_SAVE_EXTERN(ppc, ppc_checkpoint, ppc_t)
MACHINE_PART_SAVE_EXTERN(scheduler, scheduler_checkpoint, struct scheduler)
MACHINE_PART_SAVE_EXTERN(rtc, rtc_checkpoint, rtc_t)
MACHINE_PART_SAVE_EXTERN(scc, scc_checkpoint, scc_t)
MACHINE_PART_SAVE_EXTERN(atalk, atalk_conn_checkpoint, atalk_conn_t)
MACHINE_PART_SAVE_EXTERN(via, via_checkpoint, via_t)
MACHINE_PART_SAVE_EXTERN(adb, adb_checkpoint, adb_t)
MACHINE_PART_SAVE_EXTERN(scsi, scsi_checkpoint, scsi_t)
MACHINE_PART_SAVE_EXTERN(asc, asc_checkpoint, asc_t)
MACHINE_PART_SAVE_EXTERN(floppy, floppy_checkpoint, floppy_t)
MACHINE_PART_SAVE_EXTERN(keyboard, keyboard_checkpoint, keyboard_t)
MACHINE_PART_SAVE_EXTERN(mouse, mouse_checkpoint, mouse_t)
MACHINE_PART_SAVE_EXTERN(sound, sound_checkpoint, sound_t)
MACHINE_PART_SAVE_EXTERN(mmu, mmu_checkpoint_save, mmu_state_t)
MACHINE_PART_SAVE_EXTERN(scsi96, scsi_53c96_checkpoint, scsi_53c96_t)
MACHINE_PART_SAVE_EXTERN(sonic, sonic_checkpoint, sonic_t)
MACHINE_PART_SAVE_EXTERN(egret, egret_checkpoint, egret_t)
MACHINE_PART_SAVE_EXTERN(iop, iop_checkpoint, iop_t)
MACHINE_PART_SAVE_EXTERN(cuda, cuda_checkpoint, cuda_t)
MACHINE_PART_SAVE_EXTERN(dbdma, dbdma_checkpoint, dbdma_t)
MACHINE_PART_SAVE_EXTERN(mesh, mesh_checkpoint, mesh_t)

MACHINE_PART_SAVE(iw_printer_checkpoint, iw_printer_t)

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
    // A fresh boot plugs in the configuration's printer (a restore keeps the
    // checkpoint's): on the port AppleTalk leaves free -- the modem port while
    // it is active, else the printer port; the Lisa's Serial A
    const char *choice = cp ? NULL : machine_build_opts_option(&cfg->build_opts, "imagewriter");
    if (choice && strcmp(choice, "none") != 0) {
        const char *atalk = machine_build_opts_option(&cfg->build_opts, "appletalk");
        bool atalk_active = !atalk || strcmp(atalk, "inactive") != 0;
        iw_connection_t conn = (lisa || atalk_active) ? IW_CONN_SERIAL_A : IW_CONN_SERIAL_B;
        iw_model_t model = strcmp(choice, "imagewriter") == 0 ? IW_MODEL_IW1 : IW_MODEL_IW2;
        iw_printer_install(cfg->imagewriter, model, conn);
    }
    machine_part(cfg, cp, "imagewriter", iw_printer_checkpoint_part, cfg->imagewriter);
    // The AppleTalk ImageWriter: on the network beside the LaserWriter on
    // every machine with AppleTalk, whatever is on the serial ports (#345)
    if (!cfg->atalk)
        return;
    machine_part_begin(cfg, cp, "atalk_imagewriter");
    cfg->atalk_imagewriter = iw_printer_new_network(cfg->scheduler, cp);
    if (!cfg->atalk_imagewriter) {
        machine_part_cancel(cfg);
        return;
    }
    if (!lisa)
        iw_printer_set_title_source(cfg->atalk_imagewriter, mac_print_title);
    machine_part(cfg, cp, "atalk_imagewriter", iw_printer_checkpoint_part, cfg->atalk_imagewriter);
}

static void part_save_irq(void *obj, checkpoint_t *cp) {
    const config_t *cfg = obj;
    system_write_checkpoint_data(cp, &cfg->rt.irq, sizeof(cfg->rt.irq));
}

void machine_part_irq(config_t *cfg, checkpoint_t *cp) {
    machine_part_begin(cfg, cp, "irq");
    if (cp)
        system_read_checkpoint_data(cp, &cfg->rt.irq, sizeof(cfg->rt.irq));
    machine_part(cfg, cp, "irq", part_save_irq, cfg);
}

void machine_part_pmmu(config_t *cfg, checkpoint_t *cp, mmu_state_t *mmu, via_t *pre, via_t *post) {
    machine_part_begin(cfg, cp, "mmu");
    if (cp) {
        // Re-drive the overlay VIA first, while the PMMU is the fresh, disabled
        // one: its overlay output writes RAM identity entries for the low pages
        // straight into the SoA arrays.  Those are right with the MMU off and
        // wrong under the guest's tables; left in place past the invalidate
        // below, they sent the restored guest's low-memory writes elsewhere
        // (on the IIci and IIsi, into the screen at the bottom of Bank A).
        via_redrive_outputs(pre);
        mmu_checkpoint_restore(mmu, cp);
        mmu_invalidate_tlb(mmu);
        // Set both, always together: the fault hook runs on the map's PMMU and
        // the 68030 bus-error path reads cpu->mmu (asserted equal there).
        memory_map_set_pmmu(cfg->memory_map, mmu);
        cpu_attach_mmu(cfg->cpu, mmu);
        if (post)
            via_redrive_outputs(post);
    }
    machine_part(cfg, cp, "mmu", part_save_mmu, mmu);
}

MACHINE_PART_SAVE(mac_checkpoint_save_images, config_t)

void machine_part_images(config_t *cfg, checkpoint_t *cp) {
    machine_part_begin(cfg, cp, "images");
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);
    machine_part(cfg, cp, "images", mac_checkpoint_save_images_part, cfg);
}
