// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// q900.c
// Macintosh Quadra 900 ("Eclipse", 25 MHz 68040, October 1991) — the tower
// member of the MCU/DAFB family (proposal-machine-quadra-700-900-950.md
// Phase G).  Shares the 420DBFF3 ROM with the Quadra 700; the ROM picks the
// tower paths from the VIA1 PA model sense (PA & $56 == $50) and the box's
// ProductInfo flags (UniversalTables.a InfoQuadra900):
//   * ClockEgret + Caboose — RTC/PRAM/power/keyswitch behind an
//     Egret-protocol system manager on VIA1's SR + PB3/PB4/PB5 handshake
//     (the "Caboose" firmware is Egret-compatible; ref §15.14 [A][R])
//   * ADBIop — ADB behind the SWIM/ADB IOP (like the IIfx), NOT Caboose
//   * SCC and SWIM behind two Apple PIC/IOPs at island $C000 / $1E000
//     (host register layout identical to the IIfx PIC; ref §15.8)
//   * two NCR 53C96 SCSI buses: internal at $F000/$F100, external at
//     $F402/$F502 (OrwellDecoderTable [A]); INTs wire-OR onto VIA2 CB2
//   * five NuBus '90 slots A-E on VIA2 PA1-PA5 (ref §13.3)

#include "mcu.h"
#include "q900_internal.h"

#include "mac_host_io.h"
#include "machine.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu — the CPU-owned 040 MMU register file
#include "dafb.h"
#include "egret.h"
#include "floppy.h"
#include "image.h"
#include "iop.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "mmu040.h"
#include "nubus.h"
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

LOG_USE_CATEGORY_NAME("q900");

// ============================================================
// VIA callbacks (tower wiring: Caboose on VIA1, IOP IRQs on VIA2)
// ============================================================

static inline mcu_state_t *q900_state(config_t *cfg) {
    return (mcu_state_t *)cfg->machine_context;
}

// VIA1 outputs: PB carries the Egret/Caboose handshake (PB4 viaFull, PB5
// sysSes; PB3 xcvrSes is our input to the host) — VIA1InitQuadra900.  Port A
// is all model-sense inputs on the tower (no head select: floppy is behind
// the SWIM IOP).
void q900_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    mcu_state_t *st = q900_state(cfg);
    if (port == 1 && st->caboose)
        egret_via1_pb_input(st->caboose, output);
}

// VIA1 SR shift-out: a command byte for Caboose (Egret byte pump).
void q900_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    mcu_state_t *st = q900_state(cfg);
    if (st->caboose)
        egret_via1_shift_input(st->caboose, byte);
}

// VIA2 outputs: PB3/PB6 select the sound input source on the towers; DFAC
// programming itself lives behind Caboose (WrDFAC, accept-and-log there).
void q900_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// EASC interrupt → VIA2 CB1 (ref §13.4).
static void q900_asc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 1, 0, active ? 0 : 1);
}

// The two 53C96 INT outputs wire-OR (active-low) onto VIA2 CB2 (ref §12.7):
// don't drop the line while the other controller still requests.
static void q900_scsi_irq_update(config_t *cfg, int bit, bool active) {
    mcu_state_t *st = q900_state(cfg);
    st->scsi_irq_or = active ? (st->scsi_irq_or | (uint8_t)(1u << bit)) : (st->scsi_irq_or & (uint8_t) ~(1u << bit));
    via_input_c(cfg->via2, 1, 1, st->scsi_irq_or ? 0 : 1);
}

static void q900_scsi96_irq(void *context, bool active) {
    q900_scsi_irq_update((config_t *)context, 0, active);
}

static void q900_scsi96_ext_irq(void *context, bool active) {
    q900_scsi_irq_update((config_t *)context, 1, active);
}

// Level-4 source: the SCC chip INT (bypass-mode servicing) ORs with the SCC
// IOP host INT (mailbox traffic) — same combination the IIfx routes into its
// OSS source (ref §15.10/§15.12).
static void q900_scc_irq_update(config_t *cfg, int bit, bool active) {
    mcu_state_t *st = q900_state(cfg);
    st->scc_irq_or = active ? (st->scc_irq_or | (uint8_t)(1u << bit)) : (st->scc_irq_or & (uint8_t) ~(1u << bit));
    mac030_glue_update_ipl(cfg, MAC030_GLUE_IRQ_SCC, st->scc_irq_or != 0);
}

void q900_scc_irq(void *context, bool active) {
    q900_scc_irq_update((config_t *)context, 0, active);
}

static void q900_scc_iop_irq(void *context, bool active) {
    q900_scc_irq_update((config_t *)context, 1, active);
}

// SWIM/ADB IOP host INT → VIA2 CA2 (level 2; ref §15.10, VIA2InitQuadra900
// PCR "CA2 ind input neg active edge (SWIM IOP)").
static void q900_swim_iop_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 0, 1, active ? 0 : 1);
}

// SONIC INT → VIA2 PA0 (active-low) through the /SLOTIRQ aggregate.
static void q900_sonic_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    mcu_slot_irq_source(cfg, 0, active);
}

// SONIC bus-master DMA: guest-physical accesses through the bus resolver.
static uint32_t q900_sonic_mem_read(void *context, uint32_t phys, unsigned width) {
    (void)context;
    if (width == 1)
        return mmu_read_physical_uint8(g_mmu, phys);
    if (width == 2)
        return mmu_read_physical_uint16(g_mmu, phys);
    return mmu_read_physical_uint32(g_mmu, phys);
}

static void q900_sonic_mem_write(void *context, uint32_t phys, uint32_t value, unsigned width) {
    (void)context;
    if (width == 1)
        mmu_write_physical_uint8(g_mmu, phys, (uint8_t)value);
    else if (width == 2)
        mmu_write_physical_uint16(g_mmu, phys, (uint16_t)value);
    else
        mmu_write_physical_uint32(g_mmu, phys, value);
}

// DAFB video interrupt → VIA2 PA6 through the /SLOTIRQ aggregate.
static void q900_dafb_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    mcu_slot_irq_source(cfg, 6, active);
}

// ============================================================
// Device construction (mcu_board_t.build_devices)
// ============================================================

void q900_build_devices(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = q900_state(cfg);
    const mcu_board_t *board = (const mcu_board_t *)cfg->machine->board;
    const mcu_board_desc_t *desc = board->desc;

    // Model sense on VIA1 PA: the ROM's identify table matches
    // PA & $56 == $50 for the Q900 (InfoQuadra900: PA6=1, PA4=1, PA2=0,
    // PA1=0).  PA7 idles high (board default), PA0 is the diagnostic-mode
    // strap and must idle HIGH for a normal boot (same as the Q700).
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via1, 0, bit, (desc->via1_pa_model >> bit) & 1);
    via_input(cfg->via1, 0, 0, 1); // PA0 diagnostic strap high
    // CA1 idles high (60 Hz tick reference edge); CA2 is the keyswitch
    // "secure" sense — high = not in the secure position.
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 0, 1, 1);
    via_input_c(cfg->via1, 1, 0, 1);

    // VIA2 PA: Ethernet/slot/video requests, all active-low → idle high.
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via2, 0, bit, 1);
    via_input_c(cfg->via2, 0, 0, 1); // CA1 /SLOTIRQ idle
    via_input_c(cfg->via2, 0, 1, 1); // CA2 SWIM IOP host INT idle
    via_input_c(cfg->via2, 1, 0, 1); // CB1 sound idle
    via_input_c(cfg->via2, 1, 1, 1); // CB2 SCSI INT idle
    // VIA2 PB keyswitch/power/speed senses (HardwarePrivateEqu.a): PB0
    // keyswitch shadow (0 = SECURE) idles ON, PB1 bus unlocked, PB2 soft
    // power (0 = off) idles ON, PB5 CPU speed sense (0 = 25 MHz).
    via_input(cfg->via2, 1, 0, 1);
    via_input(cfg->via2, 1, 1, 1);
    via_input(cfg->via2, 1, 2, 1);
    via_input(cfg->via2, 1, 5, cfg->machine->freq >= 33000000 ? 1 : 0);

    // ADB device state: the tower's ADB bus is serviced by the SWIM/ADB IOP
    // (ADBIop in InfoQuadra900), not VIA1's shift register — pass NULL for
    // the VIA so slot-3 IOP traffic reaches adb_iop_transact() directly.
    st->adb = adb_init(NULL, cfg->scheduler, cp);
    cfg->adb = st->adb;

    if (cp)
        mac_checkpoint_restore_images(cfg, cp);

    // Internal SCSI bus: carries the configured disks + CD through the
    // shared bus/target model; the internal 53C96 fronts it.
    cfg->scsi = scsi_init(NULL, cp);
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    scsi_53c96_set_irq_callback(st->scsi96, q900_scsi96_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96, cfg->scsi);

    // External SCSI bus: electrically isolated second 53C96 (ref §12.1).
    // No default devices in v1 — selections time out like an empty chain.
    st->scsi_ext = scsi_init(NULL, cp);
    st->scsi96_ext = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    scsi_53c96_set_irq_callback(st->scsi96_ext, q900_scsi96_ext_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96_ext, st->scsi_ext);

    // SONIC Ethernet (20 MHz-class part on the Q900; no wire in v1).
    st->sonic = sonic_init(cp);
    sonic_set_irq_callback(st->sonic, q900_sonic_irq, cfg);
    sonic_set_memory_hooks(st->sonic, q900_sonic_mem_read, q900_sonic_mem_write, NULL);

    st->asc = asc_init(NULL, cfg->scheduler, cp); // EASC: ASC-compatible core
    asc_set_mix(st->asc, ASC_MIX_CH_A);
    asc_set_irq_handler(st->asc, q900_asc_irq, cfg);
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, cp);
    cfg->floppy = st->floppy;

    // Caboose: the Egret-protocol system manager (RTC/PRAM/power/keyswitch;
    // the ROM drives it through the same EgretMgr dispatch it uses for
    // Egret8 — ChkFirmware branches on the box flag, not the chip).  ADB
    // stays NULL here: tower ADB belongs to the SWIM IOP.
    st->caboose = egret_init(cfg->via1, cfg->rtc, NULL, cfg->scheduler, cp);
    assert(st->caboose != NULL);

    // The two Apple PIC/IOPs.  The host aperture layout matches the IIfx
    // PIC exactly (shared HardwarePrivateEqu.a equates), so the IIfx bridge
    // + firmware-behaviour models are reused as-is; only the base addresses
    // and IRQ routing differ.  Front-side devices ride the bypass windows.
    st->scc_iop =
        iop_init(SccIopNum, scc_get_memory_interface(cfg->scc), cfg->scc, q900_scc_iop_irq, cfg, cfg->scheduler, cp);
    st->swim_iop = iop_init(SwimIopNum, floppy_get_memory_interface(st->floppy), st->floppy, q900_swim_iop_irq, cfg,
                            cfg->scheduler, cp);

    st->dafb = dafb_init(0x00200000u, cp); // 2 MiB VRAM (Q900 maxed)
    assert(st->dafb != NULL);
    dafb_attach_scheduler(st->dafb, cfg->scheduler);
    dafb_set_irq_callback(st->dafb, q900_dafb_irq, cfg);
    // Consume unconditionally so a staged sense never leaks into a later
    // boot, but only APPLY it on a cold build: on a restore, dafb_init()
    // has already read the saved sense out of the checkpoint, and this
    // call would otherwise overwrite it with the default.
    uint8_t staged_sense = dafb_consume_pending_sense(); // default 6 = 13" RGB
    if (!cp)
        dafb_set_monitor_sense(st->dafb, staged_sense);
    dafb_set_version(st->dafb, desc->dafb_version); // 3 on the Q950 (DAFB 3)
    dafb_set_ac842a(st->dafb, desc->has_ac842a); // AC842a x555 on the Q950
    // TurboSCSI DRQ observation: channel 0 = internal, channel 1 = external.
    dafb_set_scsi_drq_query(st->dafb, 0, (dafb_drq_query_fn)scsi_53c96_dreq, st->scsi96);
    dafb_set_scsi_drq_query(st->dafb, 1, (dafb_drq_query_fn)scsi_53c96_dreq, st->scsi96_ext);

    // Bus-side physical resolver for the 040 walker (flat RAM model +
    // ROM-aperture mirrors; identical to the Q700 arrangement).
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    st->bus_mmu =
        mmu_init(ram_base, ram_size, 0x40000000u, rom_data, cfg->machine->rom_size, desc->rom_base, desc->rom_end);
    assert(st->bus_mmu != NULL);
    g_mmu = st->bus_mmu;
    mmu_attach_mmu040(st->bus_mmu, (mmu040_state_t *)cfg->cpu->mmu);

    setup_images(cfg);

    // Bind the I/O island (tower decode: IOP apertures + dual SCSI), then
    // hook the IOP host interfaces the shared table routes to.
    mcu_io_bind(&st->io, cfg, desc, st->asc, st->floppy);
    st->io.handle[MAC030_DEV_SCC_IOP] = st->scc_iop;
    st->io.iface[MAC030_DEV_SCC_IOP] = iop_get_memory_interface(st->scc_iop);
    st->io.handle[MAC030_DEV_SWIM_IOP] = st->swim_iop;
    st->io.iface[MAC030_DEV_SWIM_IOP] = iop_get_memory_interface(st->swim_iop);
    mcu_memory_layout(cfg);

    // Slot probing bus-errors in the NuBus windows.
    memory_set_bus_error_range(cfg->mem_map, desc->bus_err_lo, desc->bus_err_hi);

    if (cp)
        mcu_restore_private(cfg, cp);
}

// ============================================================
// Machine descriptor
// ============================================================

// Four banks of four equal SIMMs (ref §8.4); geometrically valid shipping
// totals with the 4 MB base configuration.
static const uint32_t q900_ram_options_kb[] = {4096, 8192, 16384, 20480, 32768, 65536, 0};

static const struct floppy_slot q900_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot q900_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

// NuBus topology (ref §10.3): five NuBus '90 sockets A-E; the 040 PDS is
// mechanically aligned with slot E.  Built-in DAFB video is pseudo-slot 9.
static const nubus_slot_decl_t q900_nubus_slots[] = {
    {.slot = 0xA, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xB, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET},
    {0},
};

static const mcu_board_desc_t q900_board_desc = {
    .chipset = "MCU+DAFB",
    .rom_base = 0x40000000u,
    .rom_end = 0x50000000u,
    .io_ranges = mcu_q900_io_ranges,
    .ram_bank_count = 4, // sixteen SIMM sockets = four four-SIMM banks
    .io_mirror_mask = 0x0003FFFFu, // 256 KiB island (ref §6.1)
    .io_unmapped_read = 0xFF, // undecoded island reads float high (see mac030_glue.h)
    .slots = q900_nubus_slots,
    .bus_err_lo = 0xF1000000u,
    .bus_err_hi = 0xFEFFFFFFu,
    .via1_pa_model = 0xD0, // Q900 model sense: PA & $56 == $50 (InfoQuadra900)
};

static const mcu_board_t q900_board = {
    .desc = &q900_board_desc,
    .via1_output = q900_via1_output,
    .via1_shift_out = q900_via1_shift_out,
    .via2_output = q900_via2_output,
    .build_devices = q900_build_devices,
    .scc_irq = q900_scc_irq, // SCC chip INT ORs with the SCC IOP host INT
};

const hw_profile_t machine_q900 = {
    .name = "Macintosh Quadra 900",
    .id = "q900",

    .cpu_model = 68040,
    .freq = 25000000, // 25 MHz
    .mmu_kind = MMU_68040,

    .address_bits = 32,
    .ram_default = 0x800000, // 8 MB
    .ram_max = 0x4000000, // 64 MB (16 SIMM slots, four 4-SIMM banks)
    .rom_size = 0x100000, // 1 MB (shared 420DBFF3 image)

    .ram_options = q900_ram_options_kb,
    .floppy_slots = q900_floppy_slots,
    .scsi_slots = q900_scsi_slots,
    .has_cdrom = true, // internal CD option shipped on the towers
    .cdrom_id = 3,

    .nubus_slots = q900_nubus_slots,

    .substrate = &mcu_substrate,
    .board = &q900_board,
};
