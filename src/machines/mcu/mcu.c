// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mcu.c
// The MCU/Orwell family substrate (Quadra 700/900/950) — see mcu.h.
// Implements the family lifecycle plus the pieces unique to this generation:
// the access-triggered ROM-at-zero overlay, the accept-and-log MCU register
// file, and the Q700 I/O island decode table run on the shared mac030 engine.

#include "mcu.h"
#include "appletalk.h"
#include "regfile.h"

#include "mac_host_io.h" // mac_fd_*/mac_input_*
#include "machine_teardown.h" // the shared config_t-owned delete chain
#include "mmu040.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_images.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (attach the 040 walker to the bus resolver)
#include "dafb.h"
#include "debug.h"
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
#include "sonic.h"
#include "via.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

static inline const mcu_board_t *mcu_board(config_t *cfg) {
    return (const mcu_board_t *)cfg->machine->board;
}

static inline mcu_state_t *mcu_st(config_t *cfg) {
    return (mcu_state_t *)cfg->machine_context;
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

// Apply the latched bank start addresses to the page table.
//
// A bank decodes from its start address up to whichever populated bank starts
// next above it, or 64 MB if none does, and mirrors its installed size
// throughout that span.  The mirroring is what lets the boot ROM size a bank:
// it walks down from the top of the window and finds where the image repeats.
static void mcu_map_ram(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);

    for (int b = 0; b < st->bank_count; b++) {
        uint32_t size = st->bank_size[b];
        if (size == 0)
            continue;
        uint32_t start = st->bank_start[b];

        // The span this bank owns ends at the next populated bank above it.
        uint32_t end = start + ORWELL_BANK_WINDOW;
        for (int o = 0; o < st->bank_count; o++)
            if (o != b && st->bank_size[o] && st->bank_start[o] > start && st->bank_start[o] < end)
                end = st->bank_start[o];

        mac030_map_mirrored(start >> PAGE_SHIFT, (end - start) >> PAGE_SHIFT, ram_base + st->bank_image_off[b],
                            size >> PAGE_SHIFT, mac030_fill_page, true);
    }
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

// --- Ethernet MAC-address PROM ($50008000) ---
// The Apple presentation the SONIC driver consumes (SonicEnet.a @GetAddr):
// bytes 0-5 hold the station address with each byte BIT-REVERSED (the
// driver's NormAddr undoes it), and the XOR of all eight bytes must equal
// $FF (checksum probed before the address is trusted).  The address here is
// the locally-administered 02:00:00:09:07:01 → bit-reversed 40 00 00 90 E0
// 80; byte 6 is zero and byte 7 makes the XOR come out to $FF.
static const uint8_t mcu_mac_prom[8] = {0x40, 0x00, 0x00, 0x90, 0xE0, 0x80, 0x00, 0x4F};

static uint8_t mcu_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)cfg;
    return mcu_mac_prom[addr & 7u];
}

static void mcu_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)cfg;
    LOG(2, "MAC PROM write $%X = $%02X ignored (read-only)", addr & 7u, value);
}

// --- SONIC ($5000A000; 16-bit registers on 4-byte spacing) ---
// The register value rides the LOW half of the 32-bit slot (current
// MAME maps it the same way); the engine byte-decomposes wider accesses, so
// reads serve bytes 2-3 of each slot and a write COMMITS when byte 3
// lands (the Quadra driver/tests use 32-bit accesses throughout — SonicEqu.a
// SONIC32).  Bytes 0-1 read as zero and their writes are ignored.

static uint8_t mcu_sonic_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte < 2)
        return 0;
    uint16_t v = sonic_reg_read(st->sonic, off >> 2);
    return (byte == 2) ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static void mcu_sonic_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0xFFFu;
    uint32_t byte = off & 3u;
    if (byte == 2)
        st->sonic_byte2 = value;
    else if (byte == 3)
        sonic_reg_write(st->sonic, off >> 2, (uint16_t)((st->sonic_byte2 << 8) | value));
}

// --- NCR 53C96 ($5000F000; registers on a 16-byte spacing) ---

static uint8_t mcu_scsi_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    return scsi_53c96_read(mcu_st(cfg)->scsi96, (addr & 0xFFu) >> 4);
}

static void mcu_scsi_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    scsi_53c96_write(mcu_st(cfg)->scsi96, (addr & 0xFFu) >> 4, value);
}

// --- SCSI 0 pseudo-DMA aperture ($5000F100): the TurboSCSI payload port.
// The engine byte-decomposes 16-bit accesses, so the byte hooks carry both
// widths in wire order (big-endian high byte first).

static uint8_t mcu_scsi_pdma_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    return scsi_53c96_pdma_read8(mcu_st(cfg)->scsi96);
}

static void mcu_scsi_pdma_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    scsi_53c96_pdma_write8(mcu_st(cfg)->scsi96, value);
}

// --- Second NCR 53C96 (towers; regs at $5000F402 on 16-byte spacing and
// pseudo-DMA at $5000F502 — UniversalTables.a OrwellDecoderTable "2nd
// (external) SCSI96"; the pdma alias matches current MAME).  The
// same (offset >> 4) register decode as bus 0 serves the +2 byte lane.

static uint8_t mcu_scsi_ext_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    return scsi_53c96_read(mcu_st(cfg)->scsi96_ext, (addr & 0xFFu) >> 4);
}

static void mcu_scsi_ext_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    scsi_53c96_write(mcu_st(cfg)->scsi96_ext, (addr & 0xFFu) >> 4, value);
}

static uint8_t mcu_scsi_ext_pdma_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    return scsi_53c96_pdma_read8(mcu_st(cfg)->scsi96_ext);
}

static void mcu_scsi_ext_pdma_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)addr;
    scsi_53c96_pdma_write8(mcu_st(cfg)->scsi96_ext, value);
}

// --- YANCC ($50028000) — the system-bus/NuBus bridge register file.
// The register address is Apple-documented, its width and bits are not:
// same latch-and-log policy as the MCU file, so the ROM's/driver's access
// sequence is recoverable as an RE artifact.  Actual
// NuBus transactions run through the memory map + nubus core directly; the
// write-buffer/error machinery this register controls is not modeled yet.

static uint8_t mcu_yancc_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0x1FFFu;
    uint32_t idx = (off >> 2) % MCU_YANCC_REG_COUNT;
    uint32_t v = st->yancc_regs[idx];
    // Log-once per register, the dafb.c pattern.  The read path used to TEST
    // yancc_touched without ever SETTING it -- only the write path did -- so a
    // register the ROM only reads logged on every single read, forever,
    // defeating the design.
    if (!(st->yancc_touched & (1ull << (idx & 63)))) {
        st->yancc_touched |= 1ull << (idx & 63);
        LOG(2, "YANCC read  $%04X -> $%08X (pc=%08X)", off, v, cpu_get_pc(cfg->cpu));
    }
    return be_lane8(v, off & 3);
}

static void mcu_yancc_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    mcu_state_t *st = mcu_st(cfg);
    uint32_t off = addr & 0x1FFFu;
    uint32_t idx = (off >> 2) % MCU_YANCC_REG_COUNT;
    be_lane8_set(&st->yancc_regs[idx], off & 3, value);
    LOG(2, "YANCC write $%04X = $%08X (pc=%08X)", off, st->yancc_regs[idx], cpu_get_pc(cfg->cpu));
    st->yancc_touched |= 1ull << (idx & 63);
}

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
const mac030_io_range_t mcu_q700_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, MAC030_DEV_VIA2, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x08008, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_prom_read, mcu_prom_write, "mac_prom"},
    {0x0A000, 0x0B100, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_sonic_read, mcu_sonic_write, "sonic"},
    {0x0C000, 0x0E000, MAC030_DEV_SCC, MCU_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x0E000, 0x0F000, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_reg_read, mcu_reg_write, "mcu"},
    {0x0F000, 0x0F100, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_read, mcu_scsi_write, "scsi_53c96"},
    {0x0F100, 0x0F102, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_pdma_read, mcu_scsi_pdma_write, "scsi_pdma"},
    {0x14000, 0x16000, MAC030_DEV_ASC, MCU_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "easc"},
    {0x1E000, 0x20000, MAC030_DEV_FLOPPY, MCU_SWIM_IO_PENALTY, MAC030_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0x28000, 0x2A000, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_yancc_read, mcu_yancc_write, "yancc"},
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
const mac030_io_range_t mcu_q900_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, MAC030_DEV_VIA2, MCU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x08008, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_prom_read, mcu_prom_write, "mac_prom"},
    {0x0A000, 0x0B100, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_sonic_read, mcu_sonic_write, "sonic"},
    {0x0C000, 0x0E000, MAC030_DEV_SCC_IOP, MCU_IOP_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc_iop"},
    {0x0E000, 0x0F000, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_reg_read, mcu_reg_write, "mcu"},
    {0x0F000, 0x0F100, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_read, mcu_scsi_write, "scsi_53c96"},
    {0x0F100, 0x0F102, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_pdma_read, mcu_scsi_pdma_write, "scsi_pdma"},
    {0x0F400, 0x0F500, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_ext_read, mcu_scsi_ext_write, "scsi1_53c96"},
    {0x0F500, 0x0F510, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_scsi_ext_pdma_read, mcu_scsi_ext_pdma_write,
     "scsi1_pdma"},
    {0x14000, 0x16000, MAC030_DEV_ASC, MCU_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "easc"},
    {0x1E000, 0x20000, MAC030_DEV_SWIM_IOP, MCU_IOP_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "swim_iop"},
    {0x28000, 0x2A000, 0, MCU_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, mcu_yancc_read, mcu_yancc_write, "yancc"},
    {0}, // sentinel: end == 0
};

void mcu_io_bind(mac030_io_t *io, config_t *cfg, const mcu_board_desc_t *desc, void *asc, void *floppy) {
    mac030_io_install(io, cfg, &desc->common);
    mac030_io_bind_dev(io, MAC030_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    mac030_io_bind_dev(io, MAC030_DEV_VIA2, cfg->via2, via_get_memory_interface(cfg->via2));
    mac030_io_bind_dev(io, MAC030_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    mac030_io_bind_dev(io, MAC030_DEV_ASC, asc, asc_get_memory_interface((asc_t *)asc));
    mac030_io_bind_dev(io, MAC030_DEV_FLOPPY, floppy, floppy_get_memory_interface((floppy_t *)floppy));
}

// ============================================================
// /SLOTIRQ aggregation
// ============================================================
// The slot/video/Ethernet request lines land on VIA2 PA0-PA6 (active-low)
// and OR into the active-low /SLOTIRQ on VIA2 CA1.  Every source funnels
// through here so the aggregate stays consistent regardless of origin
// (DAFB video = PA6, SONIC = PA0, NuBus slots A-E = PA1-PA5).

void mcu_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    mcu_state_t *st = mcu_st(cfg);
    if (pa_bit < 0 || pa_bit > 6)
        return;
    uint8_t bit = (uint8_t)(1u << pa_bit);
    st->slot_pa_mask = active ? (st->slot_pa_mask | bit) : (st->slot_pa_mask & (uint8_t)~bit);
    via_input(cfg->via2, /*port A*/ 0, pa_bit, active ? 0 : 1); // active-low line
    via_input_c(cfg->via2, /*CA1*/ 0, 0, st->slot_pa_mask ? 0 : 1); // /SLOTIRQ = OR of sources
}

// ============================================================
// DAFB construction (shared by every MCU board)
// ============================================================

// DAFB video interrupt -> VIA2 PA6 (active-low) through the family /SLOTIRQ
// aggregate on CA1, alongside the NuBus slot sources.
static void mcu_dafb_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    mcu_slot_irq_source(cfg, 6, active);
}

int mcu_build_dafb(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;

    st->dafb = dafb_init(desc->dafb_vram_size, cp);
    if (!st->dafb) {
        LOG(0, "Error: out of memory constructing the DAFB");
        return -1;
    }
    dafb_attach_scheduler(st->dafb, cfg->scheduler);
    dafb_set_irq_callback(st->dafb, mcu_dafb_irq, cfg);
    dafb_attach_objects(st->dafb); // machine.video{,.framebuffer}

    // Consume unconditionally so a staged sense never leaks into a later
    // boot, but only APPLY it on a cold build: on a restore, dafb_init()
    // has already read the saved sense out of the checkpoint, and this
    // call would otherwise overwrite it with the default.
    uint8_t staged_sense = dafb_sense_for_build(cfg); // default 6 = 13" RGB
    if (!cp)
        dafb_set_monitor_sense(st->dafb, staged_sense);

    dafb_set_version(st->dafb, desc->dafb_version); // 3 on the Q950 (DAFB 3)
    dafb_set_ac842a(st->dafb, desc->has_ac842a); // AC842a x555 on the Q950

    // TurboSCSI DRQ observation: channel 0 = internal.  The towers add a
    // second 53C96 for the external bus on channel 1 -- the ONE genuine
    // per-machine difference in this function (the Q700 has "one NCR 53C96
    // shared by internal and external connectors").
    dafb_set_scsi_drq_query(st->dafb, 0, (dafb_drq_query_fn)scsi_53c96_dreq, st->scsi96);
    if (st->scsi96_ext)
        dafb_set_scsi_drq_query(st->dafb, 1, (dafb_drq_query_fn)scsi_53c96_dreq, st->scsi96_ext);
    return 0;
}

// substrate.nubus_slot_irq: a NuBus card's /NMRQ maps to VIA2 PA(slot-9)
// (slot $A→PA1 .. $E→PA5).  Slot 9 is the built-in video and
// never arrives here — DAFB drives PA6 directly through the aggregate.
static void mcu_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    int pa_bit = slot - 0x9;
    if (pa_bit < 1 || pa_bit > 5)
        return;
    mcu_slot_irq_source(cfg, pa_bit, active);
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
    const mcu_board_desc_t *desc = mcu_board(cfg)->desc;

    // Work out the physical bank arrangement before anything maps RAM.
    mcu_bank_layout(cfg);

    // I/O island at $50000000 (256 KiB block; the published map mirrors it
    // through $53FFFFFF, current RE through $50FFFFFF — we register the
    // Apple-documented extent and let the mirror mask fold accesses).
    mac030_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->mem_map, 0x50000000u, 0x04000000u, "I/O", &st->io_interface, &st->io);

    // DAFB registers at $F9800000; VRAM pages direct at $F9000000.
    memory_map_add(cfg->mem_map, DAFB_REG_BASE, DAFB_REG_APERTURE, "DAFB regs",
                   (memory_interface_t *)dafb_reg_interface(st->dafb), st->dafb);
    uint8_t *vram = dafb_vram(st->dafb);
    uint32_t vram_pages = dafb_vram_size(st->dafb) >> PAGE_SHIFT;
    uint32_t vram_start = DAFB_VRAM_BASE >> PAGE_SHIFT;
    for (uint32_t i = 0; i < vram_pages && (int)(vram_start + i) < g_page_count; i++)
        mac030_fill_page(vram_start + i, vram + (i << PAGE_SHIFT), true);
    // Register VRAM with the bus resolver so 040 table walks / TT matches
    // reaching physical $F9xxxxxx resolve to the buffer.
    memory_map_host_region(cfg->mem_map, "dafb_vram", vram, DAFB_VRAM_BASE, dafb_vram_size(st->dafb),
                           /*writable*/ true);

    // The overlay-trigger device for the ROM aperture is registered once;
    // arming/dropping only re-points page entries.
    mac030_rom_overlay_init(&st->overlay, cfg, desc->common.rom_base, desc->common.rom_end, mcu_map_ram, "MCU");
    memory_map_add(cfg->mem_map, desc->common.rom_base, desc->common.rom_end - desc->common.rom_base, "ROM aperture",
                   &st->overlay.iface, &st->overlay);

    mac030_rom_overlay_arm(&mcu_st(cfg)->overlay);
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

    // Shared core (mem_map, 68040 CPU from the profile, scheduler) + RTC +
    // SCC + the two VIAs.
    mac030_build_core(cfg, cp);
    if (cp)
        system_read_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));

    // Towers intercept the SCC chip INT (OR with the SCC IOP host INT);
    // the Q700 routes it straight to the level-4 source, which is the
    // family default the NULL branch selects.
    mac030_build_lowspeed(cfg, cp, board->scc_irq);

    // Derived from the CPU clock (see the same note in mdu.c): the towers run
    // 25 MHz (Q700/Q900) and 33 MHz (Q950), so the previous hardcoded 20/21 —
    // inherited from the 16 MHz IIcx — ran both machines' VIA timers fast.
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 = via_init(NULL, cfg->scheduler, via_ff, "via1", board->via1_output, board->via1_shift_out,
                         mac030_glue_via1_irq, cfg, cp);
    cfg->via2 = via_init(NULL, cfg->scheduler, via_ff, "via2", board->via2_output, NULL, mac030_glue_via2_irq, cfg, cp);
    // Exact-rational phi2: the integer divisor above rounds, and on this
    // substrate that rounding is not negligible -- the Q950 lands 1.04% slow, the Q700/Q900 0.27%.  via_set_exact_clock
    // installs ticks = cycles x 783360/cpu_hz reduced, which is what the
    // PowerPC families already do.
    via_set_exact_clock(cfg->via1, cfg->machine->freq);
    via_set_exact_clock(cfg->via2, cfg->machine->freq);

    // Machine-specific tail: straps, ADB, EASC, SWIM, DAFB, bus resolver,
    // memory layout, checkpoint restore.
    if (board->build_devices(cfg, cp) != 0)
        return -1;

    // NuBus: seat the declared slot cards; their windows layer
    // over the bus-error range, and slot IRQs route through the substrate's
    // nubus_slot_irq into the VIA2 PA aggregate.
    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);
    // The substrate tail was read by mcu_restore_private inside build_devices
    // above, so the card block that mcu_checkpoint_save wrote after it reads
    // back here.
    if (cp)
        nubus_checkpoint_restore(cfg->nubus, cp);
    // Project the cards' host regions (VRAM, declaration ROMs) into the
    // page table so CPU accesses resolve with the MMU off; the bus
    // resolver serves the 040 walker when it's on.  No Mode-24 aliases —
    // this family's ROM and System are 32-bit clean, and a low alias would
    // shadow RAM at $00s00000 on large-memory configurations.
    mmu_host_regions_fill_pages(st->bus_mmu, mac030_fill_page, /*mode24_alias*/ false);

    mac030_glue_finish(cfg, cp, &st->io);
    return 0;
}

static void mcu_bus_reset(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    // Overlay re-arms (VIA1 pulls it high), DAFB registers clear, and the
    // NuBus/SCSI fan-out with them.  The 040's own MMU is reset by
    // cpu_hardware_reset_040 -- it is inside the CPU, not on the net.
    mac030_rom_overlay_arm(&st->overlay);
    if (st->dafb)
        dafb_reset(st->dafb);
    // The MCU's BUS mmu is a board part, not the CPU's, so it stays here.
    if (st->bus_mmu) {
        st->bus_mmu->enabled = false;
        mmu_invalidate_tlb(st->bus_mmu);
    }
    system_reset_common_devices(cfg);
}

static void mcu_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
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
        if (st->scsi_ext) {
            scsi_delete(st->scsi_ext);
            st->scsi_ext = NULL;
        }
        if (st->sonic) {
            sonic_delete(st->sonic);
            st->sonic = NULL;
        }
        if (st->scsi96) {
            scsi_53c96_delete(st->scsi96);
            st->scsi96 = NULL;
        }
        if (st->dafb) {
            dafb_delete(st->dafb);
            st->dafb = NULL;
        }
        if (st->bus_mmu) {
            mmu_delete(st->bus_mmu);
            st->bus_mmu = NULL;
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
    // The config_t-owned devices, in the family-shared canonical order
    // (machine_teardown.h).  Was a byte-identical copy in five families.
    machine_teardown_config_devices(cfg);
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

static void mcu_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    machine_checkpoint_save_core(cfg, cp);
    adb_checkpoint(st->adb, cp);
    mac_checkpoint_save_images(cfg, cp);
    // Device order mirrors the build_devices construction order exactly
    // (q900_build_devices is the superset; the tower entries are guarded so
    // the same save set serves the Q700's subset stream).
    if (cfg->scsi)
        scsi_checkpoint(cfg->scsi, cp);
    scsi_53c96_checkpoint(st->scsi96, cp);
    if (st->scsi_ext)
        scsi_checkpoint(st->scsi_ext, cp);
    if (st->scsi96_ext)
        scsi_53c96_checkpoint(st->scsi96_ext, cp);
    sonic_checkpoint(st->sonic, cp);
    asc_checkpoint(st->asc, cp);
    floppy_checkpoint(st->floppy, cp);
    if (st->caboose)
        egret_checkpoint(st->caboose, cp);
    if (st->scc_iop)
        iop_checkpoint(st->scc_iop, cp);
    if (st->swim_iop)
        iop_checkpoint(st->swim_iop, cp);
    dafb_checkpoint(st->dafb, cp);
    // Substrate-private state: overlay flag + Orwell config/bank starts +
    // the YANCC register file +
    // the /SLOTIRQ aggregate mask + the in-flight SONIC write latch + the
    // tower wire-OR IRQ masks (zero on the Q700).
    system_write_checkpoint_data(cp, &st->overlay.armed, sizeof(st->overlay.armed));
    system_write_checkpoint_data(cp, &st->orwell_cfg, sizeof(st->orwell_cfg));
    system_write_checkpoint_data(cp, st->bank_start, sizeof(st->bank_start));
    system_write_checkpoint_data(cp, st->yancc_regs, sizeof(st->yancc_regs));
    // The log-once bitmap travels with the registers it guards; without it a
    // restore re-logs every YANCC register the guest had already touched.
    system_write_checkpoint_data(cp, &st->yancc_touched, sizeof(st->yancc_touched));
    system_write_checkpoint_data(cp, &st->slot_pa_mask, sizeof(st->slot_pa_mask));
    system_write_checkpoint_data(cp, &st->sonic_byte2, sizeof(st->sonic_byte2));
    system_write_checkpoint_data(cp, &st->scc_irq_or, sizeof(st->scc_irq_or));
    system_write_checkpoint_data(cp, &st->scsi_irq_or, sizeof(st->scsi_irq_or));
    // Card-side display state (VRAM, palette, active mode) — last before the
    // block below, so a machine that restores with fewer cards than it saved
    // short-reads here without shifting anything that follows (mdu.c:190,
    // pdm.c:537 use the same position).
    nubus_checkpoint_save(cfg->nubus, cp);
}

// Restore the substrate-private checkpoint tail (mirrors the tail writes in
// mcu_checkpoint_save) and re-drive the derived interrupt lines.  Called at
// the end of each board's build_devices on the restore path, after the
// memory layout armed the overlay and parked the VIA input lines at idle.
void mcu_apply_via1_model_sense(config_t *cfg, const mcu_board_desc_t *desc) {
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via1, 0, bit, (desc->via1_pa_model >> bit) & 1);
    via_input(cfg->via1, 0, 0, 1); // PA0 diagnostic strap high (see mcu.h)
}

void mcu_restore_private(config_t *cfg, checkpoint_t *cp) {
    mcu_state_t *st = mcu_st(cfg);
    bool overlay = false;
    system_read_checkpoint_data(cp, &overlay, sizeof(overlay));
    system_read_checkpoint_data(cp, &st->orwell_cfg, sizeof(st->orwell_cfg));
    system_read_checkpoint_data(cp, st->bank_start, sizeof(st->bank_start));
    system_read_checkpoint_data(cp, st->yancc_regs, sizeof(st->yancc_regs));
    system_read_checkpoint_data(cp, &st->yancc_touched, sizeof(st->yancc_touched)); // mirrors the save above
    system_read_checkpoint_data(cp, &st->slot_pa_mask, sizeof(st->slot_pa_mask));
    system_read_checkpoint_data(cp, &st->sonic_byte2, sizeof(st->sonic_byte2));
    system_read_checkpoint_data(cp, &st->scc_irq_or, sizeof(st->scc_irq_or));
    system_read_checkpoint_data(cp, &st->scsi_irq_or, sizeof(st->scsi_irq_or));
    // The layout armed the overlay; a post-overlay snapshot drops it.
    if (!overlay)
        mcu_set_overlay(cfg, false);
    // Re-drive the /SLOTIRQ PA lines + the CA1 aggregate from the restored
    // mask (sources 0-6; build parked them at the inactive level).
    for (int bit = 0; bit <= 6; bit++)
        via_input(cfg->via2, 0, bit, (st->slot_pa_mask >> bit) & 1 ? 0 : 1);
    via_input_c(cfg->via2, 0, 0, st->slot_pa_mask ? 0 : 1);
    // Tower wire-ORs: VIA2 CB2 (dual 53C96) and the level-4 SCC source
    // (chip INT | SCC IOP host INT) resume at their save-time levels.
    if (st->scsi96_ext)
        via_input_c(cfg->via2, 1, 1, st->scsi_irq_or ? 0 : 1);
    if (st->scc_iop)
        mac030_glue_update_ipl(cfg, MAC030_GLUE_IRQ_SCC, st->scc_irq_or != 0);
    mmu_invalidate_tlb(st->bus_mmu);
    via_redrive_outputs(cfg->via1);
}

// VBL tick: VIA1 CA1 pulse (the 60.15 Hz VIA2-PB7 chain, functionally;
// video interrupts come from the Swatch's programmed timing in dafb.c —
// Traps 9/10 apply to the Swatch IRQs, not this line).
static void mcu_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// Primary display: the DAFB scanout (substrate .display hook).
static struct display *mcu_display(config_t *cfg) {
    mcu_state_t *st = mcu_st(cfg);
    return (st && st->dafb) ? dafb_display(st->dafb) : NULL;
}

const machine_substrate_t mcu_substrate = {
    .init = mcu_init,
    .bus_reset = mcu_bus_reset,
    .teardown = mcu_teardown,
    .checkpoint_save = mcu_checkpoint_save,
    .trigger_vbl = mcu_trigger_vbl,
    .nubus_slot_irq = mcu_nubus_slot_irq, // slots → VIA2 PA1-PA5 + /SLOTIRQ aggregate
    .fd_insert = mac_fd_insert,
    .fd_present = mac_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_detach = system_media_detach_std,
    .media_attach = system_media_attach_std,
    .media_present = system_media_present_std,
    .media_eject = system_media_eject_std,
    .display = mcu_display,
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
        mac030_rom_overlay_arm(&mcu_st(cfg)->overlay);
    else
        mac030_rom_overlay_drop(&mcu_st(cfg)->overlay);
}
