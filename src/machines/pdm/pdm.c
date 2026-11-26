// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pdm.c
// The PDM family substrate (Power Macintosh 6100/7100/8100) — the first
// machines whose main CPU is the PowerPC 601 (config_t.cpu is NULL; the 68k
// world exists only as ROM bytes the 601 executes).  See pdm.h.
//
// Memory model (Developer Note Table 2-5, as exercised by the shipping
// ROM):
//   $00000000-$3FFFFFFF  DRAM bank windows, HMC-owned (hmc.c)
//   $40000000-$4FFFFFFF  the 4 MB ROM repeating every 4 MB (HWInit runs
//                        from the $40300000 alias, the OS from $40800000)
//   $50F00000-$50F4FFFF  the AMIC-decoded I/O island (amic.c + hmc port)
//   $5FFFF000            machine-ID page ($5FFFFFFC)
//   $90000000-$EFFFFFFF  NuBus super slot space, and
//   $F0000000-$FEFFFFFF  the BART register file + NuBus/PDS slot space
//                        (bart.c) — a window with no card behind it takes a
//                        recoverable transfer error
//   the rest of $60000000-$FEFFFFFF  decoded by nobody: reads $FF
//   $FF000000-$FFFFFFFF  ROM alias (601 reset vector fetches $FFF00100)

#include "pdm.h"

#include "cuda.h" // the shared behavioral Cuda model (core/peripherals/)

#include "adb.h"
#include "appletalk.h"
#include "checkpoint.h"
#include "config_seed.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h" // the shared config_t-owned delete chain
#include "nubus.h"
#include "page_fill.h"
#include "ppc.h"
#include "ppc_build.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "swim3.h"
#include "via.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// ============================================================
// The family's NuBus topology (shared by the 7100 and the 8100)
// ============================================================

// Apple's own comparison table gives the family as 6100 = 1*, 7100 = 3,
// 8100 = 3 ("NuBus slots", Developer Note vol. 1), and its prose treats all
// four 7100/8100 variants as one: "The Power Macintosh 7100/66, 7100/66AV,
// 8100/80, and 8100/80AV contain three NuBus slots."  Hence one table.  The
// 6100's asterisk is the optional PDS adapter that carries the bridge itself
// -- see pm6100.c, which declares no slots AND no BART.
//
// The three NuBus connectors behind BART: $C/$D/$E.
//
// This is what the SOFTWARE uses, and it is the thing that matters — the
// slot number selects the address window a card answers in, the sResource
// the Slot Manager enumerates, and the pseudo-VIA2 interrupt bit the OS
// enables.  Measured on the shipping ROM: a booted 8100 enables slot-
// interrupt bits $38, i.e. bits 3/4/5, which under the Mac II bit = slot-9
// numbering are exactly $C/$D/$E — always those three, whichever connector
// holds a card — with bit 6 the built-in video VBL (it appears in the mask
// only when built-in video exists).  The ROM's own PDM slot-interrupt path
// masks the slot bits with $78, bits 3-6, agreeing.
//
// An earlier revision of this file declared $B/$C/$D from the schematic
// silkscreen (051-0333 rev A sheet 22, where the 96-pin connectors
// J11/J12/J13 are labelled NuBus Slot B, C and D).  That numbering is a
// board-level label, not the slot ID the software uses: a card seated in
// $B lands on interrupt bit 2, which nothing enables and nothing services,
// so its /NMRQ latched and stayed latched forever.  The Slot Manager then
// never ran that slot's VBL task queue — which, when the card is the main
// screen, is where the cursor task lives, so the mouse stopped moving.
// Each ships empty; the boot document names a card per slot.
//
// Apple's names for them are the silkscreen's -- NuBus slot B, C and D -- and
// those are what the configuration shows, with the slot ID the software uses
// as the detail.
const struct nubus_slot_decl pdm_nubus_slots_cde[] = {
    {.slot = 0xC, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot B", .fill_order = 1},
    {.slot = 0xD, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot C", .fill_order = 2},
    {.slot = 0xE, .kind = NUBUS_SLOT_SOCKET, .label = "NuBus slot D", .fill_order = 3},
    {0},
};

// ============================================================
// I/O island dispatch ($50F00000-$50F4FFFF)
// ============================================================
// All AMIC registers are byte-wide; wider accesses decompose into bytes,
// big-endian (MSB at the lowest address) — the convention every PDM driver
// programs multi-byte quantities with.

static uint8_t pdm_io_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= PDM_AMIC_ISLAND_SIZE)
        return pdm_hmc_read(cfg, offset - PDM_AMIC_ISLAND_SIZE);
    return pdm_amic_read(cfg, offset);
}

// An inspection: each chip's peek, nothing on the island changed.
static uint8_t pdm_io_peek8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= PDM_AMIC_ISLAND_SIZE)
        return pdm_hmc_peek(cfg, offset - PDM_AMIC_ISLAND_SIZE);
    return pdm_amic_peek(cfg, offset);
}

static void pdm_io_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= PDM_AMIC_ISLAND_SIZE)
        pdm_hmc_write(cfg, offset - PDM_AMIC_ISLAND_SIZE, value);
    else
        pdm_amic_write(cfg, offset, value);
}

static uint16_t pdm_io_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((pdm_io_read8(ctx, offset) << 8) | pdm_io_read8(ctx, offset + 1));
}

static uint32_t pdm_io_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)pdm_io_read16(ctx, offset) << 16) | pdm_io_read16(ctx, offset + 2);
}

static void pdm_io_write16(void *ctx, uint32_t offset, uint16_t value) {
    pdm_io_write8(ctx, offset, (uint8_t)(value >> 8));
    pdm_io_write8(ctx, offset + 1, (uint8_t)value);
}

static void pdm_io_write32(void *ctx, uint32_t offset, uint32_t value) {
    pdm_io_write16(ctx, offset, (uint16_t)(value >> 16));
    pdm_io_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Memory layout
// ============================================================

// Map the 4 MB ROM image repeating across a window of direct read-only
// pages (offset = addr & $3FFFFF — the wrap HWInit's self-rebase, the
// $40800000 OS view, and the $FFF00100 reset fetch all rely on).
static void pdm_map_rom_window(config_t *cfg, uint32_t base, uint32_t window) {
    uint8_t *rom = ram_native_pointer(cfg->memory_map, cfg->ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t first = base >> MEM_PAGE_SHIFT;
    uint32_t pages = window >> MEM_PAGE_SHIFT;
    for (uint32_t p = 0; p < pages; p++)
        page_fill_identity_sv(first + p, rom + ((p << MEM_PAGE_SHIFT) % rom_size), false);
}

static void pdm_memory_layout(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);

    // ROM: the whole $4xxxxxxx window plus the top-of-memory alias.
    pdm_map_rom_window(cfg, 0x40000000u, 0x10000000u);
    pdm_map_rom_window(cfg, 0xFF000000u, 0x01000000u);

    // The AMIC/HMC I/O island.
    st->io_interface.read_uint8 = pdm_io_read8;
    st->io_interface.read_uint16 = pdm_io_read16;
    st->io_interface.read_uint32 = pdm_io_read32;
    st->io_interface.write_uint8 = pdm_io_write8;
    st->io_interface.write_uint16 = pdm_io_write16;
    st->io_interface.write_uint32 = pdm_io_write32;
    st->io_interface.peek_uint8 = pdm_io_peek8; // wider peeks compose
    memory_map_add(cfg->memory_map, 0x50F00000u, 0x00050000u, "I/O", &st->io_interface, cfg);

    // Machine-ID page (hmc.md): byte reads deliver $A55A30xx; a 32-bit read
    // returns only the low half -- the ROM's long-probe must not find the
    // signature -- and the rest of the page reads 0.  Writes are ignored.
    cpuid_page_install(&st->cpuid, cfg->memory_map, 0xA55A0000u | pdm_board(cfg)->machine_id);
    st->cpuid.fill = 0x00; // nothing else in the page answers non-zero
    st->cpuid.long_low_half = true; // no $A55A signature on long reads

    // BART: the register file plus every window the bridge claims — slot
    // space, super slot space and the PDS slot-$E window — each of them
    // registered EMPTY, so an access no card answers takes the recoverable
    // transfer error the Slot Manager's probe expects.  Must run before
    // nubus_init, whose cards overlay the pages they answer (bart.c).
    //
    // Undecoded space outside those windows is not claimed by anything on
    // the board: AMIC's 40 us error there is documented as unrecoverable
    // ("forces restart"), so this model leaves such accesses reading $FF
    // rather than faulting — nothing in the ROM depends on that path, and a
    // fault would be the wrong KIND of failure.
    pdm_bart_init(cfg);

    // DRAM bank windows per the HMC's power-on state.
    pdm_hmc_remap(cfg);
}

// ============================================================
// VIA1 (the AMIC pseudo-VIA) callbacks — Cuda transport
// ============================================================

// Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out, PB5 TIP
// out — the classic Cuda bit positions); the SR shift-out is a command byte.
static void pdm_via1_output(void *context, uint8_t port, uint8_t value) {
    pdm_state_t *st = pdm_st((config_t *)context);
    cuda_via1_port_output(st ? st->cuda : NULL, port, value);
}

static void pdm_via1_shift_out(void *context, uint8_t byte) {
    pdm_state_t *st = pdm_st((config_t *)context);
    cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// VIA1 aggregate IRQ → ICR bit 0.
static void pdm_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_source(cfg, PDM_ICR_VIA1, active);
}

// 53C9x INT pins → the pseudo-VIA2 device bank (level-sensitive).
static void pdm_scsi96a_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_scsi_irq(cfg, 0, active);
}

static void pdm_scsi96b_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_scsi_irq(cfg, 1, active);
}

// SCC chip INT (one line for both channels) → AMIC ICR source bit 2, 68k
// level 4; channel discrimination is the guest's
// job via RR2B/RR3.
static void pdm_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (pdm_st(cfg))
        pdm_amic_set_source(cfg, PDM_ICR_SCC, active);
}

// ============================================================
// Substrate lifecycle
// ============================================================

// The substrate-private part: the HMC config, the AMIC register file, the
// SWIM3's plain-data prefix, the ICR sources and BART.
#define PDM_PRIVATE_CP_FIELDS(OP, cp, st)                                                                              \
    CP_FIELD(OP, cp, (st)->hmc);                                                                                       \
    CP_FIELD(OP, cp, (st)->amic);                                                                                      \
    SWIM3_CP(OP, cp, &(st)->swim3); /* the plain-data prefix; the bind wires the tail */                               \
    CP_FIELD(OP, cp, (st)->icr_sources);                                                                               \
    CP_FIELD(OP, cp, (st)->bart)

// Saves the substrate-private part.
static void part_save_pdm_private(void *obj, checkpoint_t *cp) {
    pdm_state_t *st = obj;
    PDM_PRIVATE_CP_FIELDS(system_write_checkpoint_data, cp, st);
}

// Ariel's identity part: the monitor strapped to the built-in port.
static void part_save_ariel_monitor(void *obj, checkpoint_t *cp) {
    pdm_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->video.sense, sizeof(st->video.sense), "ariel.monitor");
}

static int pdm_init(config_t *cfg, checkpoint_t *cp) {
    pdm_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    // Core: memory map, the 601, the scheduler on the PPC seam.  CPI is
    // 1.0 — the 601 is near-1-CPI on HWInit's measurement loop, and 1.0
    // makes the measured clock land exactly on the snap-table value.  The
    // 601's RTC input: 7.8336 MHz on every PDM board.  No 68k MMU owns this
    // machine's page table, so host-backed regions that core code registers
    // on the bus map — a NuBus card's VRAM and declaration ROM — are filled
    // through our own page filler.
    const ppc_core_opts_t core = {.fill_page = page_fill_identity_sv, .cpi = 1, .tb_hz = 7833600u};
    if (ppc_build_core(cfg, cp, &core) != 0)
        return -1;

    // The ESCC cell in Curio behind the AMIC island decode (single base
    // $50F04000, +0 bCtl / +2 aCtl / +4 bData / +6 aData;
    // PCLK 15.6672 MHz, RTxC 3.672 MHz synthesized by AMIC), the AppleTalk
    // connection on its LocalTalk channel, and the AMIC pseudo-VIA1: a real
    // 6522 core instance behind the island decode, its timers at 783.36 kHz
    // on every model (checked at pdm-rom-ladder rung L17).
    const ppc_lowspeed_opts_t lowspeed = {
        .scc_irq = pdm_scc_irq,
        .scc_rtxc_hz = 3672000,
        .via1_output = pdm_via1_output,
        .via1_shift_out = pdm_via1_shift_out,
        .via1_irq = pdm_via1_irq,
    };
    ppc_build_lowspeed(cfg, cp, &lowspeed);

    // ADB, and the behavioral Cuda (firmware 2.37 — the same 341S0788 part
    // as the AV machines) on the pseudo-VIA1 shift register + PB3/4/5.
    st->cuda = ppc_build_cuda(cfg, cp);
    if (!st->cuda)
        return -1;

    // The image list before the devices that reference it (floppy and SCSI
    // both resolve their media out of it by filename).
    machine_part_images(cfg, cp);

    // SCSI: the shared bus/target model on the Curio 53C94 cell (20 MHz
    // SCSI clock — the divided "Ethernet" oscillator).  The 8100 adds the
    // discrete 53CF96 on its fast internal bus (40 MHz), instantiated with
    // no bus attached: every select times out, the empty-bus presentation.
    // hd=/cd= media land on cfg->scsi, i.e. the Curio bus, on all models.
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    machine_part_begin(cfg, cp, "scsi96");
    st->scsi96[0] = scsi_53c96_init(cfg->scheduler, 20000000, cp);
    machine_part(cfg, cp, "scsi96", part_save_scsi96, st->scsi96[0]);
    scsi_53c96_set_irq_callback(st->scsi96[0], pdm_scsi96a_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96[0], cfg->scsi);
    scsi_53c96_set_bus_master(st->scsi96[0], true); // the AMIC pump
    if (pdm_board(cfg)->has_fast_scsi) {
        machine_part_begin(cfg, cp, "scsi96_fast");
        st->scsi96[1] = scsi_53c96_init(cfg->scheduler, 40000000, cp);
        machine_part(cfg, cp, "scsi96_fast", part_save_scsi96, st->scsi96[1]);
        scsi_53c96_set_irq_callback(st->scsi96[1], pdm_scsi96b_irq, cfg);
    }

    // The internal SuperDrive behind SWIM3.  No memory map: PDM decodes
    // the controller through the AMIC island, not through a floppy region
    // of its own, so the shared module only carries the drive and media.
    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy = floppy_init(FLOPPY_TYPE_SWIM3, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);

    // Board state + memory map.
    pdm_hmc_init(cfg);
    pdm_amic_init(cfg);
    pdm_amic_io_bind(cfg); // the island decode; construction only, not on reset
    pdm_amic_attach_object(cfg); // machine.amic; construction only, not on reset
    pdm_amic_register_events(cfg);
    pdm_awacs_register_events(cfg);
    pdm_swim3_bind(cfg);
    pdm_swim3_register_events(cfg);
    pdm_swim3_xfer_register_events(cfg);
    pdm_memory_layout(cfg);

    // The substrate's own part: the HMC config and AMIC register file are
    // plain data; derived mappings are rebuilt below.
    machine_part_begin(cfg, cp, "pdm");
    if (cp) {
        PDM_PRIVATE_CP_FIELDS(system_read_checkpoint_data, cp, st);
        pdm_swim3_bind(cfg);
        pdm_hmc_remap(cfg);
        via_redrive_outputs(cfg->via1);
        pdm_amic_recompute(cfg);
    }
    machine_part(cfg, cp, "pdm", part_save_pdm_private, st);

    // NuBus: the bus controller seats whatever cards the slots carry, and
    // each card registers its own regions over the empty windows BART
    // claimed above.
    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);

    // The monitor strapped to Ariel's built-in port: the document's on a
    // boot, the one the board was built with on a restore.  PDM_SENSE_NONE
    // (nothing connected) is what lets a NuBus card be the only screen.
    uint8_t monitor = cfg->build_opts.builtin_sense;
    machine_part_begin(cfg, cp, "ariel.monitor");
    if (cp)
        system_read_checkpoint_data(cp, &monitor, sizeof monitor, "ariel.monitor");

    // Presentation state, derived from the (possibly restored) register
    // file: the scanout descriptor over physical DRAM 0 and the AWACS
    // staging buffer + machine.sound node.
    pdm_video_init(cfg, monitor);
    machine_part(cfg, cp, "ariel.monitor", part_save_ariel_monitor, st);
    pdm_awacs_init(cfg);

    // Finish: the debugger (the m68k_board_finish shape).
    cfg->debugger = debug_init();

    // Fresh boot: start the free-running VBL raster (a restore rebinds
    // the checkpointed pending event through the registered type).
    if (!cp)
        pdm_amic_start_vbl(cfg);
    return 0;
}

// A power cycle's power-on-only half (machine_profile.h): Cuda stays
// powered, but the host side of its VIA1 handshake went down under it.
static void pdm_power_on(config_t *cfg) {
    cuda_host_power_cycle(pdm_st(cfg)->cuda);
}

static void pdm_bus_reset(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    // The chipset half only.  The 601 going back to its reset vector is the
    // CPU half and belongs to level 2 (system_machine_reset), not to the
    // board's /RESET net -- this used to call ppc_reset() from here, which is
    // why the Cuda path happened to work on PDM and TNT while leaving the AV
    // families' 68040 running.  (The 68k-RESET warm path re-enters HWInit
    // with MSR[IR] on and AMIC state SURVIVING — that path is guest-driven.)
    // Note pdm_amic_init memsets the whole AMIC.  The SWIM3 model is
    // deliberately NOT inside pdm_amic_t (pdm.h), so this cannot clear the
    // chip's bound fd/sched/backend pointers the way it once did.
    pdm_amic_init(cfg);
    // BART's latches go back to power-on with it; the NuBus /RESET fan-out
    // to the cards themselves is the bus controller's (system_reset_devices).
    memset(&st->bart, 0, sizeof(st->bart));
    scc_reset(cfg->scc);
    // The floppy CONTROLLER.  cfg->floppy (the drive and its media) is reset
    // by the shared chain; the SWIM3 is this family's controller and was the
    // one in the tree that survived a reset.
    swim3_reset(&st->swim3);
    for (int i = 0; i < 2; i++)
        if (st->scsi96[i])
            scsi_53c96_reset(st->scsi96[i]);
    st->hmc.cfg_lo = 0;
    st->hmc.cfg_hi = 0;
    st->hmc.bit_ptr = 0;
    st->hmc.wait_state = false;
    st->icr_sources = 0;
    pdm_hmc_remap(cfg);
    pdm_video_update(cfg); // blanked power-on raster follows the reset regs
    system_reset_common_devices(cfg); // scsi, NuBus, PCI -- the same net
}

static void pdm_teardown(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    if (st) {
        pdm_amic_detach_object(cfg);
        pdm_awacs_teardown(cfg);
        pdm_video_teardown(cfg);
        for (int i = 0; i < 2; i++) {
            if (st->scsi96[i]) {
                scsi_53c96_delete(st->scsi96[i]);
                st->scsi96[i] = NULL;
            }
        }
    }
    // The three devices that used to sit between cfg->scsi and cfg->via1 in
    // this family's own copy of the chain.  They move above the shared chain
    // (machine_teardown.h) rather than into it, because only PDM and TNT have
    // them; the single ordering change is that cfg->scsi is now freed after
    // these three instead of before.  That is safe: none of floppy_delete,
    // cuda_delete or adb_delete reads a SCSI handle, the 53C96 controllers
    // that DO hold the bus are already freed above, and scheduler_stop() ran
    // first so nothing can fire in between.  Cuda still goes before the via1,
    // rtc and adb it was handed at init, which is the ordering that matters.
    if (cfg->floppy) {
        floppy_delete(cfg->floppy);
        cfg->floppy = NULL;
    }
    if (st && st->cuda) {
        cuda_delete(st->cuda);
        st->cuda = NULL;
    }
    machine_teardown_config_devices(cfg); // the adb after the Cuda above, then the rest
}

// The AMIC-internal 60.15 Hz tick: VIA1 CA1 pulse.
static void pdm_trigger_vbl(config_t *cfg) {
    via_pulse_c(cfg->via1, 0, 0); // VBL: CA1 low then high
    image_tick_all(cfg);
    if (cfg->nubus)
        nubus_tick_vbl(cfg->nubus);
}

// A NuBus card's /NMRQ.  The umbrella edge is AMIC's own business (the
// pseudo-VIA2 "any slot" bit is recomputed from the slot levels on every
// read), so the bus controller's edge hint is not needed here.
static void pdm_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    pdm_bart_slot_irq(cfg, slot, active);
}

// Primary display: the Ariel scanout over physical DRAM 0 (ariel.c).
static struct display *pdm_display(config_t *cfg) {
    return pdm_video_display(cfg);
}

// Floppy: the one internal SuperDrive behind SWIM3.  Drive 1 is the only
// bay the family has — there is no external port on any PDM — so the shared
// mac_fd_* (bounded by the profile's floppy_slots) refuse slot 1.

const machine_substrate_t pdm_substrate = {
    .init = pdm_init,
    .bus_reset = pdm_bus_reset,
    .power_on = pdm_power_on,
    .teardown = pdm_teardown,
    .seed = mac_seed_rtc_pram,
    .nubus_slot_irq = pdm_nubus_slot_irq,
    .trigger_vbl = pdm_trigger_vbl,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = pdm_display,
};
