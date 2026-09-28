// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q700.c
// Macintosh Quadra 700 ("Spike", 25 MHz 68040, October 1991) — the desktop
// member of the MCU/DAFB family and Granny Smith's first 68040 machine.
// Low-speed I/O is direct and IIci-like: VIA1+VIA2, classic
// RTC/PRAM, VIA-shifter ADB transceiver, direct SCC and SWIM.  No IOPs, no
// Caboose, one SCSI bus.  Shares the 420DBFF3 ROM with the Quadra 900 (model
// sense on VIA1 PA distinguishes them).

#include "mcu.h"

#include "mac_host_io.h"
#include "machine.h"
#include "slot_tables.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu — the CPU-owned 040 MMU register file
#include "dafb.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "mmu040.h"
#include "nubus.h"
#include "pram_defaults.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// ============================================================
// VIA callbacks (direct IIci-like wiring)
// ============================================================

static inline mcu_state_t *q700_state(config_t *cfg) {
    return (mcu_state_t *)cfg->machine_context;
}

// VIA1 outputs: PA5 = floppy head select; PB = classic RTC serial
// (bits 0-2) + ADB ST lines (bits 4-5) — the IIci pattern verbatim.
static void q700_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    mcu_state_t *st = q700_state(cfg);

    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        // No overlay bit here: the MCU's overlay is access-triggered (mcu.c).
    } else {
        // Bits 4-5: ADB state lines (ST0/ST1) -> ADB controller.  The
        // ST-transition filter and its port-B shadow live in adb.c now.
        if (st->adb)
            adb_port_b_output(st->adb, output);
        rtc_via1_pb_output(cfg->rtc, output);
    }
}

// VIA2 outputs: PB4/PB3/PB0 drive the DFAC serial control on the Q700 —
// logged only.
static void q700_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// EASC interrupt → VIA2 CB1.
static void q700_asc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    // CB1 is active-low on the board; the 6522 PCR polarity handles edges.
    via_input_c(cfg->via2, 1, 0, active ? 0 : 1);
}

// 53C96 INT output → VIA2 CB2 (combined SCSI interrupt request).
static void q700_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 1, 1, active ? 0 : 1);
}

// SONIC INT → VIA2 PA0 (active-low) through the /SLOTIRQ aggregate
// (A/UX level-3 remap not modeled).
static void q700_sonic_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    mcu_slot_irq_source(cfg, 0, active);
}

// SONIC bus-master DMA: guest-physical accesses through the bus resolver
// (no IOMMU on this family — CPU MMU is not in the path).
// ============================================================
// Device construction (mcu_board_t.build_devices)
// ============================================================

static int q700_build_devices(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = q700_state(cfg);
    const mcu_board_t *board = (const mcu_board_t *)cfg->machine->board;
    const mcu_board_desc_t *desc = board->desc;

    // The Q700 bit-bangs the classic RTC on VIA1.
    rtc_set_via(cfg->rtc, cfg->via1);

    // Model sense on VIA1 PA from the board descriptor ($C0: the ROM's identify
    // table matches PA & $56 == $40 for the Q700), plus the PA0 diagnostic
    // strap.  These were eight hardcoded via_input calls that ignored
    // desc->via1_pa_model entirely, leaving the field dead on this board while
    // q900/q950 read it.
    mcu_apply_via1_model_sense(cfg, desc);
    // CA1/CB1 idle high (tick + ADB clock reference edges).
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    // VIA2 PA: slot/video/Ethernet requests, all active-low → idle high
    // (Q700 exposes D, E/PDS, video, Ethernet).
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via2, 0, bit, 1);
    via_input_c(cfg->via2, 0, 0, 1); // CA1 /SLOTIRQ idle
    via_input_c(cfg->via2, 1, 0, 1); // CB1 sound idle

    st->adb = adb_init(cfg->via1, cfg->scheduler, cp);
    cfg->adb = st->adb;

    if (cp)
        mac_checkpoint_restore_images(cfg, cp);

    // The bus/target model carries the disks and CD; the 53C96 chip model
    // is the protocol front-end driving it through the external-initiator
    // API (there is no NCR 5380 register file on this family).
    cfg->scsi = scsi_init(cp);
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    scsi_53c96_set_irq_callback(st->scsi96, q700_scsi96_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96, cfg->scsi);

    // SONIC Ethernet (~20 MHz part on the Q700, no wire in v1).
    st->sonic = sonic_init(cp);
    sonic_set_irq_callback(st->sonic, q700_sonic_irq, cfg);
    // SONIC bus-master DMA: the shared guest-physical port.
    sonic_set_memory_port(st->sonic, &dma_mem_port_physical);

    st->asc = asc_init(NULL, cfg->scheduler, cp); // EASC: ASC-compatible core
    asc_set_mix(st->asc, ASC_MIX_CH_A);
    asc_set_irq_handler(st->asc, q700_asc_irq, cfg);
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, cp);
    cfg->floppy = st->floppy;

    if (mcu_build_dafb(cfg, cp) != 0)
        return -1;

    // Bus-side physical resolver for the 040 walker: RAM at 0, the 1 MiB ROM
    // mirroring through the aperture.  ram_size_max is the full 1 GiB RAM
    // aperture — RAM-sizing probes above installed memory read $FF rather
    // than bus-erroring (flat functional model).
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    st->bus_mmu = mmu_init(ram_base, ram_size, 0x40000000u, rom_data, cfg->machine->rom_size, desc->common.rom_base,
                           desc->common.rom_end);
    if (!st->bus_mmu) {
        LOG(0, "Error: out of memory constructing the 040 bus MMU");
        return -1;
    }
    g_mmu = st->bus_mmu;
    // Attach the CPU-owned 040 register file: translation now dispatches to
    // the mmu040 walker; `enabled` mirrors TC.E.  (The cpu.mmu debug object
    // is bound by cpu_init itself — the 040-shaped mmu040_class in cpu.c.)
    mmu_attach_mmu040(st->bus_mmu, (mmu040_state_t *)cfg->cpu->mmu);

    setup_images(cfg);

    // Bind the I/O island + DAFB apertures + overlay, then arm the overlay.
    mcu_io_bind(&st->io, cfg, desc, st->asc, st->floppy);
    mcu_memory_layout(cfg);

    // Slot probing bus-errors in the NuBus windows (needed by the ROM's
    // slot scan even with no cards; the mapped VRAM aperture wins first).
    memory_set_bus_error_range(cfg->mem_map, desc->common.bus_err_lo, desc->common.bus_err_hi);

    if (cp)
        mcu_restore_private(cfg, cp);
    return 0;
}

// ============================================================
// Machine descriptor
// ============================================================

// Official totals 4/8/20 MB plus the accepted-in-practice 36/68 MB SIMM
// configurations (extended sizes, flagged as such).
static const uint32_t q700_ram_options_kb[] = {4096, 8192, 20480, 36864, 69632, 0};

static const scsi_bus_decl_t q700_scsi_buses[] = {
    {.object = "scsi", .label = "SCSI", .slots = mac_scsi_slots_hd01},
    {0},
};

// NuBus topology: two sockets, D and E; the PDS is mechanically
// aligned with slot E (a PDS card precludes a NuBus card there — not modeled
// as a constraint in v1).  The built-in DAFB video is pseudo-slot 9: its
// declaration ROM lives in the system ROM and its apertures are mapped
// directly by the substrate, so it is not a card on this bus.
static const nubus_slot_decl_t q700_nubus_slots[] = {
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET},
    {0},
};

static const mcu_board_desc_t q700_board_desc = {
    .common =
        {
                 .chipset = "MCU+DAFB",
                 .rom_base = 0x40000000u,
                 .rom_end = 0x50000000u,
                 .io_ranges = mcu_q700_io_ranges,
                 .io_mirror_mask = 0x0003FFFFu, // 256 KiB island
            .io_unmapped_read = 0xFF, // undecoded island reads float high (see mac030_glue.h)
            .bus_err_lo = 0xF1000000u, // slots $1-$E: this board decodes below $F9
            .bus_err_hi = NUBUS_BERR_HI,
                 },
    .ram_onboard_size = 0x00400000u, // 4 MB soldered = bank A; SIMM bank B follows
    .ram_bank_count = 2, // 4 MB soldered + one four-SIMM bank
    .via1_pa_model = 0xC0, // Q700 model sense
    // Modelled maxed.  Base is 512 KiB soldered plus three optional
    // 256 KiB-SIMM-pair banks.
    .dafb_vram_size = 0x00200000u,
};

static const mcu_board_t q700_board = {
    .desc = &q700_board_desc,
    .via1_output = q700_via1_output,
    // No VIA1 shift-out routing: adb.c reads the VIA's shift register
    // directly at each port-B ST transition, because in mode 7 the ADB
    // transceiver clocks the shift, not the VIA's internal timer, and the
    // ROM's SR writes during interrupt handling fire the callback
    // spuriously (BUG-004).  via.c tolerates a NULL here.
    .via1_shift_out = NULL,
    .via2_output = q700_via2_output,
    .build_devices = q700_build_devices,
};

const hw_profile_t machine_q700 = {
    .name = "Macintosh Quadra 700",
    .id = "q700",

    .cpu_model = 68040,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x4400000, // 68 MB (4 MB soldered + 4x16 MB SIMMs)
    .rom_size = 0x100000, // 1 MB

    .ram_options = q700_ram_options_kb,
    .floppy_slots = mac_floppy_slots_1hd,
    .scsi_buses = q700_scsi_buses,
    .has_cdrom = true,
    .cdrom_id = 3,

    .nubus_slots = q700_nubus_slots,

    .pram = &pram_defaults_mac_ii,
    .substrate = &mcu_substrate,
    .board = &q700_board,
};
