// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// hmc.c
// HMC ("High-speed Memory Controller") — the PDM memory controller.  One
// software-visible register: a 35-bit configuration accessed bit-serially
// through the AMIC-decoded window at $50F40000 (byte write to +8 resets the
// bit pointer; each byte access to +0 moves one bit, LSB first).  Plus the
// software-visible consequences this model honors:
//
//   - RAM bank windows with probe semantics: undersized banks alias (wrap)
//     within their decode window, empty banks read nothing back, and the
//     6100's SIMM banks MOVE when the SIMM_BANK_SIZE code is written
//     (contiguous after motherboard RAM); 7100/8100 banks are fixed.
//   - the wait-state bit (bit 8) that HWInit's bus-ratio measurement
//     toggles: while set, physical page 0 takes a slow path that charges
//     extra bus cycles per access, so the measured delta yields the real
//     machine's CPU:bus ratio (proposal §5.2).
//   - the machine-ID register at $5FFFFFFC (byte-readable; a 32-bit read
//     must NOT show the $A55A signature — the ROM's long-probe has to fail).
//
// Everything else (DRAM timing fields, refresh divider, L2 controls) is
// store-and-readback: no L2 is modeled, which is fully self-consistent with
// every ROM path (the cache-size sense bits 0-1 read 0 = "no cache SIMM",
// so POST skips the L2 test with the "Lisa" marker).
//
// Register truth: Apple, "Power Macintosh Computers" Developer Note (1994)
// and the shipping 1994-03 ROM's hardware-init sequence.

#include "pdm.h"

#include "log.h"
#include "ppc.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("hmc");

// Config bit assignments (bit N = Nth bit shifted, LSB first; masks on the
// low 32-bit word)
#define HMC_TIMING    0x0000FFFCu // bits 2-15: DRAM timing field
#define HMC_L2_DIAG   0x02000000u // bit 25: cache test window redirect
#define HMC_SIMM_SIZE 0x60000000u // bits 29-30: SIMM bank size code (6100)
#define HMC_MB_4MB    0x80000000u // bit 31: motherboard bank is 4 MB

// ============================================================
// Bank inventory: carve cfg->ram_size into motherboard + SIMM banks
// ============================================================
// 8 MB soldered on every model; the remainder splits into banks of the
// sizes the ROM accepts ({2, 8, 32} MB — 128 MB configs are beyond every
// profile's ram_max), largest first, bank n >= bank n+1 as the ROM
// requires.  Host storage is contiguous: motherboard at offset 0, banks
// following in order.

static void pdm_hmc_wait_state(config_t *cfg, bool on);

void pdm_hmc_init(config_t *cfg) {
    pdm_hmc_t *h = &pdm_st(cfg)->hmc;
    const pdm_board_desc_t *bd = pdm_board(cfg);

    memset(h, 0, sizeof(*h));
    h->wait_state = true; // power-on: DRAM timing field zero = slow
    uint32_t ram = cfg->ram_size;
    h->mb_size = ram < 0x800000u ? ram : 0x800000u;
    uint32_t rest = ram - h->mb_size;
    uint32_t off = h->mb_size;
    static const uint32_t sizes[] = {0x2000000u, 0x800000u, 0x200000u}; // 32/8/2 MB
    for (int b = 0; b < bd->bank_count && rest > 0; b++) {
        for (unsigned s = 0; s < 3; s++) {
            if (rest >= sizes[s]) {
                h->bank_size[b] = sizes[s];
                h->bank_host_off[b] = off;
                off += sizes[s];
                rest -= sizes[s];
                break;
            }
        }
    }
    if (rest)
        LOG(0, "RAM size $%X does not decompose into PDM banks; $%X unmapped", ram, rest);
}

// ============================================================
// RAM decode
// ============================================================

// Map `size` bytes of RAM (host offset `host_off`) into the window at
// [base, base+window), wrapping so undersized banks alias — the sizing
// probe depends on finding the aliases.
static void map_bank_window(config_t *cfg, uint32_t base, uint32_t window, uint32_t host_off, uint32_t size) {
    if (!size)
        return;
    uint8_t *host = ram_native_pointer(cfg->mem_map, 0) + host_off;
    uint32_t pages = window >> PAGE_SHIFT;
    uint32_t first = base >> PAGE_SHIFT;
    for (uint32_t p = 0; p < pages; p++)
        pdm_fill_page(first + p, host + ((p << PAGE_SHIFT) % size), true);
}

// Rebuild the whole DRAM decode ($00000000-$3FFFFFFF) from the current
// config.  Called at init (power-on state) and whenever a config write
// changes the placement-relevant fields.
void pdm_hmc_remap(config_t *cfg) {
    pdm_hmc_t *h = &pdm_st(cfg)->hmc;
    const pdm_board_desc_t *bd = pdm_board(cfg);

    // Clear the DRAM region: unmapped pages read 0 (outside the bus-error
    // range), so empty windows fail the probe's signature compare — the
    // "empty banks must not echo" requirement.
    for (uint32_t p = 0; p < (0x40000000u >> PAGE_SHIFT); p++)
        pdm_clear_page(p);

    // Motherboard bank at 0: 8 MB window, 4 MB parts alias twice.
    uint32_t mb_decode = (h->cfg_lo & HMC_MB_4MB) ? 0x400000u : 0x800000u;
    uint32_t mb = h->mb_size < mb_decode ? h->mb_size : mb_decode;
    map_bank_window(cfg, 0, 0x800000u, 0, mb);

    if (bd->bank_layout == PDM_BANKS_FIXED) {
        // 7100/8100: bank n in its fixed 32 MB window at $01000000 +
        // n*$04000000, independent of the config.
        for (int b = 0; b < bd->bank_count; b++)
            map_bank_window(cfg, 0x01000000u + (uint32_t)b * 0x04000000u, 0x02000000u, h->bank_host_off[b],
                            h->bank_size[b]);
    } else {
        // 6100: placement is a pure function of the SIMM_BANK_SIZE code.
        uint32_t code = (h->cfg_lo & HMC_SIMM_SIZE) >> 29;
        h->active_code = code;
        if (code == 0) {
            // Reset map ("128 MB banks"): bank 1 at $10000000, bank 2 at
            // $08000000, each with a 128 MB decode.
            map_bank_window(cfg, 0x10000000u, 0x08000000u, h->bank_host_off[0], h->bank_size[0]);
            map_bank_window(cfg, 0x08000000u, 0x08000000u, h->bank_host_off[1], h->bank_size[1]);
        } else {
            // Sized: banks contiguous after motherboard RAM, each with a
            // bank-size decode window.
            uint32_t bank_decode = 0x200000u << (2 * (code - 1)); // 2/8/32 MB
            uint32_t base = 0x800000u; // motherboard top (8 MB model)
            for (int b = 0; b < 2; b++) {
                map_bank_window(cfg, base, bank_decode, h->bank_host_off[b], h->bank_size[b]);
                base += bank_decode;
            }
        }
    }

    // The remap filled page 0 direct; reapply the slow path if active.
    if (h->wait_state)
        pdm_hmc_wait_state(cfg, true);

    // The physical map moved under the MMU's caches (identity SoA is
    // rebuilt above; translated fills/TLBs re-resolve lazily).
    if (cfg->ppc)
        ppc_mmu_invalidate_all(cfg->ppc);
}

// ============================================================
// Wait-state slow path for the bus-ratio measurement (§5.2)
// ============================================================
// While the DRAM timing field (bits 2-15) is all zero — the power-on state
// and the ROM's $00090000 test pattern — physical page 0 is remapped to
// this device interface: accesses still hit RAM, but each one charges the
// board's wait-state penalty through the phantom-instruction channel.
// HWInit times its 1024x4 `lwz` loop once in this state and once with a
// timing bit set ($00090100), and the delta over its add-loop timebase is
// exactly the per-load penalty — the CPU:bus ratio.  (The ROM's subtract
// order proves the zero-field run is the SLOW one; every production
// timing value leaves the field non-zero, so configured machines never
// see the penalty.)

static uint8_t *pdm_wait_ram(void *ctx) {
    return ram_native_pointer(((config_t *)ctx)->mem_map, 0);
}

static uint8_t wait_read8(void *ctx, uint32_t offset) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    return pdm_wait_ram(ctx)[offset];
}

static uint16_t wait_read16(void *ctx, uint32_t offset) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    return LOAD_BE16(pdm_wait_ram(ctx) + offset);
}

static uint32_t wait_read32(void *ctx, uint32_t offset) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    return LOAD_BE32(pdm_wait_ram(ctx) + offset);
}

static void wait_write8(void *ctx, uint32_t offset, uint8_t v) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    pdm_wait_ram(ctx)[offset] = v;
}

static void wait_write16(void *ctx, uint32_t offset, uint16_t v) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    STORE_BE16(pdm_wait_ram(ctx) + offset, v);
}

static void wait_write32(void *ctx, uint32_t offset, uint32_t v) {
    memory_io_penalty(pdm_board((config_t *)ctx)->wait_state_penalty);
    STORE_BE32(pdm_wait_ram(ctx) + offset, v);
}

// Apply the wait-state mapping of physical page 0.
static void pdm_hmc_wait_state(config_t *cfg, bool on) {
    pdm_state_t *st = pdm_st(cfg);
    if (on) {
        st->wait_interface.read_uint8 = wait_read8;
        st->wait_interface.read_uint16 = wait_read16;
        st->wait_interface.read_uint32 = wait_read32;
        st->wait_interface.write_uint8 = wait_write8;
        st->wait_interface.write_uint16 = wait_write16;
        st->wait_interface.write_uint32 = wait_write32;
        pdm_clear_page(0);
        g_page_table[0].dev = &st->wait_interface;
        g_page_table[0].dev_context = cfg;
        g_page_table[0].base_addr = 0;
    } else {
        pdm_fill_page(0, ram_native_pointer(cfg->mem_map, 0), true);
    }
}

// ============================================================
// The serial config port
// ============================================================

// Read/write one bit at the current pointer; the pointer auto-increments.
// Per-bit immediate commit: nothing in the ROM distinguishes immediate vs
// latch-on-35th, and its failure path does read-modify-write of the whole
// register, so a partial write must leave the untouched bits intact.
static void hmc_shift_in(config_t *cfg, uint32_t bit) {
    pdm_hmc_t *h = &pdm_st(cfg)->hmc;
    uint32_t n = h->bit_ptr;
    if (n >= 35)
        return; // extra accesses past the register fall off the end
    uint32_t old_lo = h->cfg_lo;
    bool old_wait = h->wait_state;
    if (n < 32)
        h->cfg_lo = (h->cfg_lo & ~(1u << n)) | (bit << n);
    else
        h->cfg_hi = (h->cfg_hi & ~(1u << (n - 32))) | (bit << (n - 32));
    h->bit_ptr = n + 1;

    // Derived consequences, applied only when the relevant field changed
    if ((old_lo ^ h->cfg_lo) & (HMC_SIMM_SIZE | HMC_MB_4MB)) {
        LOG(1, "config remap: lo=$%08X hi=$%X", h->cfg_lo, h->cfg_hi);
        pdm_hmc_remap(cfg);
    }
    h->wait_state = (h->cfg_lo & HMC_TIMING) == 0;
    if (h->wait_state != old_wait) {
        LOG(2, "wait state %d (timing field $%04X)", h->wait_state, h->cfg_lo & HMC_TIMING);
        pdm_hmc_wait_state(cfg, h->wait_state);
    }
}

static uint32_t hmc_shift_out(config_t *cfg) {
    pdm_hmc_t *h = &pdm_st(cfg)->hmc;
    uint32_t n = h->bit_ptr;
    if (n >= 35)
        return 0;
    h->bit_ptr = n + 1;
    // Bits 0-1 read back the cache-SIMM size-sense pins: no L2 modeled, so
    // both read 0 ("no cache SIMM") regardless of what was written.
    if (n < 2)
        return 0;
    if (n < 32)
        return (h->cfg_lo >> n) & 1u;
    return (h->cfg_hi >> (n - 32)) & 1u;
}

uint8_t pdm_hmc_read(config_t *cfg, uint32_t offset) {
    if ((offset & 0xF) == 0)
        return (uint8_t)hmc_shift_out(cfg);
    return 0;
}

void pdm_hmc_write(config_t *cfg, uint32_t offset, uint8_t value) {
    pdm_hmc_t *h = &pdm_st(cfg)->hmc;
    if (offset & 0x8) {
        h->bit_ptr = 0; // any byte write to +8 resets the pointer
        LOG(3, "bit pointer reset");
    } else {
        hmc_shift_in(cfg, value & 1u);
    }
}

// ============================================================
// Machine-ID register page ($5FFFF000; the register is $5FFFFFFC)
// ============================================================
// Byte reads deliver $A55A30xx; wider reads must NOT show the signature
// (the ROM's 32-bit probe has to fail before it falls back to bytes), so a
// 32-bit read returns only the low half.  Writes are ignored.

uint8_t pdm_id_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= 0xFFCu) {
        uint32_t id = 0xA55A0000u | pdm_board(cfg)->machine_id;
        return (uint8_t)(id >> (8 * (3 - (offset & 3))));
    }
    return 0;
}

uint32_t pdm_id_read32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (offset >= 0xFFCu)
        return pdm_board(cfg)->machine_id; // no $A55A signature on long reads
    return 0;
}
