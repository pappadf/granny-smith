// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iisi.c
// Macintosh IIsi ("Erickson", 20 MHz 68030, 1990) machine implementation.
// Architecturally the IIci with the Egret companion chip added: Egret rides
// VIA1's shift register and takes over ADB, the RTC, parameter RAM, the
// 1-second tick, and soft power-off.  See proposal-machine-iici-iisi.md and
// docs/iisi.md.
//
// Diff vs iici.c at a glance:
//   * VIA1 port B + shift register drive the Egret companion (egret.c) instead
//     of the classic transceiver/RTC bit-bang.  ADB is delegated to the shared
//     adb_t via adb_iop_transact() (like the IIfx IOP path), not VIA-shift.
//   * RBV runs in its V8/VISA variant (RBV_VARIANT_V8_IISI).
//   * Built-in video framebuffer lives in the slot-$E aperture ($FEE08000 per
//     VideoInfoMacIIsi), not the IIci's slot-$B ($FBB08000).
//   * Machine-ID readback on VIA1 PA6/PA4/PA2/PA1 = 0/1/1/1 (mask $56, value
//     $16 per InfoMacIIsi), vs the IIci's $46.
//   * CPU clock 20 MHz.
// Everything else (MDU I/O map, RBV interrupt → IPL 2, soft power-off, SCSI via
// RBV, Mode-24 framebuffer aliasing) matches the IIci.

#include "machine.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "builtin_rbv_video.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // for cpu->mmu field
#include "debug.h"
#include "egret.h"
#include "floppy.h"
#include "glue030.h" // family-shared image-list checkpoint helpers
#include "iisi_internal.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rbv.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("iisi");

// ============================================================
// I/O island offsets (private to the dispatcher) — MDU map
// ============================================================

#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_SCC            0x04000
#define IO_SCC_END        0x06000
#define IO_SCSI_DRQ       0x06000
#define IO_SCSI_DRQ_END   0x08000
#define IO_SCSI_REG       0x10000
#define IO_SCSI_REG_END   0x12000
#define IO_SCSI_BLIND     0x12000
#define IO_SCSI_BLIND_END 0x14000
#define IO_ASC            0x14000
#define IO_ASC_END        0x16000
#define IO_SWIM           0x16000
#define IO_SWIM_END       0x18000
#define IO_VDAC           0x24000
#define IO_VDAC_END       0x26000
#define IO_RBV            0x26000
#define IO_RBV_END        0x28000

// ============================================================
// SoA page helper (same logic as the IIci helper)
// ============================================================

static void iisi_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;
    if (writable) {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = adjusted;
        if (g_user_write)
            g_user_write[page_index] = adjusted;
    }
}

// ============================================================
// ROM overlay
// ============================================================

static void iisi_set_rom_overlay(config_t *cfg, bool overlay) {
    iisi_state_t *st = iisi_state(cfg);
    if (st->rom_overlay == overlay)
        return;
    st->rom_overlay = overlay;
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IISI_ROM_START >> PAGE_SHIFT;
    if (overlay) {
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
            uint8_t *host_ptr = g_page_table[rom_start_page + p].host_base;
            iisi_fill_page(p, host_ptr, false);
        }
    } else {
        uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
        for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++)
            iisi_fill_page(p, ram_base + (p << PAGE_SHIFT), true);
    }
}

// ============================================================
// Hardware reset
// ============================================================

static void iisi_reset(config_t *cfg) {
    iisi_state_t *st = iisi_state(cfg);
    st->rom_overlay = false;
    iisi_set_rom_overlay(cfg, true);
    if (st->mmu) {
        st->mmu->enabled = false;
        st->mmu->tc = 0;
        mmu_invalidate_tlb(st->mmu);
    }
}

// ============================================================
// I/O dispatcher (MDU island — IIci map: VIA1/SCC/SCSI/ASC/SWIM/VDAC/RBV)
// ============================================================

static uint8_t iisi_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iisi_state_t *st = iisi_state(cfg);
    uint32_t offset = addr & IISI_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IISI_VIA_IO_PENALTY);
        return st->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(IISI_SCC_IO_PENALTY);
        return st->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        return st->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IISI_ASC_IO_PENALTY);
        return st->asc_iface->read_uint8(st->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IISI_SWIM_IO_PENALTY);
        return st->floppy_iface->read_uint8(st->floppy, offset - IO_SWIM);
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(IISI_VDAC_IO_PENALTY);
        return builtin_rbv_video_vdac_read(st->video_card, offset - IO_VDAC);
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(IISI_RBV_IO_PENALTY);
        return st->rbv_iface->read_uint8(st->rbv, offset - IO_RBV);
    }
    return 0;
}

static uint16_t iisi_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)iisi_io_read_uint8(ctx, addr) << 8) | iisi_io_read_uint8(ctx, addr + 1);
}

static uint32_t iisi_io_read_uint32(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iisi_state_t *st = iisi_state(cfg);
    uint32_t offset = addr & IISI_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    return ((uint32_t)iisi_io_read_uint16(ctx, addr) << 16) | iisi_io_read_uint16(ctx, addr + 2);
}

static void iisi_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    iisi_state_t *st = iisi_state(cfg);
    uint32_t offset = addr & IISI_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IISI_VIA_IO_PENALTY);
        st->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(IISI_SCC_IO_PENALTY);
        st->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IISI_ASC_IO_PENALTY);
        st->asc_iface->write_uint8(st->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(IISI_SWIM_IO_PENALTY);
        st->floppy_iface->write_uint8(st->floppy, offset - IO_SWIM, value);
        return;
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(IISI_VDAC_IO_PENALTY);
        builtin_rbv_video_vdac_write(st->video_card, offset - IO_VDAC, value);
        return;
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(IISI_RBV_IO_PENALTY);
        st->rbv_iface->write_uint8(st->rbv, offset - IO_RBV, value);
        return;
    }
}

static void iisi_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    iisi_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    iisi_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

static void iisi_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    iisi_state_t *st = iisi_state(cfg);
    uint32_t offset = addr & IISI_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(IISI_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    iisi_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    iisi_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

// ============================================================
// Memory layout
// ============================================================

static void iisi_memory_layout_init(config_t *cfg) {
    iisi_state_t *st = iisi_state(cfg);

    uint32_t ram_size = cfg->ram_size;
    uint32_t rom_size = cfg->machine->rom_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);

    // Two physical RAM banks (Developer Note §3.2/§3.3): Bank A (soldered 1 MB)
    // at physical 0 — its bottom is the on-board video frame buffer — and Bank B
    // (SIMM expansion) at physical $04000000, which the OS makes system "low
    // memory."  Each bank mirrors its installed size throughout its 64 MB window
    // (the boot ROM sizes a bank by that wrap).  This static (MMU-off) page table
    // models the physical map the ROM probes before it builds its PMMU tree.
    uint32_t bank_a_pages = IISI_BANK_A_SIZE >> PAGE_SHIFT; // 1 MB / 4 KB = 256
    uint32_t bank_a_window_pages = IISI_BANK_B_PHYS >> PAGE_SHIFT; // [0, $04000000)
    for (uint32_t p = 0; p < bank_a_window_pages && (int)p < g_page_count; p++)
        iisi_fill_page(p, ram_base + ((p % bank_a_pages) << PAGE_SHIFT), true);

    uint8_t *bank_b = ram_base + IISI_BANK_A_SIZE;
    uint32_t bank_b_size = (ram_size > IISI_BANK_A_SIZE) ? (ram_size - IISI_BANK_A_SIZE) : 0;
    uint32_t bank_b_pages = bank_b_size >> PAGE_SHIFT;
    uint32_t bank_b_start_page = IISI_BANK_B_PHYS >> PAGE_SHIFT;
    uint32_t bank_b_window_pages = IISI_BANK_WINDOW >> PAGE_SHIFT;
    for (uint32_t i = 0; bank_b_pages && i < bank_b_window_pages && (int)(bank_b_start_page + i) < g_page_count; i++)
        iisi_fill_page(bank_b_start_page + i, bank_b + ((i % bank_b_pages) << PAGE_SHIFT), true);

    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint32_t rom_start_page = IISI_ROM_START >> PAGE_SHIFT;
    uint32_t rom_end_page = IISI_ROM_END >> PAGE_SHIFT;
    if (rom_pages > 0) {
        for (uint32_t p = rom_start_page; p < rom_end_page && (int)p < g_page_count; p++) {
            uint32_t offset_in_rom = (p - rom_start_page) % rom_pages;
            iisi_fill_page(p, rom_data + (offset_in_rom << PAGE_SHIFT), false);
        }
    }

    st->io_interface.read_uint8 = iisi_io_read_uint8;
    st->io_interface.read_uint16 = iisi_io_read_uint16;
    st->io_interface.read_uint32 = iisi_io_read_uint32;
    st->io_interface.write_uint8 = iisi_io_write_uint8;
    st->io_interface.write_uint16 = iisi_io_write_uint16;
    st->io_interface.write_uint32 = iisi_io_write_uint32;
    memory_map_add(cfg->mem_map, IISI_IO_BASE, IISI_IO_SIZE, "IIsi I/O", &st->io_interface, cfg);

    // No separate VRAM aperture to wire: the on-board frame buffer IS the bottom
    // of Bank A (physical 0).  The OS reaches the screen through its PMMU tree
    // (slot-$E base $FEE08000 / $00E08000 -> physical 0 -> Bank A), and the card
    // renders straight out of Bank A — both resolve via the normal RAM path.

    st->rom_overlay = false;
    iisi_set_rom_overlay(cfg, true);
}

// ============================================================
// Interrupt routing
// ============================================================

static void iisi_update_ipl(config_t *cfg, int source, bool active) {
    if (active)
        cfg->irq |= source;
    else
        cfg->irq &= ~source;
    uint32_t new_ipl;
    if (cfg->irq & IISI_IRQ_NMI)
        new_ipl = 7;
    else if (cfg->irq & IISI_IRQ_SCC)
        new_ipl = 4;
    else if (cfg->irq & IISI_IRQ_RBV)
        new_ipl = 2;
    else if (cfg->irq & IISI_IRQ_VIA1)
        new_ipl = 1;
    else
        new_ipl = 0;
    cpu_set_ipl(cfg->cpu, new_ipl);
    cpu_reschedule();
}

// ============================================================
// RBV / SCC / SCSI / Egret callbacks
// ============================================================

// RBV combined interrupt → 68030 IPL 2.
static void iisi_rbv_irq(void *context, bool active) {
    iisi_update_ipl((config_t *)context, IISI_IRQ_RBV, active);
}

// Soft power-off — from either RBV RvPowerOff or Egret PwrDown.  Stop the
// scheduler so the headless run exits cleanly.
static void iisi_power_off(void *context) {
    config_t *cfg = (config_t *)context;
    LOG(1, "IIsi soft power-off");
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
}

// RBV depth change (V8MonP bits 0-2) → reshape the built-in video display.
static void iisi_rbv_mode(void *context, int depth_code) {
    config_t *cfg = (config_t *)context;
    iisi_state_t *st = iisi_state(cfg);
    builtin_rbv_video_set_depth(st->video_card, depth_code);
}

// SCSI IRQ/DRQ → RBV flag bits (RvSCSIRQ / RvSCSIDRQ), as on the IIci.
static void iisi_scsi_irq(void *context, bool irq, bool drq) {
    config_t *cfg = (config_t *)context;
    iisi_state_t *st = iisi_state(cfg);
    if (!st->rbv)
        return;
    rbv_set_scsi_irq(st->rbv, irq);
    rbv_set_scsi_drq(st->rbv, drq);
}

// VIA1 outputs.  Port A: floppy head-select (PA5) + ROM overlay (PA4 — an
// input on the IIsi, so it reads 0 and the first port-A write disables the
// overlay, matching the IIci).  Port B carries the Egret handshake pins
// (PB3 xcvrSes / PB4 viaFull / PB5 sysSes), routed wholesale to the Egret chip.
static void iisi_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iisi_state_t *st = iisi_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        // ROM overlay.  Unlike the IIci (reset PC in high ROM) the IIsi reset
        // vector is $0000002A: the ROM runs its early POST from the $0 overlay
        // and PA4 here is a machine-ID *input* (not the SE/30's overlay output).
        // The overlay must therefore stay mapped until the ROM relocates itself
        // to high ROM ($40800000) — disabling it while the PC is still in the
        // overlay would pull the executing ROM out from under the CPU.  We
        // model the MDU's "first high-ROM access turns the overlay off" by
        // disabling it on the first VIA port-A write issued from high ROM (the
        // ROM's INITVIAS, which runs only after the relocation jump).
        if (st->rom_overlay && cpu_get_pc(cfg->cpu) >= IISI_ROM_START)
            iisi_set_rom_overlay(cfg, false);
        st->last_port_a = output;
    } else {
        if (st->egret)
            egret_via1_pb_input(st->egret, output);
    }
}

// VIA1 shift-out: bytes the host shifts to Egret (command packets).
static void iisi_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    iisi_state_t *st = iisi_state(cfg);
    if (st->egret)
        egret_via1_shift_input(st->egret, byte);
}

static void iisi_via1_irq(void *context, bool active) {
    iisi_update_ipl((config_t *)context, IISI_IRQ_VIA1, active);
}

static void iisi_scc_irq(void *context, bool active) {
    iisi_update_ipl((config_t *)context, IISI_IRQ_SCC, active);
}

// ============================================================
// VBL trigger
// ============================================================

// VBL.  VIA1 CA1 carries the 60.15 Hz vertical-blank interrupt (IPL 1) — the
// ROM's VIATest waits on exactly this (it counts 10 CA1 interrupts) and the OS
// VBL manager runs off it, so we must pulse CA1 every frame just like the
// classic-ADB machines.  The RBV/V8 slot-0 (RvIRQ0 → IPL 2) interrupt is fanned
// out to the NuBus card on_vbl below.  The 1-second tick is delivered by Egret.
static void iisi_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// ============================================================
// Slot table
// ============================================================
//
// Built-in V8 video is "slot $0" to the Slot Manager, but the boot ROM drives
// it from the baked-in VideoInfoMacIIsi (screen base $FEE08000), not by
// scanning the bus.  We seat the pseudo-card at internal NuBus slot $E — the
// slot whose address space holds the framebuffer aperture ($FExxxxxx).  The
// slot index is an implementation detail (same arrangement as the IIci's
// slot-$B seating); there are no user-visible expansion slots in v1.
static const nubus_slot_decl_t iisi_slots[] = {
    {.slot = 0xE, .kind = NUBUS_SLOT_BUILTIN, .builtin_card_id = "builtin_rbv_video"},
    {0},
};

// ============================================================
// Init / Teardown
// ============================================================

static void iisi_init(config_t *cfg, checkpoint_t *checkpoint) {
    iisi_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);
    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 4, 4);
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    // RTC backs Egret's clock + PRAM pseudo-commands; it is NOT bit-banged on
    // VIA1 PB0-2 (the IIsi has no classic transceiver path), so we do not call
    // rtc_set_via.
    cfg->rtc = rtc_init(cfg->scheduler, checkpoint, true);
    cfg->scc = scc_init(NULL, cfg->scheduler, iisi_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 20, "via1", iisi_via1_output, iisi_via1_shift_out, iisi_via1_irq, cfg,
                         checkpoint);

    // Machine-ID readback on VIA1 port A: PA6/PA4/PA2/PA1 = 0/1/1/1 for the
    // IIsi ("Erickson") — mask $56, value $16 per InfoMacIIsi.
    via_input(cfg->via1, 0, 6, 0);
    via_input(cfg->via1, 0, 4, 1);
    via_input(cfg->via1, 0, 2, 1);
    via_input(cfg->via1, 0, 1, 1);
    // CA1 / CB1 idle high (VBL heartbeat + shift-register clock reference).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    // ADB is driven by Egret via adb_iop_transact() (like the IIfx IOP path),
    // not by the VIA1 shift register — pass NULL for the VIA argument.
    st->adb = adb_init(NULL, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    if (checkpoint)
        glue030_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    scsi_set_irq_callback(cfg->scsi, iisi_scsi_irq, cfg);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint); // RBV path; sound IRQ unwired (IIci/IIfx-parity)
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    // Egret companion: owns ADB / RTC / PRAM / 1-sec tick / soft power-off via
    // the VIA1 shift register.  Created after via1/rtc/adb exist.
    st->egret = egret_init(cfg->via1, cfg->rtc, st->adb, cfg->scheduler, checkpoint);
    assert(st->egret != NULL);
    egret_set_power_off_callback(st->egret, iisi_power_off, cfg);

    // RBV chip in the V8/VISA variant.  Default monitor sense 6 = 13" RGB.
    st->rbv = rbv_init(RBV_VARIANT_V8_IISI, checkpoint);
    assert(st->rbv != NULL);
    rbv_set_irq_callback(st->rbv, iisi_rbv_irq, cfg);
    rbv_set_power_off_callback(st->rbv, iisi_power_off, cfg);
    rbv_set_mode_callback(st->rbv, iisi_rbv_mode, cfg);
    rbv_set_monitor_sense(st->rbv, 6);

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);
    st->rbv_iface = rbv_get_memory_interface(st->rbv);

    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    st->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, IISI_ROM_START, IISI_ROM_END);
    assert(st->mmu != NULL);
    g_mmu = st->mmu;
    cpu_attach_mmu(cfg->cpu, st->mmu);
    // TT1 identity-maps NuBus space $F0-$FF for supervisor FCs (same as IIci).
    st->mmu->tt1 = 0xF00F8043;

    // Two physical RAM banks (Developer Note §3.2/§3.3): Bank A is the soldered
    // 1 MB at physical 0 (its bottom is the video frame buffer); Bank B is the
    // SIMM expansion at physical $04000000, which the OS makes system "low
    // memory."  Bank B is backed by the host RAM image above Bank A.  Teaching
    // the MMU about the second bank lets PMMU table walks resolve Bank B (where
    // the OS puts its page tables and globals) and Bank A's video frame buffer.
    mmu_set_ram_bank_b(st->mmu, IISI_BANK_A_SIZE, ram_base + IISI_BANK_A_SIZE, IISI_BANK_B_PHYS,
                       (ram_size > IISI_BANK_A_SIZE) ? (ram_size - IISI_BANK_A_SIZE) : 0, IISI_BANK_WINDOW);

    cfg->nubus = nubus_init(cfg, iisi_slots, checkpoint);
    st->video_card = nubus_card(cfg->nubus, 0xE);
    assert(st->video_card != NULL);
    builtin_rbv_video_set_rbv(st->video_card, st->rbv);

    // On-board video reads its frame buffer from the BOTTOM of Bank A — physical
    // 0 (Developer Note §8.2; VideoInfoMacIIsi screen physical base = 0).  Point
    // the card at Bank A's base; the active screen is at the frame-buffer start
    // (offset 0), unlike the IIci's separate $8000-offset buffer.  The OS reaches
    // the screen through its PMMU tree (slot-$E base $FEE08000 / $00E08000 ->
    // physical 0), so the guest's writes and the renderer share Bank A directly —
    // no separate VRAM aperture and no $E00000 offset.
    builtin_rbv_video_set_framebuffer(st->video_card, ram_base + IISI_FB_PHYS_OFFSET, IISI_FB_SCREEN_OFFSET);

    // NuBus expansion / slot space bus-errors on unmapped reads.  There is no
    // addressable card in slot $E (built-in video is main DRAM mapped by the OS),
    // so the $FExxxxxx slot aperture legitimately bus-errors when probed.
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iisi_memory_layout_init(cfg);

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &st->mmu->tc, sizeof(st->mmu->tc));
        system_read_checkpoint_data(checkpoint, &st->mmu->crp, sizeof(st->mmu->crp));
        system_read_checkpoint_data(checkpoint, &st->mmu->srp, sizeof(st->mmu->srp));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt0, sizeof(st->mmu->tt0));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt1, sizeof(st->mmu->tt1));
        system_read_checkpoint_data(checkpoint, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
        system_read_checkpoint_data(checkpoint, &st->mmu->enabled, sizeof(st->mmu->enabled));
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cpu_attach_mmu(cfg->cpu, st->mmu);
        via_redrive_outputs(cfg->via1);
    }

    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

static void iisi_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    iisi_state_t *st = iisi_state(cfg);
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
    // cfg->nubus is freed by system_destroy (before the machine teardown).
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

// ============================================================
// Checkpoint
// ============================================================

static void iisi_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    iisi_state_t *st = iisi_state(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(st->adb, cp);
    egret_checkpoint(st->egret, cp);
    glue030_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    rbv_checkpoint(st->rbv, cp);
    system_write_checkpoint_data(cp, &st->mmu->tc, sizeof(st->mmu->tc));
    system_write_checkpoint_data(cp, &st->mmu->crp, sizeof(st->mmu->crp));
    system_write_checkpoint_data(cp, &st->mmu->srp, sizeof(st->mmu->srp));
    system_write_checkpoint_data(cp, &st->mmu->tt0, sizeof(st->mmu->tt0));
    system_write_checkpoint_data(cp, &st->mmu->tt1, sizeof(st->mmu->tt1));
    system_write_checkpoint_data(cp, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
    system_write_checkpoint_data(cp, &st->mmu->enabled, sizeof(st->mmu->enabled));
}

// ============================================================
// Machine descriptor
// ============================================================

// Real IIsi RAM configs (Developer Note §6.1): fixed 1 MB soldered Bank A plus
// four equal SIMMs in Bank B (1/2/4/8/16/64 MB) → totals 2/3/5/9/17/65 MB.  The
// frame buffer lives at the bottom of Bank A (physical 0), so there is no longer
// a "must reach past the framebuffer" floor.  Offer the System-7-capable sizes.
static const uint32_t iisi_ram_options_kb[] = {5120, 9216, 17408, 66560, 0}; // 5/9/17/65 MB

static const struct floppy_slot iisi_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iisi_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_iisi = {
    .name = "Macintosh IIsi",
    .id = "iisi",

    .cpu_model = 68030,
    .freq = 20000000, // 20 MHz
    .mmu_present = true,
    .fpu_present = true, // FPU adapter present by default
    .mmu_kind = MMU_68030_PMMU,

    .address_bits = 32,
    .ram_default = 0x1100000, // 17 MB (1 MB Bank A + 16 MB Bank B)
    .ram_max = 0x8000000, // Bank B window top ($04000000 + 64 MB)
    .rom_size = 0x80000, // 512 KB

    .ram_options = iisi_ram_options_kb,
    .floppy_slots = iisi_floppy_slots,
    .scsi_slots = iisi_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    // Built-in V8 video has no separate declaration ROM — the boot ROM drives
    // it from the hard-coded VideoInfoMacIIsi record.
    .needs_vrom = false,

    .via_count = 1, // VIA2 is replaced by the RBV
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 0, // no user-visible expansion slots in v1
    .nubus_slots = iisi_slots,

    .init = iisi_init,
    .reset = iisi_reset,
    .teardown = iisi_teardown,
    .checkpoint_save = iisi_checkpoint_save,
    .checkpoint_restore = NULL,
    .memory_layout_init = iisi_memory_layout_init,
    .update_ipl = iisi_update_ipl,
    .trigger_vbl = iisi_trigger_vbl,
    .display = NULL, // primary display sourced from cfg->nubus
};
