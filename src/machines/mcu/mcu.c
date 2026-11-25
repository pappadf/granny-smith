// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mcu.c
// The MCU/Orwell family substrate (Quadra 700/900/950) — see mcu.h.
// Implements the family lifecycle plus the pieces unique to this generation:
// the access-triggered ROM-at-zero overlay, the accept-and-log MCU register
// file, and the Q700 I/O island decode table run on the shared board_io engine.

#include "mcu.h"
#include "appletalk.h"
#include "config_seed.h"
#include "regfile.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "machine_teardown.h" // the shared config_t-owned delete chain
#include "mmu040.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (attach the 040 walker to the bus resolver)
#include "dafb.h"
#include "debug.h"
#include "dma_mem.h" // dma_mem_port_physical (SONIC bus-master DMA)
#include "egret.h" // tower Caboose = Egret-protocol engine
#include "floppy.h"
#include "image.h"
#include "iop.h" // tower SCC/SWIM Apple PIC/IOPs (IIfx-compatible host aperture)
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
#include "slot_irq.h"
#include "sonic.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

static inline const mcu_board_t *mcu_board(config_t *cfg) {
    return (const mcu_board_t *)cfg->machine->board;
}

// ============================================================
// Accept-and-log handler windows (MCU / SONIC / MAC PROM / SCSI / YANCC)
// ============================================================
// Register semantics in these blocks are not publicly documented, so writes
// latch and read back and every first touch is logged: the boot ROM's access
// sequence becomes an RE artifact.

// --- MCU/Orwell register file ($5000E000) ---

// Decompose the installed RAM into physical banks.
//
// A bank is four equal SIMMs, so a populated bank is 4, 16 or 64 MB from the
// 1/4/16 MB parts Apple documented — plus 32 MB from the 8 MB SIMMs that were
// never in the launch note but work in practice, which is exactly what makes
// the Q700's 36 MB and the towers' larger totals reachable.  The Q700 also has
// 4 MB soldered, forming a fixed bank A.
//
// Fill the largest legal bank first, so a total decomposes the way the board
// would really be populated, and never use more banks than the board decodes.
// Anything that will not decompose is presented as a single bank and left for
// the ROM to judge.
static void mcu_bank_layout(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    static const uint32_t simm_bank[] = {0x04000000u, 0x02000000u, 0x01000000u, 0x00400000u}; // 64/32/16/4 MB
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;
    uint32_t onboard = desc->ram_onboard_size;
    int max_banks = desc->ram_bank_count ? desc->ram_bank_count : ORWELL_MAX_BANKS;
    uint32_t left = cfg->ram_size;
    uint32_t off = 0;

    st->bank_count = 0;
    for (int i = 0; i < ORWELL_MAX_BANKS; i++) {
        st->bank_size[i] = 0;
        st->bank_image_off[i] = 0;
    }

    if (onboard && left >= onboard) {
        st->bank_size[0] = onboard;
        st->bank_image_off[0] = 0;
        st->bank_count = 1;
        left -= onboard;
        off = onboard;
    }

    while (left > 0 && st->bank_count < max_banks) {
        uint32_t take = 0;
        for (size_t i = 0; i < sizeof(simm_bank) / sizeof(simm_bank[0]); i++)
            if (simm_bank[i] <= left) {
                take = simm_bank[i];
                break;
            }
        if (take == 0)
            break; // remainder is not a legal bank — see the fallback below
        st->bank_size[st->bank_count] = take;
        st->bank_image_off[st->bank_count] = off;
        st->bank_count++;
        off += take;
        left -= take;
    }

    if (left > 0) {
        // Not a shipping SIMM arrangement.  Present it all as one bank rather
        // than silently dropping the remainder.
        st->bank_size[0] = cfg->ram_size;
        st->bank_image_off[0] = 0;
        st->bank_count = 1;
        for (int i = 1; i < ORWELL_MAX_BANKS; i++)
            st->bank_size[i] = 0;
    }

    // Power-up split: bank A at 0, the rest on their 64 MB boundaries. The ROM
    // reprograms these to the same values before sizing (SizeMem.a
    // @OrwellSplit) and then merges them contiguous once the sizes are known.
    for (int i = 0; i < ORWELL_MAX_BANKS; i++)
        st->bank_start[i] = (uint32_t)i * ORWELL_BANK_WINDOW;
    st->orwell_cfg =
        ((uint64_t)0x10) | ((uint64_t)0x20 << ORWELL_BANK_BITS) | ((uint64_t)0x30 << (2 * ORWELL_BANK_BITS));

    for (int i = 0; i < st->bank_count; i++)
        LOG(2, "Orwell bank %c: %u MB at image offset $%08X", 'A' + i, st->bank_size[i] >> 20, st->bank_image_off[i]);
}

// How far up the Orwell can place RAM: bank D's six-bit field (4 MB units)
// plus its 64 MB window.
#define MCU_RAM_REACH (((1u << ORWELL_BANK_BITS) - 1) * ORWELL_BANK_UNIT + ORWELL_BANK_WINDOW)

// Apply the latched bank start addresses to the page table.
//
// A bank decodes from its start address up to whichever populated bank starts
// next above it, or 64 MB if none does, and mirrors its installed size
// throughout that span.  The mirroring is what lets the boot ROM size a bank:
// it walks down from the top of the window and finds where the image repeats.
static void mcu_map_ram(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    m68k_board_bank_t banks[ORWELL_MAX_BANKS];
    for (int b = 0; b < ORWELL_MAX_BANKS; b++)
        banks[b] =
            (m68k_board_bank_t){st->bank_start[b], b < st->bank_count ? st->bank_size[b] : 0, st->bank_image_off[b]};
    m68k_board_map_banks(cfg, banks, ORWELL_MAX_BANKS, ORWELL_BANK_WINDOW, 0, &st->ram_extent, st->q.bus_mmu);
}

// Recompute bank starts from the staged config register and re-map.  Called
// when the ROM pokes OrLoadBanks ($A0).
static void mcu_orwell_latch_banks(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    st->bank_start[0] = 0; // bank A is not programmable
    for (int b = 1; b < ORWELL_MAX_BANKS; b++) {
        uint32_t field = (uint32_t)((st->orwell_cfg >> ((b - 1) * ORWELL_BANK_BITS)) & ((1u << ORWELL_BANK_BITS) - 1));
        st->bank_start[b] = field * ORWELL_BANK_UNIT;
    }
    LOG(2, "Orwell latch banks: A=$%08X B=$%08X C=$%08X D=$%08X (pc=%08X)", st->bank_start[0], st->bank_start[1],
        st->bank_start[2], st->bank_start[3], cpu_get_pc(cfg->cpu));
    mcu_map_ram(cfg);
}

static uint8_t mcu_reg_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    if (off >= ORWELL_STATUS_BASE)
        return 0; // parity status / error latches — no parity hardware modelled
    if (off >= ORWELL_LATCH_BASE)
        return 0; // latch addresses are write-strobes
    uint32_t bit = off >> 2;
    if (bit >= ORWELL_CFG_BITS)
        return 0;
    // Only bit 0 carries data; it reads back in the low bit of every byte lane
    // the caller happens to touch, and callers assemble it one bit at a time.
    return (off & 3) == 3 ? (uint8_t)((st->orwell_cfg >> bit) & 1) : 0;
}

static void mcu_reg_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;

    if (off >= ORWELL_LATCH_BASE && off < ORWELL_STATUS_BASE) {
        // A write to a latch address commits the staged config bits it owns.
        if ((off & ~3u) == ORWELL_LATCH_BANKS)
            mcu_orwell_latch_banks(cfg);
        else
            LOG(2, "Orwell latch $%03X (pc=%08X)", off, cpu_get_pc(cfg->cpu));
        return;
    }
    if (off >= ORWELL_STATUS_BASE)
        return;

    // Config bit N at longword offset N*4; only bit 0 of the datum is wired.
    // The engine decomposes wider accesses into bytes, so the bit arrives in
    // the lowest byte lane of the longword.
    if ((off & 3) != 3)
        return;
    uint32_t bit = off >> 2;
    if (bit >= ORWELL_CFG_BITS)
        return;
    st->orwell_cfg = (st->orwell_cfg & ~(1ull << bit)) | ((uint64_t)(value & 1) << bit);
}

// --- Ethernet MAC-address PROM ($50008000; quadra040_prom_read) ---
// The address is 08:00:07:09:07:01, in Apple's own OUI: MacTest Pro's
// "Ethernet ROM" test insists on 08:00:07 (bit-reversed 10 00 E0) and fails
// anything else, the locally administered 02:00:00 included.  Bit-reversed:
// 10 00 E0 90 E0 80; byte 6 is zero and byte 7 makes the XOR come out to
// $FF.  The eight bytes repeat across the PROM's $2000 window (mcu.md §5):
// the same test reads sixteen bytes and checks each half's XOR, which an
// 8-byte window floating $FF above it failed.
static const uint8_t mcu_mac_prom[8] = {0x10, 0x00, 0xE0, 0x90, 0xE0, 0x80, 0x00, 0xFF};

// --- SONIC ($5000A000) is quadra040_sonic_read/write. ---

// --- NCR 53C96 ($5000F000; registers on a 16-byte spacing) and its
// pseudo-DMA aperture ($5000F100, the TurboSCSI payload port) are device rows
// (BOARD_DEV_SCSI96, BOARD_IO_STRIDE_16 / BOARD_IO_FIXED).  The engine
// byte-decomposes 16-bit pDMA accesses, so the byte port carries both widths
// in wire order (big-endian high byte first).
//
// Second NCR 53C96 (towers; regs at $5000F402 on 16-byte spacing and
// pseudo-DMA at $5000F502 — UniversalTables.a OrwellDecoderTable "2nd
// (external) SCSI96"; the pdma alias matches current MAME): rows on
// BOARD_DEV_SCSI96_EXT.  The same 16-byte register stride serves the +2
// byte lane.

// --- YANCC ($50028000) — the system-bus/NuBus bridge register file
// (yancc.h, through quadra040_yancc_*).  Latch-and-readback under a log; its
// write-buffer error interrupt requests level 7.

// ============================================================
// Q700 I/O island decode ($50000000, 256 KiB, mirror $3FFFF)
// ============================================================
// Direct low-speed I/O (no IOPs): VIA1/VIA2, direct SCC, direct SWIM, EASC,
// plus the handler windows above.  Penalties follow the MDU values until the
// JDB/Relayer per-device wait-state classes are measured.

#define MCU_VIA_IO_PENALTY  16
#define MCU_SCC_IO_PENALTY  2
#define MCU_ASC_IO_PENALTY  2
#define MCU_SWIM_IO_PENALTY 5 // current MAME charges 5 CPU cycles
// The handler-row chips (MAC PROM, SONIC, the MCU's own registers, the 53C96
// and its pseudo-DMA aperture, YANCC) sit on the same island as the SCC and
// EASC and pay the same turnaround.
#define MCU_IO_PENALTY 2

//   base     end      device            penalty          xform            rd wr  rd_fn/wr_fn      name
const board_io_range_t mcu_q700_io_ranges[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, MCU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, BOARD_DEV_VIA2, MCU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x0A000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_prom_read, quadra040_prom_write, "mac_prom"},
    {0x0A000, 0x0B100, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_sonic_read, quadra040_sonic_write, "sonic"},
    {0x0C000, 0x0E000, BOARD_DEV_SCC, MCU_SCC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x0E000, 0x0F000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, mcu_reg_read, mcu_reg_write, "mcu"},
    {0x0F000, 0x0F100, BOARD_DEV_SCSI96, MCU_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0, NULL, NULL, "scsi_53c96"},
    {0x0F100, 0x0F102, BOARD_DEV_SCSI96, MCU_IO_PENALTY, BOARD_IO_FIXED, SCSI_53C96_PDMA_SUB, SCSI_53C96_PDMA_SUB, NULL,
     NULL, "scsi_pdma"},
    {0x14000, 0x16000, BOARD_DEV_ASC, MCU_ASC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "easc"},
    {0x1E000, 0x20000, BOARD_DEV_FLOPPY, MCU_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0x28000, 0x2A000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_yancc_read, quadra040_yancc_write, "yancc",
     .peek_fn = quadra040_yancc_peek},
    {0}, // sentinel: end == 0
};

// ============================================================
// Q900/Q950 tower I/O island decode (UniversalTables.a
// OrwellDecoderTable — Eclipse memory map)
// ============================================================
// Deltas vs the Q700: the SCC and SWIM windows route to the two Apple
// PIC/IOP host apertures (register layout identical to the IIfx PIC —
// iopRamAddrH $00 / L $02 / iopStatCtl $04 / iopRamData $08 / bypass $20,
// HardwarePrivateEqu.a), and a second 53C96 serves the external SCSI bus.

#define MCU_IOP_IO_PENALTY 2 // same class as the IIfx PIC apertures

//   base     end      device            penalty          xform            rd wr  rd_fn/wr_fn      name
const board_io_range_t mcu_q900_io_ranges[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, MCU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, BOARD_DEV_VIA2, MCU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x0A000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_prom_read, quadra040_prom_write, "mac_prom"},
    {0x0A000, 0x0B100, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_sonic_read, quadra040_sonic_write, "sonic"},
    {0x0C000, 0x0E000, BOARD_DEV_SCC_IOP, MCU_IOP_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc_iop"},
    {0x0E000, 0x0F000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, mcu_reg_read, mcu_reg_write, "mcu"},
    {0x0F000, 0x0F100, BOARD_DEV_SCSI96, MCU_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0, NULL, NULL, "scsi_53c96"},
    {0x0F100, 0x0F102, BOARD_DEV_SCSI96, MCU_IO_PENALTY, BOARD_IO_FIXED, SCSI_53C96_PDMA_SUB, SCSI_53C96_PDMA_SUB, NULL,
     NULL, "scsi_pdma"},
    {0x0F400, 0x0F500, BOARD_DEV_SCSI96_EXT, MCU_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0, NULL, NULL, "scsi1_53c96"},
    {0x0F500, 0x0F510, BOARD_DEV_SCSI96_EXT, MCU_IO_PENALTY, BOARD_IO_FIXED, SCSI_53C96_PDMA_SUB, SCSI_53C96_PDMA_SUB,
     NULL, NULL, "scsi1_pdma"},
    {0x14000, 0x16000, BOARD_DEV_ASC, MCU_ASC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "easc"},
    {0x1E000, 0x20000, BOARD_DEV_SWIM_IOP, MCU_IOP_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "swim_iop"},
    {0x28000, 0x2A000, 0, MCU_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_yancc_read, quadra040_yancc_write, "yancc",
     .peek_fn = quadra040_yancc_peek},
    {0}, // sentinel: end == 0
};

void mcu_io_bind(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    quadra040_io_bind(cfg);
    if (st->scsi96_ext) // towers only
        board_io_bind_dev(&st->q.io, BOARD_DEV_SCSI96_EXT, st->scsi96_ext, scsi_53c96_get_memory_interface());
}

// ============================================================
// DAFB construction (shared by every MCU board)
// ============================================================

int mcu_build_dafb(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;
    // The version / AC842a facts come from the descriptor on every board
    // (DAFB 343S0128-01 is "Q700 and Q900; version values 0, 1, or 2"; only
    // the Q950 is DAFB II, version 3).  TurboSCSI DRQ observation is wired:
    // channel 0 the internal 53C96, channel 1 the towers' external one.
    const quadra040_video_t video = {
        .vram_size = desc->dafb_vram_size,
        .version = desc->dafb_version,
        .ac842a = desc->has_ac842a,
        .turbo_scsi = true,
    };
    return quadra040_build_dafb(cfg, cp, &video, st->scsi96_ext);
}

// ============================================================
// ROM-at-zero overlay (access-triggered)
// ============================================================
// While armed, the ROM aperture ($40000000-$4FFFFFFF) is registered as a
// device window: the first access drops the overlay — RAM appears at zero,
// the aperture pages become direct ROM mirrors — and the triggering access
// itself returns ROM data.  This matches the MCU's documented behavior
// without trapping every access after the drop.

// ============================================================
// Memory layout
// ============================================================

static void mcu_memory_layout_init(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);

    // Work out the physical bank arrangement before anything maps RAM.
    mcu_bank_layout(cfg);
    // Nothing tracked yet: the first remap clears the banks' whole reach.
    st->ram_extent = MCU_RAM_REACH;

    // I/O island at $50000000, DAFB registers at $F9800000 and VRAM at
    // $F9000000, then the armed ROM-aperture trip-wire.
    quadra040_map_island(cfg);
    quadra040_map_video(cfg, "DAFB regs", NULL, NULL);
    quadra040_map_overlay(cfg, mcu_map_ram, "MCU");
}

// ============================================================
// Substrate lifecycle
// ============================================================

static int mcu_init(config_t *cfg, checkpoint_t *cp) {
    const mcu_board_t *board = mcu_board(cfg);
    mcu_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    st->q.mac_prom = mcu_mac_prom;
    yancc_init(&st->q.yancc, "YANCC", quadra040_yancc_irq, cfg);

    // Shared core + RTC + SCC + the two VIAs.  Towers intercept the SCC chip
    // INT (OR with the SCC IOP host INT); the Q700 routes it straight to the
    // level-4 source, which is the family default the NULL selects.
    quadra040_build_core(cfg, cp, &board->desc->common, board->scc_irq, board->via1_output, board->via1_shift_out,
                         board->via2_output);

    // Machine-specific tail: straps, ADB, EASC, SWIM, DAFB, bus resolver,
    // memory layout, checkpoint restore.
    if (board->build_devices(cfg, cp) != 0)
        return -1;

    quadra040_build_finish(cfg, cp);
    return 0;
}

static void mcu_bus_reset(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    // Overlay re-arms (VIA1 pulls it high), DAFB registers clear, the bus
    // MMU goes off and the 53C96 chips reset (scsi96_ext is NULL on the
    // Q700).
    quadra040_bus_reset(cfg, /*reset_video*/ true);
    st->ram_extent = MCU_RAM_REACH; // the ROM at zero: the next remap clears the whole reach
    scsi_53c96_reset(st->scsi96_ext);
    // The YANCC is on the NuBus /RESET: its register file clears, and an
    // error request pending at reset no longer holds level 7.  The Orwell
    // keeps its bank configuration (the RAM decode survives a RESET
    // instruction; the ROM reprograms it as it sizes).
    yancc_reset(&st->q.yancc);
    system_reset_common_devices(cfg);
}

static void mcu_teardown(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    if (st) {
        // Tower devices first (Q900/Q950; all NULL on the Q700).
        if (st->caboose) {
            egret_delete(st->caboose);
            st->caboose = NULL;
        }
        if (st->scc_iop) {
            iop_delete(st->scc_iop);
            st->scc_iop = NULL;
        }
        if (st->swim_iop) {
            iop_delete(st->swim_iop);
            st->swim_iop = NULL;
        }
        if (st->scsi96_ext) {
            scsi_53c96_delete(st->scsi96_ext);
            st->scsi96_ext = NULL;
        }
        if (cfg->scsi2) {
            scsi_delete(cfg->scsi2);
            cfg->scsi2 = NULL;
        }
        quadra040_teardown(&st->q);
    }
    // The config_t-owned devices, in the family-shared canonical order
    // (machine_teardown.h).  Was a byte-identical copy in five families.
    machine_teardown_config_devices(cfg);
}

// The substrate-private part: overlay flag + Orwell config/bank starts + the
// YANCC register file + the /SLOTIRQ aggregate mask + the in-flight SONIC
// write latch + the tower wire-OR IRQ masks (zero on the Q700).
//
// The overlay flag leads the part; it is restored into a local and applied
// through mcu_set_overlay, so it stays out of the field list.  The YANCC
// log-once bitmap travels with the registers it guards; without it a
// restore re-logs every YANCC register the guest had already touched.
#define MCU_PRIVATE_CP_FIELDS(OP, cp, st)                                                                              \
    CP_FIELD(OP, cp, (st)->orwell_cfg);                                                                                \
    CP_ARRAY(OP, cp, (st)->bank_start);                                                                                \
    CP_ARRAY(OP, cp, (st)->q.yancc.regs);                                                                              \
    CP_FIELD(OP, cp, (st)->q.yancc.touched);                                                                           \
    CP_FIELD(OP, cp, (st)->q.slot_pa_mask);                                                                            \
    CP_FIELD(OP, cp, (st)->q.sonic_byte2);                                                                             \
    CP_FIELD(OP, cp, (st)->scc_irq_or);                                                                                \
    CP_FIELD(OP, cp, (st)->scsi_irq_or)

// Saves the substrate-private part.
static void part_save_mcu_private(void *obj, checkpoint_t *cp) {
    mcu_state_t *st = obj;
    CP_FIELD(system_write_checkpoint_data, cp, st->q.overlay.armed);
    MCU_PRIVATE_CP_FIELDS(system_write_checkpoint_data, cp, st);
}

// The substrate-private part, read on a restore -- then the derived
// interrupt lines re-driven.  At the end of each board's build_devices,
// after the memory layout armed the overlay and parked the VIA input lines
// at idle.
static void mcu_restore_private(config_t *cfg, checkpoint_t *cp);

void mcu_private_part(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    machine_part_begin(cfg, cp, "mcu");
    if (cp)
        mcu_restore_private(cfg, cp);
    machine_part(cfg, cp, "mcu", part_save_mcu_private, st);
}

static void mcu_restore_private(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    bool overlay = false;
    CP_FIELD(system_read_checkpoint_data, cp, overlay);
    MCU_PRIVATE_CP_FIELDS(system_read_checkpoint_data, cp, st);
    // The layout armed the overlay; a post-overlay snapshot drops it.
    if (!overlay)
        mcu_set_overlay(cfg, false);
    // Tower wire-ORs: VIA2 CB2 (dual 53C96) and the level-4 SCC source
    // (chip INT | SCC IOP host INT) resume at their save-time levels.
    if (st->scsi96_ext)
        via_input_c(cfg->via2, 1, 1, st->scsi_irq_or ? 0 : 1);
    if (st->scc_iop)
        m68k_board_update_ipl(cfg, M68K_BOARD_IRQ_SCC, st->scc_irq_or != 0);
    // The /SLOTIRQ lines, a pending YANCC error, the bus TLB, VIA1's outputs.
    quadra040_restore_lines(cfg);
}

// Power cycle: the DAFB's VRAM goes with main RAM (system_machine_power_cycle).
static void mcu_power_on(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    if (st)
        dafb_power_on(st->q.dafb);
}

// === Built-in video as a display device ======================================
// The DAFB's monitors are quadra040's table (quadra040_monitor_*): every one
// the Q700/Q900 ROM was measured with, or (the Q950, measured on the passive
// codes only) the passive ones.

// The DAFB's startup modes: slot $9's PRAM record, as each ROM writes it for
// the monitor -- BoardID ($0033 Quadra 700, $002B Quadra 900, $0036 Quadra
// 950), savedMode, the monitor's sResource twice, its sense code and a family
// byte -- and the depths it honours from a seeded one (measured: the Quadra
// 700/900 ROM takes 32 bpp at code $84 on the monitors its VRAM holds it
// for; the Quadra 950's, 16 bpp at $84 and 32 at $85).
#define DAFB_ROW(board, mon, w, h, b3, sense, b6, ...)                                                                 \
    {                                                                                                                  \
        .monitor = (mon), .width = (w), .height = (h), .record = {0x00, (board), 0x80, (b3), (b3), (sense), (b6), 0},  \
        .modes = {                                                                                                     \
            __VA_ARGS__                                                                                                \
        }                                                                                                              \
    }
#define DAFB_TO8                                                                                                       \
    {1, 0x80}, {2, 0x81}, {4, 0x82}, {                                                                                 \
        8, 0x83                                                                                                        \
    }
#define Q700_STARTUP(board)                                                                                            \
    {                                                                                                                  \
        DAFB_ROW(board, "13in_rgb", 640, 480, 0xC9, 0x06, 0x01, DAFB_TO8, {32, 0x84}),                                 \
        DAFB_ROW(board, "12in_rgb", 512, 384, 0xC3, 0x02, 0x00, DAFB_TO8, {32, 0x84}),                                 \
        DAFB_ROW(board, "15in_portrait", 640, 870, 0xC1, 0x01, 0x01, DAFB_TO8),                                        \
        DAFB_ROW(board, "15in_portrait_color", 640, 870, 0xCD, 0x05, 0x01, DAFB_TO8),                                  \
        DAFB_ROW(board, "16in_rgb", 832, 624, 0xC7, 0x2D, 0x01, DAFB_TO8, {32, 0x84}),                                 \
        DAFB_ROW(board, "21in_rgb", 1152, 870, 0xCF, 0x00, 0x01, DAFB_TO8),                                            \
        DAFB_ROW(board, "21in_mono", 1152, 870, 0xC5, 0x03, 0x01, DAFB_TO8),                                           \
        {0},                                                                                                           \
    }
static const builtin_startup_t q700_startup[] = Q700_STARTUP(0x33);
static const builtin_startup_t q900_startup[] = Q700_STARTUP(0x2B);
static const builtin_startup_t q950_startup[] = {
    DAFB_ROW(0x36, "13in_rgb", 640, 480, 0xE3, 0x06, 0x17, DAFB_TO8, {16, 0x84}, {32, 0x85}),
    DAFB_ROW(0x36, "12in_rgb", 512, 384, 0xEC, 0x02, 0x16, DAFB_TO8, {16, 0x84}, {32, 0x85}),
    DAFB_ROW(0x36, "15in_portrait", 640, 870, 0xC1, 0x01, 0x07, DAFB_TO8),
    DAFB_ROW(0x36, "15in_portrait_color", 640, 870, 0xED, 0x05, 0x17, DAFB_TO8, {16, 0x84}),
    DAFB_ROW(0x36, "21in_rgb", 1152, 870, 0xEF, 0x00, 0x17, DAFB_TO8, {16, 0x84}),
    DAFB_ROW(0x36, "21in_mono", 1152, 870, 0xC5, 0x03, 0x07, DAFB_TO8),
    {0},
};

// The DAFB as a display device: the monitor table, the strap convention and
// slot $9's startup records.
#define MCU_BUILTIN_VIDEO(monitor_at_, startup_)                                                                       \
    {                                                                                                                  \
        .detail = "DAFB",                                                                                              \
        .monitor_at = (monitor_at_),                                                                                   \
        .monitor_sense = quadra040_monitor_sense,                                                                      \
        .default_monitor = "13in_rgb",                                                                                 \
        .indexed_sense = true,                                                                                         \
        .startup_slot = 0x9,                                                                                           \
        .startup = (startup_),                                                                                         \
    }

const builtin_video_desc_t mcu_builtin_video_q700 = MCU_BUILTIN_VIDEO(quadra040_monitor_at, q700_startup);
const builtin_video_desc_t mcu_builtin_video_q900 = MCU_BUILTIN_VIDEO(quadra040_monitor_at, q900_startup);
// The Quadra 950's ROM is not measured beyond the passive codes, so it offers
// those monitors.
const builtin_video_desc_t mcu_builtin_video_q950 = MCU_BUILTIN_VIDEO(quadra040_monitor_passive_at, q950_startup);

const machine_substrate_t mcu_substrate = {
    .init = mcu_init,
    .bus_reset = mcu_bus_reset,
    .power_on = mcu_power_on,
    .teardown = mcu_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = quadra040_trigger_vbl,
    .nubus_slot_irq = quadra040_nubus_slot_irq, // slots → VIA2 PA1-PA5 + /SLOTIRQ aggregate
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = quadra040_display,
};

// Shared layout entry for the machine files (called from build_devices once
// the DAFB and bus resolver exist).
void mcu_memory_layout(config_t *cfg) {
    mcu_memory_layout_init(cfg);
}

// Public overlay control for checkpoint restore: layout leaves the overlay
// armed; a restore of a post-overlay state drops it again.
void mcu_set_overlay(config_t *cfg, bool on) {
    if (on)
        m68k_board_overlay_arm(&mcu_st(cfg)->q.overlay);
    else
        m68k_board_overlay_drop(&mcu_st(cfg)->q.overlay);
}
