// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// av.c
// The Cyclone/Tempest AV family substrate (Quadra 840AV / Centris 660AV) —
// see av.h.  Implements the family lifecycle plus the pieces unique to this
// generation: the access-triggered ROM-at-zero overlay (no software overlay
// control exists), the YMCA 1-bit register file with the
// machine-ID straps, the CPU-ID register, the MUNI latches (with the 660AV
// bus-error probe behavior), and the family I/O island decode run on the
// shared mac030 engine.

#include "av.h"
#include "appletalk.h"
#include "regfile.h"

#include "config_seed.h"
#include "machine_teardown.h" // the shared config_t-owned delete chain

#include "civic.h"
#include "cuda.h"
#include "dsp.h"
#include "floppy.h"
#include "mace.h"
#include "psc.h"
#include "singer.h"
#include "vdc.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "mmu040.h"

#include "adb.h"
#include "checkpoint.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (attach the 040 walker to the bus resolver)
#include "debug.h"
#include "image.h"
#include "log.h"
#include "machine_checkpoint.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

static inline const av_board_t *av_board(config_t *cfg) {
    return (const av_board_t *)cfg->machine->board;
}

static inline av_state_t *av_st(config_t *cfg) {
    return (av_state_t *)cfg->machine_context;
}

// ============================================================
// YMCA register file ($50F30400)
// ============================================================
// Every register is one bit wide, addressed as a longword with the value in
// bit 31 — i.e. bit 7 of the big-endian MSB byte lane.  The engine
// decomposes wider accesses into bytes, so only lane 0 of each longword
// carries data.  The machine-ID straps read the board's nibble; everything
// else is a latch that reads back (speed/width semantics are not modelled —
// even the ROM only knows fixed patterns).

static uint8_t av_ymca_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu; // island offset $30400 + $000..$3FF
    if ((off & 3) != 0)
        return 0; // only the MSB byte lane carries the bit
    uint32_t idx = off >> 2;
    if (idx >= AV_YMCA_REG_COUNT)
        return 0;
    // Straps CPUID0..3 ($38/$3C/$40/$44): nibble bit n in strap register n.
    if (idx >= AV_YMCA_CPUID0 && idx < AV_YMCA_CPUID0 + 4) {
        uint8_t bit = (uint8_t)((av_board(cfg)->desc->strap_nibble >> (idx - AV_YMCA_CPUID0)) & 1);
        return (uint8_t)(bit << 7);
    }
    return (uint8_t)((st->ymca_regs[idx] & 1) << 7);
}

static void av_ymca_remap(config_t *cfg);

// The register index of bank `bank`'s bit `bit`: boundary A20..A26 are bits
// 0-6, the size code Sz0..Sz2 bits 7-9.
static inline uint32_t ymca_bank_reg(int bank, int bit) {
    return (AV_YMCA_BANK_BASE + (uint32_t)bank * AV_YMCA_BANK_STRIDE) / 4 + (uint32_t)bit;
}

static void av_ymca_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    if ((off & 3) != 0)
        return;
    uint32_t idx = off >> 2;
    if (idx >= AV_YMCA_REG_COUNT)
        return;
    if (idx >= AV_YMCA_CPUID0 && idx < AV_YMCA_CPUID0 + 4)
        return; // straps are inputs
    st->ymca_regs[idx] = (uint8_t)((value >> 7) & 1);
    LOG(3, "YMCA write $%03X = %d (pc=%08X)", off, st->ymca_regs[idx], cpu_get_pc(cfg->cpu));
    // A bank's ten bits are written boundary first and Sz2 last (the ROM's
    // split and merge passes), so its last write moves the bank.
    for (int b = 0; b < AV_YMCA_BANK_COUNT; b++)
        if (idx == ymca_bank_reg(b, AV_YMCA_BDRY_BITS + AV_YMCA_SIZE_BITS - 1)) {
            av_ymca_remap(cfg);
            break;
        }
}

// ============================================================
// MUNI ($50F30000)
// ============================================================
// Two latches: IntCntrl (+$00) and Control (+$08).  A 660AV without the
// NuBus adapter has no MUNI at all — reads AND writes of MUNI_Control must
// bus-error so the ROM's TestForMUNI clears MUNIExists (the speed-programming
// write in JumpIntoROM runs under a temp bus-error handler and is skipped).

static uint8_t av_muni_read_access(config_t *cfg, uint32_t win_off, uint32_t addr, bool peek) {
    (void)win_off; // this window's handler decodes from addr itself
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    uint32_t reg = off & ~3u;
    if (reg == AV_MUNI_CONTROL && !av_board(cfg)->desc->muni_present) {
        if (!peek)
            memory_signal_bus_error(addr, false);
        return 0xFF;
    }
    uint32_t v = (reg == AV_MUNI_CONTROL) ? st->muni_control : (reg == AV_MUNI_INTCNTRL) ? st->muni_intcntrl : 0;
    return be_lane8(v, off & 3);
}

static uint8_t av_muni_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    return av_muni_read_access(cfg, win_off, addr, false);
}
static uint8_t av_muni_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    return av_muni_read_access(cfg, win_off, addr, true);
}

static void av_muni_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    av_state_t *st = av_st(cfg);
    uint32_t off = addr & 0x3FFu;
    uint32_t reg = off & ~3u;
    unsigned lane = off & 3u;
    if (reg == AV_MUNI_CONTROL) {
        if (!av_board(cfg)->desc->muni_present) {
            memory_signal_bus_error(addr, true);
            return;
        }
        be_lane8_set(&st->muni_control, lane, value);
        LOG(2, "MUNI Control = $%08X (pc=%08X)", st->muni_control, cpu_get_pc(cfg->cpu));
    } else if (reg == AV_MUNI_INTCNTRL) {
        be_lane8_set(&st->muni_intcntrl, lane, value);
        LOG(2, "MUNI IntCntrl = $%08X (pc=%08X)", st->muni_intcntrl, cpu_get_pc(cfg->cpu));
    }
}

// ============================================================
// CPU-ID register ($5FFFFFFC = $A55A2830, read-only)
// ============================================================
// Registered as its own page-sized device window at $5FFFF000.  The ROM's
// GetCPUIDReg validates the $A55A signature AND that the location is not
// writable — writes are simply dropped, so the write-then-readback probe
// sees the constant and concludes "not writable".

#define AV_CPUID_VALUE 0xA55A2830u

static uint8_t av_cpuid_read8(void *ctx, uint32_t offset) {
    (void)ctx;
    if (offset >= 0xFFCu)
        return be_lane8(AV_CPUID_VALUE, offset & 3);
    return 0xFF; // nothing else decodes in this page — float high
}

static uint16_t av_cpuid_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((av_cpuid_read8(ctx, offset) << 8) | av_cpuid_read8(ctx, offset + 1));
}

static uint32_t av_cpuid_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)av_cpuid_read16(ctx, offset) << 16) | av_cpuid_read16(ctx, offset + 2);
}

static void av_cpuid_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(2, "CPU-ID write $%X = $%02X ignored (read-only)", offset, value);
}

static void av_cpuid_write16(void *ctx, uint32_t offset, uint16_t value) {
    av_cpuid_write8(ctx, offset, (uint8_t)value);
}

static void av_cpuid_write32(void *ctx, uint32_t offset, uint32_t value) {
    av_cpuid_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// I/O island decode ($50F00000, 256 KiB, mirrored at $50F40000)
// ============================================================

#define AV_VIA_IO_PENALTY 16
#define AV_SCC_IO_PENALTY 2
// Every other window on the island is a handler row, and every one of them
// completes a bus cycle -- so all of them pay the island's turnaround, the
// way the IIfx table has always charged its handler rows.
#define AV_IO_PENALTY 2

// 53C96 window handlers (defined with the SCSI wiring below).
static uint8_t av_scsi_read(config_t *cfg, uint32_t win_off, uint32_t addr);
static void av_scsi_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);
static uint8_t av_scsi_pdma_read(config_t *cfg, uint32_t win_off, uint32_t addr);
static void av_scsi_pdma_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);
static uint8_t av_scsi_peek(config_t *cfg, uint32_t win_off, uint32_t addr);
static uint8_t av_scsi_pdma_peek(config_t *cfg, uint32_t win_off, uint32_t addr);

//   base     end      device            penalty          xform            rd wr  rd_fn/wr_fn      name
const mac030_io_range_t av_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, AV_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_psc_via2_read, av_psc_via2_write, "psc_via2"},
    {0x04000, 0x08000, MAC030_DEV_SCC, AV_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x08000, 0x08080, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_mace_prom_read, av_mace_prom_write, "mac_prom"},
    {0x18000, 0x18100, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_scsi_read, av_scsi_write, "scsi_53c96",
     .peek_fn = av_scsi_peek},
    {0x18100, 0x18200, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_scsi_pdma_read, av_scsi_pdma_write, "scsi_rdma",
     .peek_fn = av_scsi_pdma_peek},
    {0x1C000, 0x1C200, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_mace_read, av_mace_write, "mace",
     .peek_fn = av_mace_peek},
    {0x2A000, 0x2A200, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_new_age_read, av_new_age_write, "new_age",
     .peek_fn = av_new_age_peek},
    {0x2E000, 0x2E100, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_civic_clk_read, av_civic_clk_write, "clock"},
    {0x30000, 0x30400, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_muni_read, av_muni_write, "muni",
     .peek_fn = av_muni_peek},
    {0x30400, 0x30800, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_ymca_read, av_ymca_write, "ymca"},
    {0x30800, 0x30C00, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_civic_seb_read, av_civic_seb_write, "sebastian",
     .peek_fn = av_civic_seb_peek},
    {0x31000, 0x33000, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_psc_reg_read, av_psc_reg_write, "psc"},
    {0x36000, 0x38000, 0, AV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, av_civic_read, av_civic_write, "civic"},
    {0}, // sentinel: end == 0
};

// Bind the family device set + board tables into the shared I/O engine.
static void av_io_bind(mac030_io_t *io, config_t *cfg, const av_board_desc_t *desc) {
    mac030_io_install(io, cfg, &desc->common);
    mac030_io_bind_dev(io, MAC030_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    if (cfg->scc) {
        mac030_io_bind_dev(io, MAC030_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    }
}

// ============================================================
// IRQ routing
// ============================================================

static const mac030_irq_route_t av_irq_routes_tbl[] = {
    {AV_IRQ_NMI,  7},
    {AV_IRQ_L6,   6},
    {AV_IRQ_L5,   5},
    {AV_IRQ_L4,   4},
    {AV_IRQ_L3,   3},
    {AV_IRQ_VIA2, 2},
    {AV_IRQ_VIA1, 1},
    {0,           0},
};

const mac030_irq_route_t *av_irq_routes(void) {
    return av_irq_routes_tbl;
}

void av_update_ipl(config_t *cfg, int source, bool active) {
    if (active)
        cfg->rt.irq |= source;
    else
        cfg->rt.irq &= ~source;
    int new_ipl = mac030_irq_resolve_ipl(av_irq_routes_tbl, (uint32_t)cfg->rt.irq);
    cpu_set_ipl(cfg->cpu, new_ipl);
    cpu_reschedule(cfg->scheduler);
}

// VIA1 interrupt line → IPL 1.
static void av_via1_irq(void *context, bool active) {
    av_update_ipl((config_t *)context, AV_IRQ_VIA1, active);
}

// substrate.nubus_slot_irq — the PSC aggregates NuBus slot interrupts itself,
// so the bus drives one SInt source per slot and the PSC raises the VIA2
// window's CA1 bit while any of them is asserted (psc.c psc_update_slot_bit).
// The umbrella edge is therefore the chip's business, not ours, exactly as on
// the MDU's RBV.
//
// Slots C/D/E map to SInt bits 3/4/5 (psc.h; the guest's PSCVIA2SlotInt reads
// PSCVIA2SInt under mask ~$78 -- slots C/D/E plus on-board VBL on bit 6 --
// and inverts, the register reading active-LOW).  So bit = slot - $9, and
// av_psc_slot_source owns the inversion.
//
// No AV board declares a slot table yet (.slots = NULL on both, "declared but
// unpopulated"), so nothing reaches this today.  It exists so the first AV
// declaration-ROM card does not have to discover that its /NMRQ went nowhere.
static void av_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    av_state_t *st = (av_state_t *)cfg->machine_context;
    if (!st || !st->psc)
        return;
    int bit = slot - 0x9;
    if (bit < 3 || bit > 5) // only C/D/E exist on this family
        return;
    av_psc_slot_source(st->psc, bit, active);
}

// ============================================================
// SCSI: the Curio's 53C96 at island $18000 ($10 register stride)
// ============================================================
// No pseudo-DMA on this platform (pdmaAddr = 0) — data moves through PSC
// channel 0, which the SCSI HAL POLLS (it never installs a channel-0
// handler).  The chip IRQ is level-sensitive into the PSC-VIA2 window,
// bits 3 and mirror 0.

static uint8_t av_scsi_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    return scsi_53c96_read(av_st(cfg)->scsi96, (addr & 0xFFu) >> 4);
}
static uint8_t av_scsi_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off, (void)addr;
    return scsi_53c96_peek(av_st(cfg)->scsi96, (addr & 0xFFu) >> 4);
}

static void av_scsi_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    scsi_53c96_write(av_st(cfg)->scsi96, (addr & 0xFFu) >> 4, value);
}

// Curio's rDMA pseudo-DMA port at +$100: the ROM's boot-time SCSI Manager
// (SCSIMgrHWPSC.a) streams 16-bit words through it — the engine
// byte-decomposes them, and byte order equals wire order.
static uint8_t av_scsi_pdma_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    return scsi_53c96_pdma_read8(av_st(cfg)->scsi96);
}
static uint8_t av_scsi_pdma_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off, (void)addr;
    return scsi_53c96_pdma_peek8(av_st(cfg)->scsi96);
}

static void av_scsi_pdma_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    scsi_53c96_pdma_write8(av_st(cfg)->scsi96, value);
}

// The chip INT drives ONLY the CB2-position bit (3).  Bit 0 is nominally
// a "SCSI mirror", but driving it as a second interrupt source feeds the
// ROM's pattern-indexed level-2 dispatcher combinations it never expects
// (IFR $09/$29): the SCSI service then runs on patterns whose table entries
// mis-classify, the manager's deferred-interrupt bookkeeping is left stale,
// and the next transaction's select is never issued — verified against the
// reference CD image, where the boot hangs in SCSIComplete's phase wait with
// the mirror driven and reaches the desktop without it.
static void av_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (!st || !st->psc)
        return;
    av_psc_via2_source(st->psc, AV_PSC_VIA2_SCSI_CB2, active);
}

// The channel-0 pump: the hardware's DREQ/DACK engine, functionally — while
// the chip has payload to move and the channel's active set is armed,
// shuttle bytes between the 53C96 FIFO and memory.  The SCSI HAL polls CIRQ
// in the CmdStat for completion and never installs a channel-0 handler.
//
// The phase gate matters: `scsi_53c96_dreq()` only reports "a DMA-mode
// transfer is armed", which stays true across a phase change, so without it
// the pump can drain bytes the CPU is about to read out of the FIFO during a
// STATUS or MESSAGE phase.
#define AV_SCSI_PUMP_NS  10000.0 // 10 us cadence
#define AV_SCSI_PUMP_MAX 2048 // bytes per firing (a CD sector burst)

// True while the bus is in a phase whose payload the DMA engine carries.
// DATA IN/OUT are the payload phases, and COMMAND is included because a
// DMA-mode select streams the CDB out of the same engine.  STATUS and the
// MESSAGE phases are always the CPU's through the FIFO, so pumping there
// would steal bytes the driver is about to read.
static bool av_scsi_data_phase(config_t *cfg) {
    int ph = scsi_get_bus_phase(cfg->scsi);
    return ph == scsi_data_in || ph == scsi_data_out || ph == scsi_command;
}

static void av_scsi_pump_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    av_state_t *st = av_st(cfg);
    bool running = false;
    if (st && st->psc && st->scsi96 && cfg->scsi) {
        int dir = av_psc_dma_dir(st->psc, AV_PSC_DMA_SCSI);
        // The channel being armed at all is what keeps the pump alive; a
        // transfer that has not reached its first DREQ yet still counts.
        running = dir >= 0;
        bool mem_to_scsi = dir == 0;
        int moved = 0;
        while (dir >= 0 && moved < AV_SCSI_PUMP_MAX && av_scsi_data_phase(cfg) && scsi_53c96_dreq(st->scsi96)) {
            uint8_t byte;
            if (dir == 1) { // device → memory
                byte = scsi_53c96_pdma_read8(st->scsi96);
                if (av_psc_dma_device_in(st->psc, AV_PSC_DMA_SCSI, &byte, 1) != 1)
                    break;
            } else { // memory → device
                if (av_psc_dma_device_out(st->psc, AV_PSC_DMA_SCSI, &byte, 1) != 1)
                    break;
                scsi_53c96_pdma_write8(st->scsi96, byte);
            }
            moved++;
            dir = av_psc_dma_dir(st->psc, AV_PSC_DMA_SCSI);
        }
        if (moved)
            running = true; // a pass that did work is asked again next tick
        // Did the loop stop because the TARGET ran out?  Same rule the AMIC
        // pump applies, and for the same reason -- this path had the identical
        // structure and never asked.
        scsi_53c96_dma_end_if_short(st->scsi96, moved, mem_to_scsi, av_scsi_data_phase(cfg));
    }
    // Re-arm only while there is something to pump.
    //
    // This used to re-arm unconditionally, from av_init onward, for the life
    // of the machine.  The cost is not the wake-ups: scheduler.md S1.2 gives
    // the sprint length as min(remaining_budget, cycles_to_next_event), so a
    // permanently-scheduled 10 us event caps EVERY sprint at 10 us of guest
    // time -- a few hundred cycles on a 660AV/840AV -- whether or not any SCSI
    // transfer exists.  Measured across the AV suite: 129,200,000 firings in
    // 1,292 s of guest time, of which 24,604 moved a byte.  One in 5,251.
    if (running)
        scheduler_new_cpu_event(cfg->scheduler, &av_scsi_pump_event, cfg, 0, 0, (uint64_t)AV_SCSI_PUMP_NS);
}

// Arm the pump when the PSC's SCSI channel is touched.
//
// Called on ANY write into that channel's register block, not on the bit that
// starts it: av_psc_dma_ready wants !pause && ENABLED && cnt != 0, a driver may
// assemble those three in any order, and arming on the last one to arrive means
// guessing which that is.
//
// Which makes idempotence the whole job.  An already-running pump is LEFT
// ALONE -- no remove-and-re-add, because that would restart its 10 us phase,
// and this is called several times per transfer setup.  The first version did
// remove-and-re-add and the repeated phase resets moved sprint boundaries
// enough to change CPU/DSP interleaving: suite-av's av-sr-macro row spoke its
// answer for 0.6 s instead of 2.3 s, RMS 528 against the golden's 1184.  The
// pump's phase is an emulation artifact, so nothing may depend on it -- but
// things downstream of sprint boundaries evidently do, and the cheapest way to
// owe them nothing is never to move it.
void av_scsi_pump_arm(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    if (!cfg || !cfg->scheduler)
        return;
    if (has_event(cfg->scheduler, &av_scsi_pump_event))
        return; // already pumping: leave its cadence where it is
    scheduler_new_cpu_event(cfg->scheduler, &av_scsi_pump_event, cfg, 0, 0, (uint64_t)AV_SCSI_PUMP_NS);
}

// PSC bus-master DMA: guest-physical accesses through the bus resolver
// (the sonic memory-hook pattern — the CPU MMU is deliberately not in the
// path).
// SCC chip INT → PSC level-4 SCCA/SCCB bits.  The chip has one INT line;
// the ROM's SccDecode handler reads SCC RR3 to find the channel, so both
// bits track the line.  (Guarded: scc_init fires this before the PSC is
// built.)
static void av_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    av_state_t *st = av_st(cfg);
    if (!st || !st->psc)
        return;
    av_psc_level_source(st->psc, AV_PSC_L4, 1, active);
    av_psc_level_source(st->psc, AV_PSC_L4, 2, active);
}

// ============================================================
// RAM mapping
// ============================================================
// YMCA decodes eight banks.  Each bank sits at its boundary register (in
// MB) with a window of its size code -- 1 MB << (code - 1), code 0 = off --
// and a bank smaller than its window repeats through it.  The ROM sizes
// memory against exactly that: @YMCASplit puts bank n at n x 16 MB with
// 16 MB windows, YMCASizeBanks finds each bank's size where its signature
// wraps, and YMCAMerge packs the banks it can use largest first from 0.  A
// bank whose size is not a power of two has no code and is switched off,
// which is why the totals the board can hold are SIMM populations
// (av_ram_banks), not any multiple of 4 MB.
//
// The flat RAM image holds the banks in that merged order, so once the ROM
// has merged them the decode is the image at 0 -- what the 040 bus
// resolver, DMA and checkpoints address.

bool av_ram_banks(const av_board_desc_t *desc, uint32_t ram, uint32_t size[AV_YMCA_BANK_COUNT]) {
    static const uint32_t simm_mb[] = {32, 16, 8, 4};
    memset(size, 0, sizeof(uint32_t) * AV_YMCA_BANK_COUNT);
    if (ram < desc->ram_onboard)
        return false;
    if (desc->ram_onboard)
        size[0] = desc->ram_onboard;
    uint32_t left = ram - desc->ram_onboard;
    // Largest SIMM that fits, slot by slot; the slots then hold a
    // non-increasing population, which reaches every total any population
    // of these sizes reaches.
    for (int k = 0; k < desc->simm_slots && left; k++) {
        uint32_t mb = 0;
        for (size_t i = 0; i < sizeof(simm_mb) / sizeof(simm_mb[0]); i++)
            if ((simm_mb[i] << 20) <= left) {
                mb = simm_mb[i];
                break;
            }
        if (!mb)
            return false;
        int bank = desc->simm_first_bank + 2 * k;
        if (bank + 1 >= AV_YMCA_BANK_COUNT)
            return false;
        if (mb == 32) { // two 16 MB banks
            size[bank] = size[bank + 1] = 16u << 20;
        } else {
            size[bank] = mb << 20;
        }
        left -= mb << 20;
    }
    return left == 0;
}

// Lay the banks into the flat image in the order YMCAMerge packs them:
// largest first, ties in bank order.
static void av_bank_image(av_state_t *st) {
    uint32_t off = 0;
    for (uint32_t sz = 16u << 20; sz >= 1u << 20; sz >>= 1)
        for (int b = 0; b < AV_YMCA_BANK_COUNT; b++)
            if (st->bank_size[b] == sz) {
                st->bank_image_off[b] = off;
                off += sz;
            }
}

// The registers' power-on layout, what @YMCASplit also programs: bank n at
// n x 16 MB, every window 16 MB (size code 5).
static void av_ymca_split(av_state_t *st) {
    for (int b = 0; b < AV_YMCA_BANK_COUNT; b++) {
        uint32_t boundary = (uint32_t)b * 16, code = 5;
        for (int k = 0; k < AV_YMCA_BDRY_BITS; k++)
            st->ymca_regs[ymca_bank_reg(b, k)] = (uint8_t)((boundary >> k) & 1);
        for (int k = 0; k < AV_YMCA_SIZE_BITS; k++)
            st->ymca_regs[ymca_bank_reg(b, AV_YMCA_BDRY_BITS + k)] = (uint8_t)((code >> k) & 1);
    }
}

// Rebuild the RAM decode from the bank registers.  Not while the ROM
// overlay holds low memory: dropping it maps RAM (av_map_ram).
static void av_ymca_remap(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    if (st->overlay.armed)
        return;
    uint32_t end = AV_YMCA_BANK_COUNT * (16u << 20); // the split layout's reach
    if (st->decode_end > end)
        end = st->decode_end;
    for (uint32_t p = 0; p < (end >> PAGE_SHIFT); p++)
        mac030_clear_page(p);
    st->decode_end = 0;
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    for (int b = 0; b < AV_YMCA_BANK_COUNT; b++) {
        uint32_t boundary = 0, code = 0;
        for (int k = 0; k < AV_YMCA_BDRY_BITS; k++)
            boundary |= (uint32_t)st->ymca_regs[ymca_bank_reg(b, k)] << k;
        for (int k = 0; k < AV_YMCA_SIZE_BITS; k++)
            code |= (uint32_t)st->ymca_regs[ymca_bank_reg(b, AV_YMCA_BDRY_BITS + k)] << k;
        if (!code || !st->bank_size[b])
            continue;
        uint32_t window = 1u << (20 + (code > 5 ? 5 : code) - 1);
        uint32_t base = boundary << 20;
        mac030_map_mirrored(base >> PAGE_SHIFT, window >> PAGE_SHIFT, ram + st->bank_image_off[b],
                            st->bank_size[b] >> PAGE_SHIFT, mac030_fill_page, true);
        if (base + window > st->decode_end)
            st->decode_end = base + window;
    }
}

static void av_map_ram(config_t *cfg) {
    av_ymca_remap(cfg);
}

// ============================================================
// ROM-at-zero overlay (access-triggered)
// ============================================================
// While armed, the ROM aperture ($40800000-$40A00000) is registered as a
// device window: the first access drops the overlay — RAM appears at zero,
// the aperture pages become direct ROM pages — and the triggering access
// itself returns ROM data.  The reset PC ($0000002A from ROM offset 4)
// executes `JMP $40800074` as its very first instruction, so the drop
// happens before any RAM is touched.

// ============================================================
// Memory layout
// ============================================================

static void av_memory_layout(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    const av_board_desc_t *desc = av_board(cfg)->desc;

    // I/O island: the serialized window at $50F00000 plus its non-serialized
    // alias at $50F40000, folded by the $3FFFF mirror mask.
    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, 0x50F00000u, 0x00080000u, "I/O", &st->io_interface, &st->io);

    // CPU-ID register page at $5FFFF000 (the register itself is $5FFFFFFC).
    st->cpuid_interface.read_uint8 = av_cpuid_read8;
    st->cpuid_interface.read_uint16 = av_cpuid_read16;
    st->cpuid_interface.read_uint32 = av_cpuid_read32;
    st->cpuid_interface.write_uint8 = av_cpuid_write8;
    st->cpuid_interface.write_uint16 = av_cpuid_write16;
    st->cpuid_interface.write_uint32 = av_cpuid_write32;
    memory_map_add(cfg->memory_map, 0x5FFFF000u, 0x00001000u, "CPU-ID", &st->cpuid_interface, cfg);

    // The overlay-trigger device for the ROM aperture is registered once;
    // arming/dropping only re-points page entries.
    mac030_rom_overlay_init(&st->overlay, cfg, desc->common.rom_base, desc->common.rom_end, av_map_ram, "AV");
    memory_map_add(cfg->memory_map, desc->common.rom_base, desc->common.rom_end - desc->common.rom_base, "ROM aperture",
                   &st->overlay.iface, &st->overlay);

    mac030_rom_overlay_arm(&av_st(cfg)->overlay);
}

// ============================================================
// VIA1 callbacks (shared by both leaves)
// ============================================================
// Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out, PB5 TIP
// out); the SR shift-out is a Cuda command byte.

void av_via1_output(void *context, uint8_t port, uint8_t value) {
    av_state_t *st = av_st((config_t *)context);
    av_cuda_via1_port_output(st ? st->cuda : NULL, port, value);
}

void av_via1_shift_out(void *context, uint8_t byte) {
    av_state_t *st = av_st((config_t *)context);
    av_cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// ============================================================
// Device construction (shared by both leaves)
// ============================================================

MACHINE_PART_SAVE(av_psc_checkpoint, av_psc_t)
MACHINE_PART_SAVE(av_dsp_checkpoint, av_dsp_t)
MACHINE_PART_SAVE(av_singer_checkpoint, av_singer_t)
MACHINE_PART_SAVE(av_new_age_checkpoint, av_new_age_t)
MACHINE_PART_SAVE(av_mace_checkpoint, av_mace_t)
MACHINE_PART_SAVE(av_civic_checkpoint, av_civic_t)
MACHINE_PART_SAVE(av_vdc_checkpoint, av_vdc_t)

int av_build_devices(config_t *cfg, checkpoint_t *cp) {
    av_state_t *st = av_st(cfg);
    const av_board_desc_t *desc = av_board(cfg)->desc;

    // VIA1 idle input levels.  Port A: PA0/PA1 are the
    // POST CheckLoopBack burn-in probe — held at differing levels so no
    // jumper is detected; PA7 vSCCWrReq idles high (no SCC request).
    via_input(cfg->via1, 0, 0, 1);
    via_input(cfg->via1, 0, 1, 0);
    via_input(cfg->via1, 0, 2, 0);
    via_input(cfg->via1, 0, 5, 0);
    via_input(cfg->via1, 0, 7, 1);
    // Port B: PB3 is Cuda TREQ (active-low, idle high).
    via_input(cfg->via1, 1, 3, 1);
    // CA1 (60 Hz) and the Cuda CB1/CB2 lines idle high -- the VIA's own
    // power-on state, so this does not have to say so.

    // The PSC interrupt controller + DMA engine (VIA2 window, L3-L6,
    // sndPhase, the 7 channels).
    machine_part_begin(cfg, cp, "psc");
    st->psc = av_psc_init(cfg, cp);
    if (!st->psc) {
        LOG(0, "Error: out of memory constructing the PSC");
        return -1;
    }
    machine_part(cfg, cp, "psc", av_psc_checkpoint_part, st->psc);
    av_psc_set_memory_port(st->psc, &dma_mem_port_physical); // the shared physical-memory pair

    // The DSP3210 aux core on the PSC's dspOverRun reset latch, and the
    // Singer sound frame engine that feeds it EXT1 ticks.
    machine_part_begin(cfg, cp, "dsp");
    st->dsp = av_dsp_init(cfg, cp);
    if (!st->dsp) {
        LOG(0, "Error: out of memory constructing the DSP3210");
        return -1;
    }
    machine_part(cfg, cp, "dsp", av_dsp_checkpoint_part, st->dsp);
    av_psc_set_dsp_hook(st->psc, av_dsp_overrun_hook, st->dsp);
    machine_part_begin(cfg, cp, "singer");
    st->singer = av_singer_init(cfg, cp);
    if (!st->singer) {
        LOG(0, "Error: out of memory constructing the Singer codec");
        return -1;
    }
    machine_part(cfg, cp, "singer", av_singer_checkpoint_part, st->singer);

    // ADB device state, serviced through Cuda packets (adb_iop_transact),
    // not the VIA shifter — pass NULL for the VIA (the IIsi/Egret pattern).
    machine_part_begin(cfg, cp, "adb");
    st->adb = adb_init(NULL, cfg->scheduler, cp);
    cfg->adb = st->adb;
    machine_part(cfg, cp, "adb", part_save_adb, st->adb);

    // The behavioral Cuda on VIA1's shift register + PB3/PB4/PB5.
    machine_part_begin(cfg, cp, "cuda");
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, st->adb, cfg->scheduler, cp, /*mode3_clock=*/false);
    if (!st->cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return -1;
    }
    machine_part(cfg, cp, "cuda", part_save_cuda, st->cuda);

    // MACE Ethernet register stub + address PROM (no wire).
    machine_part_begin(cfg, cp, "mace");
    st->mace = av_mace_init(cfg, cp);
    if (!st->mace) {
        LOG(0, "Error: out of memory constructing the MACE Ethernet");
        return -1;
    }
    machine_part(cfg, cp, "mace", av_mace_checkpoint_part, st->mace);

    // CIVIC + Sebastian video (Hi-Res 640x480 monitor, 2 MB VRAM).
    machine_part_begin(cfg, cp, "civic");
    st->civic = av_civic_init(cfg, cp);
    if (!st->civic) {
        LOG(0, "Error: out of memory constructing the CIVIC");
        return -1;
    }
    machine_part(cfg, cp, "civic", av_civic_checkpoint_part, st->civic);

    // The video digitizer (DMSD + VDC + frame engine), reached through
    // Cuda pseudo-command $22 and CIVIC's video-in gates.
    machine_part_begin(cfg, cp, "vdc");
    st->vdc = av_vdc_init(cfg, cp);
    if (!st->vdc) {
        LOG(0, "Error: out of memory constructing the VDC");
        return -1;
    }
    machine_part(cfg, cp, "vdc", av_vdc_checkpoint_part, st->vdc);
    av_cuda_attach_vdc(st->cuda, st->vdc);

    machine_part_images(cfg, cp);

    // The internal SuperDrive (after the image list it resolves its media
    // from) and the New Age FDC that drives it.  No memory map: the PSC
    // island decodes the controller (av_io_ranges), so the shared module
    // only carries the drive and its media.
    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_NEW_AGE, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);
    machine_part_begin(cfg, cp, "new_age");
    st->fdc = av_new_age_init(cfg, cp);
    if (!st->fdc) {
        LOG(0, "Error: out of memory constructing the New Age FDC");
        return -1;
    }
    machine_part(cfg, cp, "new_age", av_new_age_checkpoint_part, st->fdc);

    // SCSI: the bus/target model carries the disks and CD; the 53C96 chip
    // model fronts it through the external-initiator API.
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    machine_part_begin(cfg, cp, "scsi96");
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    machine_part(cfg, cp, "scsi96", part_save_scsi96, st->scsi96);
    scsi_53c96_set_irq_callback(st->scsi96, av_scsi96_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96, cfg->scsi);
    // The HAL polls PSC-VIA2 IFR bit 0 for the chip's DREQ (see psc.c).
    av_psc_set_dreq_query(st->psc, (av_psc_dreq_fn)scsi_53c96_dreq, st->scsi96);
    av_psc_set_scsi_touch_hook(st->psc, av_scsi_pump_arm, cfg);

    // The PSC channel-0 pump (the hardware's DREQ/DACK engine).
    // Registered, not armed: av_scsi_pump_arm() starts it when the guest
    // programs the PSC's SCSI channel, and it stops itself when the channel
    // goes idle.
    scheduler_new_event_type(cfg->scheduler, "av", cfg, "scsi_pump", &av_scsi_pump_event);

    // Bus-side physical resolver for the 040 walker: RAM decoded up to the
    // ROM base, the 2 MB ROM at $40800000.  ram aperture max = $40800000 so
    // RAM-sizing probes above installed memory read $FF, not bus-error.
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint8_t *rom_data = ram_native_pointer(cfg->memory_map, ram_size);
    st->bus_mmu = mmu_init(ram_base, ram_size, desc->common.rom_base, rom_data, cfg->machine->rom_size,
                           desc->common.rom_base, desc->common.rom_end);
    if (!st->bus_mmu) {
        LOG(0, "Error: out of memory constructing the 040 bus MMU");
        return -1;
    }
    memory_map_set_pmmu(cfg->memory_map, st->bus_mmu);
    mmu_attach_mmu040(st->bus_mmu, (mmu040_state_t *)cfg->cpu->mmu);

    setup_images(cfg);

    // Bind the I/O island + CPU-ID + ROM aperture, then arm the overlay.
    av_io_bind(&st->io, cfg, desc);
    av_memory_layout(cfg);

    // VRAM pages + the $50036000 CIVIC alias layer over the flat map.
    av_civic_install_memory(cfg, st->civic);
    return 0;
}

// ============================================================
// Substrate lifecycle
// ============================================================

// The substrate-private part: the overlay flag, YMCA and MUNI.
static void part_save_av_private(void *obj, checkpoint_t *cp) {
    av_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->overlay.armed, sizeof(st->overlay.armed));
    system_write_checkpoint_data(cp, st->ymca_regs, sizeof(st->ymca_regs));
    system_write_checkpoint_data(cp, &st->muni_intcntrl, sizeof(st->muni_intcntrl));
    system_write_checkpoint_data(cp, &st->muni_control, sizeof(st->muni_control));
}

static int av_init(config_t *cfg, checkpoint_t *cp) {
    const av_board_t *board = av_board(cfg);
    av_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;

    // Shared core (memory_map, 68040 CPU from the profile, scheduler) + RTC +
    // SCC + the single VIA (there is no VIA2 chip on this platform).
    mac030_build_core(cfg, &board->desc->common, cp);
    machine_part_irq(cfg, cp);

    mac030_build_lowspeed(cfg, cp, av_scc_irq);

    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", board->via1_output, board->via1_shift_out, av_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    // Exact-rational phi2: the integer divisor above rounds, and on this
    // substrate that rounding is not negligible -- the 840AV lands 0.12% fast, the 660AV 0.27% slow.
    // via_set_exact_clock installs ticks = cycles x 783360/cpu_hz reduced, which is what the PowerPC families already
    // do.
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // The installed RAM as the board's banks; the registers start split.
    if (!av_ram_banks(board->desc, cfg->ram_size, st->bank_size)) {
        LOG(0, "Error: %u MB is no SIMM population of the %s", cfg->ram_size >> 20, cfg->machine->name);
        return -1;
    }
    av_bank_image(st);
    av_ymca_split(st);

    // Machine-specific tail (shared for both AV leaves).
    if (board->build_devices(cfg, cp) != 0)
        return -1;

    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);

    // The substrate's own part.
    bool overlay = true;
    machine_part_begin(cfg, cp, "av");
    if (cp) {
        system_read_checkpoint_data(cp, &overlay, sizeof(overlay));
        system_read_checkpoint_data(cp, st->ymca_regs, sizeof(st->ymca_regs));
        system_read_checkpoint_data(cp, &st->muni_intcntrl, sizeof(st->muni_intcntrl));
        system_read_checkpoint_data(cp, &st->muni_control, sizeof(st->muni_control));
    }
    machine_part(cfg, cp, "av", part_save_av_private, st);
    if (cp) {
        if (!overlay)
            av_set_overlay(cfg, false);
        mmu_invalidate_tlb(st->bus_mmu);
        via_redrive_outputs(cfg->via1);
    }

    mac030_glue_finish(cfg, cp, &st->io);
    return 0;
}

// A power cycle's power-on-only half (machine_profile.h): Cuda stays
// powered, but the host side of its VIA1 handshake went down under it, and
// YMCA's bank registers return to their power-on layout.
static void av_power_on(config_t *cfg) {
    av_cuda_host_power_cycle(av_st(cfg)->cuda);
    av_civic_power_on(av_st(cfg)->civic); // Civic's VRAM goes with main RAM
    av_ymca_split(av_st(cfg)); // the bank registers come up split
}

static void av_bus_reset(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    // Overlay re-arms; the 040's own MMU is reset by cpu_hardware_reset_040.
    mac030_rom_overlay_arm(&st->overlay);
    // The AV's BUS mmu is a board part, so it stays on this side.
    if (st->bus_mmu) {
        st->bus_mmu->enabled = false;
        mmu_invalidate_tlb(st->bus_mmu);
    }
    av_civic_reset(st->civic); // sync off until the ROM re-enables video
    // The floppy CONTROLLER; cfg->floppy (the drive and its media) is reset
    // by the shared chain.
    av_new_age_reset(st->fdc);
    system_reset_common_devices(cfg);
}

static void av_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    av_state_t *st = av_st(cfg);
    if (st) {
        if (st->vdc) {
            av_vdc_delete(st->vdc);
            st->vdc = NULL;
        }
        if (st->civic) {
            av_civic_delete(st->civic);
            st->civic = NULL;
        }
        if (st->mace) {
            av_mace_delete(st->mace);
            st->mace = NULL;
        }
        if (st->scsi96) {
            scsi_53c96_delete(st->scsi96);
            st->scsi96 = NULL;
        }
        if (st->fdc) {
            av_new_age_delete(st->fdc);
            st->fdc = NULL;
        }
        // The drive the controller was bound to (above the shared chain,
        // as on PDM and TNT: only these families build it themselves).
        if (cfg->floppy) {
            floppy_delete(cfg->floppy);
            cfg->floppy = NULL;
        }
        if (st->cuda) {
            av_cuda_delete(st->cuda);
            st->cuda = NULL;
        }
        if (st->singer) {
            av_singer_delete(st->singer);
            st->singer = NULL;
        }
        if (st->dsp) {
            av_dsp_delete(st->dsp);
            st->dsp = NULL;
        }
        if (st->psc) {
            av_psc_delete(st->psc);
            st->psc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
        if (st->bus_mmu) {
            mmu_delete(st->bus_mmu);
            st->bus_mmu = NULL;
        }
    }
    // The config_t-owned devices, in the family-shared canonical order
    // (machine_teardown.h).  Was a byte-identical copy in five families.
    machine_teardown_config_devices(cfg);
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

// VBL tick: VIA1 CA1 pulse (60 Hz reference) + the PSC's own 60.15 Hz
// level-6 source.
static void av_trigger_vbl(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    if (st && st->psc)
        av_psc_tick60(st->psc);
    if (cfg->nubus)
        nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// Primary display: the CIVIC scanout (substrate .display hook).
static struct display *av_display(config_t *cfg) {
    av_state_t *st = av_st(cfg);
    return (st && st->civic) ? av_civic_display(st->civic) : NULL;
}

// CIVIC's built-in video as a display device.  Its monitor strap is fixed at
// the 13" RGB today; the extended-sense walk that would tell the others apart
// is not modelled, so this is the one monitor offered.
static bool civic_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "13in_rgb";
    return true;
}

// Its passive sense code, %110; an unplugged port grounds nothing.
static bool civic_monitor_sense(const char *id, uint8_t *out) {
    if (strcmp(id, "13in_rgb") == 0)
        *out = 6;
    else if (strcmp(id, "none") == 0)
        *out = MACHINE_SENSE_NONE;
    else
        return false;
    return true;
}

// CIVIC's startup modes: slot $9's PRAM record, as each ROM writes it for the
// 13" RGB -- BoardID ($003D Quadra 840AV, $0050 Centris/Quadra 660AV),
// savedMode, the monitor's sResource twice, its sense code -- and the depths
// it honours from a seeded one: 1 to 32 bpp (measured).
#define CIVIC_STARTUP(board)                                                                                           \
    {                                                                                                                  \
        {.monitor = "13in_rgb",                                                                                        \
         .width = 640,                                                                                                 \
         .height = 480,                                                                                                \
         .record = {0x00, (board), 0x80, 0xB1, 0xB1, 0x06, 0, 0},                                                      \
         .modes = {{1, 0x80}, {2, 0x81}, {4, 0x82}, {8, 0x83}, {16, 0x84}, {32, 0x85}}},                               \
        {0},                                                                                                           \
}
static const builtin_startup_t q840av_startup[] = CIVIC_STARTUP(0x3D);
static const builtin_startup_t q660av_startup[] = CIVIC_STARTUP(0x50);

const builtin_video_desc_t av_builtin_video_q840av = {
    .detail = "CIVIC",
    .monitor_at = civic_monitor_at,
    .monitor_sense = civic_monitor_sense,
    .default_monitor = "13in_rgb",
    .startup_slot = 0x9,
    .startup = q840av_startup,
};

const builtin_video_desc_t av_builtin_video_q660av = {
    .detail = "CIVIC",
    .monitor_at = civic_monitor_at,
    .monitor_sense = civic_monitor_sense,
    .default_monitor = "13in_rgb",
    .startup_slot = 0x9,
    .startup = q660av_startup,
};

const machine_substrate_t av_substrate = {
    .init = av_init,
    .bus_reset = av_bus_reset,
    .power_on = av_power_on,
    .teardown = av_teardown,
    .seed = mac_seed_rtc_pram,
    .nubus_slot_irq = av_nubus_slot_irq, // slots C/D/E → PSC SInt bits 3-5
    .trigger_vbl = av_trigger_vbl,
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_attach = system_media_attach_std,
    .media_present = system_media_present_std,
    .media_eject = system_media_eject_std,
    .display = av_display,
};

// Public overlay control for checkpoint restore / tests.
void av_set_overlay(config_t *cfg, bool on) {
    if (on)
        mac030_rom_overlay_arm(&av_st(cfg)->overlay);
    else
        mac030_rom_overlay_drop(&av_st(cfg)->overlay);
}
