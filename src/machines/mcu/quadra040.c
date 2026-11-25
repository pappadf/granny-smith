// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// quadra040.c
// The device block and board plumbing the MCU and djMEMC families share --
// see quadra040.h.

#include "quadra040.h"

#include "machine_checkpoint.h"
#include "slot_irq.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "cpu.h"
#include "dafb.h"
#include "dma_mem.h" // dma_mem_port_physical (SONIC bus-master DMA)
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "via.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// ============================================================
// Construction
// ============================================================

void quadra040_build_core(config_t *cfg, checkpoint_t *cp, const m68k_board_desc_t *common,
                          void (*scc_irq)(void *, bool), void (*via1_output)(void *, uint8_t, uint8_t),
                          void (*via1_shift_out)(void *, uint8_t), void (*via2_output)(void *, uint8_t, uint8_t)) {
    quadra040_t *q = quadra040_st(cfg);
    q->common = common;

    // Shared core (memory_map, 68040 CPU from the profile, scheduler), the
    // IRQ part, then RTC + SCC + AppleTalk.
    m68k_board_build_core(cfg, common, cp);
    machine_part_irq(cfg, cp);
    m68k_board_build_lowspeed(cfg, cp, scc_irq);

    // The VIA clock factor derives from the CPU clock (25 or 33 MHz here).
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", via1_output, via1_shift_out, m68k_board_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    machine_part_begin(cfg, cp, "via2");
    cfg->via2 = via_init(NULL, cfg->scheduler, via_ff, "via2", via2_output, NULL, m68k_board_via2_irq, cfg, cp);
    machine_part(cfg, cp, "via2", part_save_via, cfg->via2);
    // Exact-rational phi2: the integer divisor above rounds, and on these
    // clocks that rounding is not negligible (the Q950 lands 1.04% slow).
    // via_set_exact_clock installs ticks = cycles x 783360/cpu_hz reduced.
    via_set_exact_clock(cfg->via1, cfg->machine->freq);
    via_set_exact_clock(cfg->via2, cfg->machine->freq);
}

void quadra040_build_finish(config_t *cfg, checkpoint_t *cp) {
    quadra040_t *q = quadra040_st(cfg);
    // NuBus: seat the declared slot cards; their windows layer over the
    // bus-error range, and slot IRQs route through the substrate's
    // nubus_slot_irq into the VIA2 PA aggregate.
    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);
    // Project the cards' host regions (VRAM, declaration ROMs) into the page
    // table so CPU accesses resolve with the MMU off; the bus resolver serves
    // the 040 walker when it's on.  No Mode-24 aliases -- these ROMs and
    // Systems are 32-bit clean, and a low alias would shadow RAM at
    // $00s00000 on large-memory configurations.
    mmu_host_regions_fill_pages(q->bus_mmu, page_fill_68k, /*mode24_alias*/ false);
    m68k_board_finish(cfg, cp, &q->io);
}

void quadra040_idle_lines(config_t *cfg, uint8_t via1_pa) {
    // VIA1 port A: the identity straps (PA0, the diagnostic strap, high --
    // see mcu.h).
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via1, 0, bit, (via1_pa >> bit) & 1);
    // VIA1 CA1/CB1 idle high (tick + ADB clock reference edges).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    // VIA2 port A: slot/video/Ethernet requests, active low, idle high.
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via2, 0, bit, 1);
    via_input_c(cfg->via2, 0, 0, 1); // CA1 /SLOTIRQ idle
    via_input_c(cfg->via2, 1, 0, 1); // CB1 sound idle
}

void quadra040_build_scsi0(config_t *cfg, checkpoint_t *cp, void (*irq)(void *ctx, bool active)) {
    quadra040_t *q = quadra040_st(cfg);
    // The bus/target model carries the disks and CD; the 53C96 chip model
    // is the protocol front-end driving it through the external-initiator
    // API (there is no NCR 5380 register file on these boards).
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    machine_part_begin(cfg, cp, "scsi96");
    q->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    machine_part(cfg, cp, "scsi96", part_save_scsi96, q->scsi96);
    scsi_53c96_set_irq_callback(q->scsi96, irq, cfg);
    scsi_53c96_attach_bus(q->scsi96, cfg->scsi);
}

void quadra040_build_sonic_asc_floppy(config_t *cfg, checkpoint_t *cp, void (*sonic_irq)(void *ctx, bool active),
                                      void (*asc_irq)(void *ctx, bool active)) {
    quadra040_t *q = quadra040_st(cfg);
    // SONIC Ethernet (a 20 MHz-class part; no wire).
    if (sonic_irq) {
        machine_part_begin(cfg, cp, "sonic");
        q->sonic = sonic_init(cp);
        machine_part(cfg, cp, "sonic", part_save_sonic, q->sonic);
        sonic_set_irq_callback(q->sonic, sonic_irq, cfg);
        // SONIC bus-master DMA: the shared guest-physical port.
        sonic_set_memory_port(q->sonic, &dma_mem_port_physical);
    }

    machine_part_begin(cfg, cp, "asc");
    q->asc = asc_init(NULL, cfg->scheduler, cp); // EASC: ASC-compatible core
    machine_part(cfg, cp, "asc", part_save_asc, q->asc);
    asc_set_mix(q->asc, ASC_MIX_CH_A);
    asc_set_irq_handler(q->asc, asc_irq, cfg);
    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);
}

// Built-in video interrupt -> VIA2 PA6 (active-low) through the /SLOTIRQ
// aggregate on CA1, alongside the NuBus slot sources.
static void quadra040_dafb_irq(void *context, bool active) {
    quadra040_slot_irq_source((config_t *)context, 6, active);
}

// The DAFB's identity part: the monitor strapped to it.
static void part_save_dafb_monitor(void *obj, checkpoint_t *cp) {
    uint8_t monitor = dafb_monitor(((quadra040_t *)obj)->dafb);
    system_write_checkpoint_data(cp, &monitor, sizeof monitor, "dafb.monitor");
}

MACHINE_PART_SAVE(dafb_checkpoint, dafb_t)

int quadra040_build_dafb(config_t *cfg, checkpoint_t *cp, const quadra040_video_t *video,
                         struct scsi_53c96 *scsi96_ext) {
    quadra040_t *q = quadra040_st(cfg);

    // The monitor on the built-in port: the document's on a boot, the one
    // the board was built with on a restore.
    uint8_t monitor = cfg->build_opts.builtin_sense;
    machine_part_begin(cfg, cp, "dafb.monitor");
    if (cp)
        system_read_checkpoint_data(cp, &monitor, sizeof monitor, "dafb.monitor");
    machine_part(cfg, cp, "dafb.monitor", part_save_dafb_monitor, q);

    machine_part_begin(cfg, cp, "dafb");
    q->dafb = dafb_init(video->vram_size, monitor, cp);
    if (!q->dafb) {
        LOG(0, "Error: out of memory constructing the built-in video");
        return -1;
    }
    machine_part(cfg, cp, "dafb", dafb_checkpoint_part, q->dafb);
    dafb_attach_scheduler(q->dafb, cfg->scheduler, cp);
    dafb_set_irq_callback(q->dafb, quadra040_dafb_irq, cfg);
    dafb_attach_objects(q->dafb); // machine.video{,.framebuffer}

    dafb_set_version(q->dafb, video->version); // 3 on the Q950 (DAFB 3)
    dafb_set_ac842a(q->dafb, video->ac842a); // AC842a x555 (Q950; the djMEMC's Antelope answers it too)
    dafb_set_antelope(q->dafb, video->antelope); // the Antelope's non-zero mfg/rev

    // TurboSCSI DRQ observation: channel 0 = internal, channel 1 the towers'
    // external 53C96.
    if (video->turbo_scsi) {
        dafb_set_scsi_drq_query(q->dafb, 0, (dafb_drq_query_fn)scsi_53c96_dreq, q->scsi96);
        if (scsi96_ext)
            dafb_set_scsi_drq_query(q->dafb, 1, (dafb_drq_query_fn)scsi_53c96_dreq, scsi96_ext);
    }
    return 0;
}

int quadra040_build_bus_mmu(config_t *cfg) {
    quadra040_t *q = quadra040_st(cfg);
    q->bus_mmu = m68k_board_build_bus_mmu040(cfg, 0x40000000u, q->common->rom_base, q->common->rom_end);
    return q->bus_mmu ? 0 : -1;
}

void quadra040_io_bind(config_t *cfg) {
    quadra040_t *q = quadra040_st(cfg);
    board_io_t *io = &q->io;
    m68k_board_io_install(io, cfg, q->common);
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_VIA2, cfg->via2, via_get_memory_interface(cfg->via2));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_ASC, q->asc, asc_get_memory_interface(q->asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, cfg->floppy, floppy_get_memory_interface(cfg->floppy));
    board_io_bind_dev(io, BOARD_DEV_SCSI96, q->scsi96, scsi_53c96_get_memory_interface());
}

void quadra040_map_island(config_t *cfg) {
    quadra040_t *q = quadra040_st(cfg);
    // The 256 KiB island at $50000000; the published map repeats it through
    // $53FFFFFF, and the mirror mask folds accesses.
    board_io_fill_interface(&q->io_interface);
    memory_map_add(cfg->memory_map, 0x50000000u, 0x04000000u, "I/O", &q->io_interface, &q->io);
}

void quadra040_map_video(config_t *cfg, const char *regs_name, const memory_interface_t *regs_iface, void *regs_ctx) {
    quadra040_t *q = quadra040_st(cfg);
    if (!regs_iface) {
        regs_iface = dafb_reg_interface(q->dafb);
        regs_ctx = q->dafb;
    }
    memory_map_add(cfg->memory_map, DAFB_REG_BASE, DAFB_REG_APERTURE, regs_name, (memory_interface_t *)regs_iface,
                   regs_ctx);
    // The VRAM decode spans the whole 2 MB aperture whatever is fitted, the
    // installed VRAM repeating through it: the djMEMC's PrimaryInit reads
    // the top of every possible VRAM size before it sizes (it must not
    // bus-error), and the repeat is what makes its write-then-compare sizing
    // find the fitted amount.  (The MCU boards fit the whole 2 MB.)
    uint8_t *vram = dafb_vram(q->dafb);
    uint32_t vram_size = dafb_vram_size(q->dafb);
    m68k_board_map_mirrored(DAFB_VRAM_BASE >> MEM_PAGE_SHIFT, DAFB_VRAM_APERTURE >> MEM_PAGE_SHIFT, vram,
                            vram_size >> MEM_PAGE_SHIFT, page_fill_68k, true);
    // ...and the same repeat for the bus resolver, so 040 table walks and
    // translated accesses reaching physical $F9xxxxxx resolve to the buffer.
    for (uint32_t off = 0; off < DAFB_VRAM_APERTURE; off += vram_size)
        memory_map_host_region(cfg->memory_map, "dafb_vram", vram, DAFB_VRAM_BASE + off, vram_size, true);
}

void quadra040_map_overlay(config_t *cfg, void (*map_ram)(config_t *cfg), const char *name) {
    quadra040_t *q = quadra040_st(cfg);
    const m68k_board_desc_t *c = q->common;
    // The trigger device for the ROM aperture is registered once; arming and
    // dropping only re-point page entries.
    m68k_board_overlay_init(&q->overlay, cfg, c->rom_base, c->rom_end, map_ram, name);
    memory_map_add(cfg->memory_map, c->rom_base, c->rom_end - c->rom_base, "ROM aperture", &q->overlay.iface,
                   &q->overlay);
    m68k_board_overlay_arm(&q->overlay);
}

// ============================================================
// Lifecycle
// ============================================================

void quadra040_bus_reset(config_t *cfg, bool reset_video) {
    quadra040_t *q = quadra040_st(cfg);
    // The overlay re-arms with /RESET.  The 040's own MMU is reset by
    // cpu_hardware_reset_040 -- it is inside the CPU, not on the net.
    m68k_board_overlay_arm(&q->overlay);
    if (reset_video && q->dafb)
        dafb_reset(q->dafb);
    // The bus MMU is a board part, not the CPU's, so it resets here.
    if (q->bus_mmu) {
        q->bus_mmu->enabled = false;
        mmu_invalidate_tlb(q->bus_mmu);
    }
    // The 53C96 is on /RESET as on PDM and TNT: its configuration and FIFO
    // clear (the bus itself is reset by the shared chain).
    scsi_53c96_reset(q->scsi96);
}

void quadra040_teardown(quadra040_t *q) {
    if (q->sonic) {
        sonic_delete(q->sonic);
        q->sonic = NULL;
    }
    if (q->scsi96) {
        scsi_53c96_delete(q->scsi96);
        q->scsi96 = NULL;
    }
    if (q->dafb) {
        dafb_delete(q->dafb);
        q->dafb = NULL;
    }
    if (q->bus_mmu) {
        mmu_delete(q->bus_mmu);
        q->bus_mmu = NULL;
    }
    if (q->asc) {
        asc_delete(q->asc);
        q->asc = NULL;
    }
}

void quadra040_restore_lines(config_t *cfg) {
    quadra040_t *q = quadra040_st(cfg);
    // Re-drive the /SLOTIRQ PA lines + the CA1 aggregate from the restored
    // mask (build parked them at the inactive level).
    slot_irq_redrive(cfg->via2, q->slot_pa_mask);
    // A NuBus-controller error pending at save time keeps requesting level 7.
    if (yancc_err_irq(&q->yancc))
        m68k_board_update_ipl(cfg, M68K_BOARD_IRQ_NMI, true);
    mmu_invalidate_tlb(q->bus_mmu);
    via_redrive_outputs(cfg->via1);
}

void quadra040_trigger_vbl(config_t *cfg) {
    via_pulse_c(cfg->via1, 0, 0); // VBL: CA1 low then high
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

struct display *quadra040_display(config_t *cfg) {
    quadra040_t *q = quadra040_st(cfg);
    return (q && q->dafb) ? dafb_display(q->dafb) : NULL;
}

// ============================================================
// Interrupt wiring
// ============================================================
// The slot/video/Ethernet request lines land on VIA2 PA0-PA6 (active-low)
// and OR into the active-low /SLOTIRQ on VIA2 CA1.  Every source funnels
// through here so the aggregate stays consistent regardless of origin
// (video = PA6, SONIC = PA0, NuBus slots A-E = PA1-PA5).

void quadra040_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    slot_irq_source(cfg->via2, &quadra040_st(cfg)->slot_pa_mask, pa_bit, active);
}

void quadra040_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    // Slot 9 is the built-in video and never arrives here: it drives PA6
    // directly through the aggregate.
    int pa_bit = slot - 0x9;
    if (pa_bit < 1 || pa_bit > 5)
        return;
    quadra040_slot_irq_source(cfg, pa_bit, active);
}

void quadra040_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    if (port == 0) {
        if (cfg->floppy)
            floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
        // No overlay bit here: the overlay is access-triggered.
    } else {
        // Bits 4-5: ADB state lines (ST0/ST1) -> the ADB controller; the
        // ST-transition filter and its port-B shadow live in adb.c.
        if (cfg->adb)
            adb_port_b_output(cfg->adb, output);
        rtc_via1_pb_output(cfg->rtc, output);
    }
}

void quadra040_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

void quadra040_asc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    // CB1 is active-low on the board; the 6522 PCR polarity handles edges.
    via_input_c(cfg->via2, 1, 0, active ? 0 : 1);
}

void quadra040_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 1, 1, active ? 0 : 1);
}

void quadra040_sonic_irq(void *context, bool active) {
    quadra040_slot_irq_source((config_t *)context, 0, active);
}

void quadra040_yancc_irq(void *context, bool active) {
    m68k_board_update_ipl((config_t *)context, M68K_BOARD_IRQ_NMI, active);
}

// ============================================================
// I/O island handler rows
// ============================================================

// --- Ethernet MAC-address PROM ($08000) ---
// The Apple presentation the SONIC driver consumes (SonicEnet.a @GetAddr):
// bytes 0-5 hold the station address with each byte BIT-REVERSED (the
// driver's NormAddr undoes it), and the XOR of all eight bytes must equal
// $FF (checksum probed before the address is trusted).  The eight bytes
// repeat across the PROM's $2000 window (mcu.md): MacTest Pro reads sixteen
// bytes and checks each half's XOR.  Each family supplies its bytes
// (quadra040_t.mac_prom).

uint8_t quadra040_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return quadra040_st(cfg)->mac_prom[win_off & 7u];
}

void quadra040_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)cfg;
    (void)addr;
    LOG(2, "MAC PROM write $%X = $%02X ignored (read-only)", win_off & 7u, value);
}

// --- SONIC ($0A000; 16-bit registers on 4-byte spacing) ---
// The register value rides the LOW half of the 32-bit slot (current MAME
// maps it the same way); the engine byte-decomposes wider accesses, so reads
// serve bytes 2-3 of each slot and a write COMMITS when byte 3 lands (the
// Quadra driver/tests use 32-bit accesses throughout -- SonicEqu.a SONIC32).
// Bytes 0-1 read as zero and their writes are ignored.

uint8_t quadra040_sonic_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    quadra040_t *q = quadra040_st(cfg);
    uint32_t off = win_off & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte < 2)
        return 0;
    uint16_t v = sonic_reg_read(q->sonic, off >> 2);
    return (byte == 2) ? (uint8_t)(v >> 8) : (uint8_t)v;
}

void quadra040_sonic_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    quadra040_t *q = quadra040_st(cfg);
    uint32_t off = win_off & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte == 2)
        q->sonic_byte2 = value;
    else if (byte == 3)
        sonic_reg_write(q->sonic, off >> 2, (uint16_t)((q->sonic_byte2 << 8) | value));
}

// --- YANCC / Kiwi ($28000; yancc.h) ---

uint8_t quadra040_yancc_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return yancc_read(&quadra040_st(cfg)->yancc, win_off, false, cpu_get_pc(cfg->cpu));
}

uint8_t quadra040_yancc_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return yancc_read(&quadra040_st(cfg)->yancc, win_off, true, 0); // no first-touch log
}

void quadra040_yancc_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    yancc_write(&quadra040_st(cfg)->yancc, win_off, value, cpu_get_pc(cfg->cpu));
}

// ============================================================
// Built-in video as a display device
// ============================================================
// The DAFB's monitor straps, in Apple's indexed numbering (dafb.h): the
// passive 3-bit codes plus the GoldFish 16" on the extended probe, each
// measured through a Q700/Q900 ROM boot ("dafb.c": only GoldFish produced a
// new raster among the extended codes).  Code 7 with no extended answer is
// an unplugged port.  The djMEMC's WombatDAFB drives the same set.

// One monitor the DAFB drives: its catalogue id and its strap.
typedef struct dafb_monitor {
    const char *monitor; // catalogue id (doubles as the strap's token)
    uint8_t sense;
} dafb_monitor_t;

static const dafb_monitor_t k_dafb_monitors[] = {
    {"13in_rgb",            0x6u                 },
    {"12in_rgb",            0x2u                 },
    {"15in_portrait",       0x1u                 },
    {"15in_portrait_color", 0x5u                 },
    {"16in_rgb",            DAFB_SENSE_INDEXED_GF},
    {"21in_rgb",            0x0u                 },
    {"21in_mono",           0x3u                 },
    {"none",                0x7u                 },
    {NULL,                  0                    },
};

// The table's rows a ROM can show: all of them, or the passive codes only.
static bool dafb_monitor_at(bool passive_only, size_t i, const char **id, const char **monitor) {
    size_t n = 0;
    for (const dafb_monitor_t *m = k_dafb_monitors; m->monitor; m++) {
        if (passive_only && m->sense > 7)
            continue;
        if (n++ == i) {
            *id = *monitor = m->monitor;
            return true;
        }
    }
    return false;
}

bool quadra040_monitor_at(size_t i, const char **id, const char **monitor) {
    return dafb_monitor_at(false, i, id, monitor);
}

bool quadra040_monitor_passive_at(size_t i, const char **id, const char **monitor) {
    return dafb_monitor_at(true, i, id, monitor);
}

bool quadra040_monitor_sense(const char *id, uint8_t *out) {
    for (const dafb_monitor_t *m = k_dafb_monitors; m->monitor; m++) {
        if (strcmp(m->monitor, id) == 0) {
            *out = m->sense;
            return true;
        }
    }
    return false;
}
