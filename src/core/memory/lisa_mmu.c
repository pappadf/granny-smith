// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_mmu.c
// Apple Lisa custom segment MMU. See lisa_mmu.h and docs/lisa.md §4-7.
//
// Translation model (verified against the rev-H boot ROM source,
// "Lisa Boot ROM RM248.{E,K}.TEXT"):
//
//  * 24-bit logical address = segment(23-17) | page(16-9) | byte(8-0).
//  * Each segment has a 12-bit SOR (physical page-number origin) and a 12-bit
//    SLR (bits 11-8 = access/space code, bits 7-0 = length in pages, two's
//    complement) per context.  4 contexts x 128 segments.
//  * START (setup) mode is set at power-on.  While START is set, logical bit 14
//    switches: bit14=0 -> special-I/O directly (MMU bypassed: ROM if bit15=0,
//    descriptor RAM if bit15=1); bit14=1 -> normal segment translation.  The
//    boot ROM programs the descriptor RAM under START (bit14=0 writes to
//    $xx8000/$xx8008), then strobes SETUP off ($00FCE012) to enter "map land".
//  * Descriptor-RAM access uses the RAW SEG1/SEG2 latch context (NO supervisor
//    override) — proven by the ROM's CONCHK context test.  Normal translation
//    forces context 0 in supervisor mode.
//  * SETUP strobe polarity per the ROM equates: $00FCE010 = SETUP ON (START),
//    $00FCE012 = SETUP OFF.  (docs/lisa.md §6.1 lists these reversed — the ROM
//    source is authoritative.)

#include "lisa_mmu.h"

#include "cpu.h"
#include "memory.h"
#include "scheduler.h"
#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lisa video timing (docs/lisa.md §8): the state machine scans 379 lines of 720
// pixels at the 5.09375 MHz CPU clock, ~60 Hz vertical.  The Status Register
// vertical-retrace bit (bit 2) is held set for ~90 µs after retrace begins.
#define LISA_FRAME_CYCLES   84896u // 5,093,750 Hz / 60 Hz ≈ one displayed frame
#define LISA_RETRACE_CYCLES 458u // ~90 µs vertical-retrace window

// SLR access/space codes (bits 11-8).
#define ACC_MEM_RO_STK 0x4 // 0100 main memory, read-only, stack
#define ACC_MEM_RO     0x5 // 0101 main memory, read-only
#define ACC_MEM_RW_STK 0x6 // 0110 main memory, read/write, stack
#define ACC_MEM_RW     0x7 // 0111 main memory, read/write
#define ACC_IO         0x9 // 1001 I/O space
#define ACC_INVALID    0xC // 1100 page invalid
#define ACC_SPECIAL    0xF // 1111 special-I/O (ROM + MMU registers)

// Routing decision for a resolved access.
typedef enum {
    L_FAULT = 0, // limit / invalid / read-only violation → bus error
    L_RAM, // main memory: .phys is the 21-bit physical address
    L_IO, // I/O space:   .phys is the physical I/O address
    L_ROM, // special-I/O ROM: .phys is the ROM byte offset
    L_MMUREG, // special-I/O descriptor RAM: .seg/.ctx/.sor select the register
} lisa_route_t;

typedef struct {
    lisa_route_t route;
    uint32_t phys; // RAM/IO physical address, or ROM offset
    int seg; // MMU register segment (0-127)
    int ctx; // MMU register context (0-3)
    bool reg_is_sor; // MMU register: SOR (true) vs SLR (false)
} lisa_resolved_t;

// One registered physical-I/O device range.
typedef struct {
    uint32_t base; // physical I/O base
    uint32_t size; // span in bytes
    memory_interface_t *iface;
    void *dev;
} lisa_io_dev_t;

#define LISA_MAX_IO_DEVS 12

struct lisa_mmu {
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t ram_min; // physical base of installed RAM (Lisa memory boards sit high)
    uint32_t ram_max; // physical end (exclusive); RAM occupies [ram_min, ram_max)
    uint8_t *rom;
    uint32_t rom_size; // 16 KB

    uint16_t sor[4][128]; // 12-bit origin (physical page number) per context
    uint16_t slr[4][128]; // 12-bit limit/access per context

    bool start; // START / SETUP latch (set at power-on)
    uint8_t seg1; // context selector bit 1
    uint8_t seg2; // context selector bit 2

    uint8_t vidlatch; // Video Address Latch ($00E800): A15-A20 of framebuffer
    bool vtir_enabled; // vertical-retrace interrupt enable (VTMSK)
    bool sfmsk; // soft memory-error detect enable
    bool hdmsk; // hard memory-error detect enable
    bool vbl_active; // legacy: forced retrace bit (still set by trigger_vbl; unused by status read)
    uint8_t status_toggle; // unused (retrace bit is cycle-derived); kept for checkpoint layout
    struct scheduler *sched; // clock source for the cycle-accurate retrace bit
    bool vertical; // vertical-retrace latch: 1 while in retrace (Status Register bit 2 reads 0)
    uint64_t last_retrace_frame; // frame index of the last retrace-window rising edge
    uint32_t serial_ctr; // serial-number PROM bit-stream read counter (RDSERN)

    // Parity-circuitry test (PARTST) support.
    bool wwp_on; // write-wrong-parity mode (DG2ON/DG2OFF strobes)
    bool parity_detect; // parity-error detection enabled (PARON/PAROFF)
    uint32_t bad_par_gran; // 32-byte granule written with bad parity (~0 = none)
    uint32_t mealtch; // Memory Error Address latch (physical addr >> 5)
    void (*nmi_cb)(void *ctx, bool active); // assert/clear the level-7 parity NMI
    void *nmi_ctx;
    void (*vbl_ack_cb)(void *ctx); // OS read of the Status Register acks the latched VBL IRQ
    void *vbl_ack_ctx;

    lisa_io_dev_t io[LISA_MAX_IO_DEVS];
    int io_count;
};

lisa_mmu_t *g_lisa_mmu = NULL;

// === Lifecycle =============================================================

lisa_mmu_t *lisa_mmu_init(uint8_t *ram, uint32_t ram_size, uint8_t *rom, uint32_t rom_size, bool ram_high,
                          checkpoint_t *cp) {
    lisa_mmu_t *m = (lisa_mmu_t *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->ram = ram;
    m->ram_size = ram_size;
    // Lisa memory boards populate the address space from 512 KB upward, not from
    // physical 0 (verified vs LisaEm: 512 KB → [$80000,$100000), 1 MB →
    // [$80000,$180000), 2 MB → [0,$200000)).  The boot ROM's MEMSIZ scans up from 0
    // and reports this base as MINMEM ($2A4); the OS lays out real memory (and the
    // kernel stack at the top of it) relative to that base.  With RAM wrongly based
    // at 0, the OS placed the kernel stack on a non-existent high page.
    // Lisa memory boards populate [$80000 .. min($80000+installed, $200000)) — verified
    // against LisaEm (boots LOS 3.1 to the Install menu with RAM [$80000,$200000)).
    // The Macintosh XL (ram_high=false) keeps RAM at 0: MacWorks' framebuffer and
    // boot path live in low memory.  GSRAMMIN forces the high layout regardless (a
    // debug override; harmless on the Lisa, which already defaults high).
    m->ram_min = (ram_high || getenv("GSRAMMIN")) ? 0x80000u : 0u;
    m->ram_max = m->ram_min + ram_size;
    if (m->ram_max > 0x200000u)
        m->ram_max = 0x200000u;
    m->rom = rom;
    m->rom_size = rom_size;
    // Power-on: START set, descriptor RAM cleared. Cleared descriptors read
    // back as 0, which the ROM's context test relies on (contexts 1-3 must NOT
    // match context 0's programmed values).
    m->start = true;
    m->vidlatch = 0;
    m->bad_par_gran = 0xFFFFFFFFu; // no bad-parity location
    g_lisa_mmu = m;
    if (cp)
        lisa_mmu_checkpoint(m, cp); // restore (same field order as save)
    return m;
}

void lisa_mmu_delete(lisa_mmu_t *m) {
    if (!m)
        return;
    if (g_lisa_mmu == m)
        g_lisa_mmu = NULL;
    free(m);
}

void lisa_mmu_checkpoint(lisa_mmu_t *m, checkpoint_t *cp) {
    // Symmetric no-op for now: save and restore both contribute zero bytes, so
    // the surrounding subsystems' fixed checkpoint ordering is preserved.
    // Full descriptor-RAM + latch save/restore lands in Step 9 (R7).
    (void)m;
    (void)cp;
}

void lisa_mmu_map_io(lisa_mmu_t *m, uint32_t phys_base, uint32_t size, memory_interface_t *iface, void *dev) {
    if (!m || m->io_count >= LISA_MAX_IO_DEVS)
        return;
    m->io[m->io_count].base = phys_base;
    m->io[m->io_count].size = size;
    m->io[m->io_count].iface = iface;
    m->io[m->io_count].dev = dev;
    m->io_count++;
}

uint32_t lisa_mmu_video_base(const lisa_mmu_t *m) {
    if (!m)
        return 0;
    // Latch holds A15-A20; framebuffer physical base = latch << 15.  Return the
    // offset into the installed-RAM buffer (RAM is based at ram_min, not 0).
    uint32_t base = (uint32_t)m->vidlatch << 15;
    if (base >= m->ram_min && base < m->ram_max)
        return base - m->ram_min;
    if (m->ram_size) // fallback: keep within the buffer
        return base & (m->ram_size - 1);
    return base;
}

bool lisa_mmu_vbl_enabled(const lisa_mmu_t *m) {
    return m && m->vtir_enabled;
}

void lisa_mmu_set_vbl_active(lisa_mmu_t *m, bool active) {
    if (m)
        m->vbl_active = active;
}

void lisa_mmu_set_vbl_ack(lisa_mmu_t *m, void (*cb)(void *), void *ctx) {
    if (m) {
        m->vbl_ack_cb = cb;
        m->vbl_ack_ctx = ctx;
    }
}

void lisa_mmu_set_clock(lisa_mmu_t *m, struct scheduler *sched) {
    if (m)
        m->sched = sched;
}

// === Control-block strobes & registers ($00E000-$00FFFF physical) ==========

// Strobe a control latch.  Triggered by ANY access (read or write) to a strobe
// address in $00E000-$00E01E; the data value is ignored.
static void lisa_strobe(lisa_mmu_t *m, uint32_t phys) {
    switch (phys & 0x1E) {
    case 0x08:
        m->seg1 = 0;
        break; // SEG1OFF  $E008
    case 0x0A:
        m->seg1 = 1;
        break; // SEG1ON   $E00A
    case 0x0C:
        m->seg2 = 0;
        break; // SEG2OFF  $E00C
    case 0x0E:
        m->seg2 = 1;
        break; // SEG2ON   $E00E
    case 0x10:
        m->start = true;
        break; // SETUPON  $E010 (START on)
    case 0x12:
        m->start = false;
        break; // SETUP    $E012 (START off)
    case 0x18:
        m->vtir_enabled = false;
        m->vertical = false; // VTIRDIS also clears the retrace latch (Status bit 2 → 1)
        break; // VTIRDIS $E018
    case 0x1A:
        m->vtir_enabled = true;
        break; // VTIRENB $E01A
    case 0x14:
        m->sfmsk = false;
        break; // SFMSK off $E014
    case 0x16:
        m->sfmsk = true;
        break; // SFMSK on  $E016
    case 0x04:
        m->wwp_on = false;
        break; // DG2OFF $E004 (write-wrong-parity off)
    case 0x06:
        m->wwp_on = true;
        break; // DG2ON  $E006 (write-wrong-parity on)
    case 0x1C: // PAROFF $E01C: disable parity detect + clear any latched error
        m->parity_detect = false;
        if (m->nmi_cb)
            m->nmi_cb(m->nmi_ctx, false);
        break;
    case 0x1E:
        m->parity_detect = true;
        break; // PARON $E01E (parity detect on)
    default:
        break; // $E000/$E002: DIAG1 latch — cosmetic for now
    }
}

// Set the level-7 parity-NMI callback (machine routes it to cpu_set_ipl).
void lisa_mmu_set_nmi(lisa_mmu_t *m, void (*cb)(void *, bool), void *ctx) {
    if (!m)
        return;
    m->nmi_cb = cb;
    m->nmi_ctx = ctx;
}

// Read the Status Register byte.  Bit 2 is the vertical-retrace signal.  We
// model only the *frame-accurate* VBL (docs/lisa.md §8.1, §6.2 in the proposal:
// cycle-exact video dot timing is a non-goal), so rather than reproduce the
// exact dot-clock phase the ROM's video-logic self-test (VIDTST) samples, we
// present a retrace bit that simply *changes* over time — alternating on each
// Status Register read.  VIDTST waits for the bit low then expects it high, so
// an alternating bit satisfies it; `vbl_active` additionally forces the bit set
// across a real retrace window for VBL-aware readers.  The rest read 0 (no
// memory errors, diagnostics inactive).
static uint8_t lisa_status_byte(lisa_mmu_t *m) {
    // Status Register bit 2 = vertical retrace, modelled as the real hardware
    // (docs/lisa.md §7.4).  The bit is ACTIVE-LOW:
    // the video state machine drives it 0 while in vertical retrace and 1 during
    // active scan.  `vertical` is a latch set on the rising edge into each frame's
    // ~90 µs retrace window (a pure function of the cycle counter), cleared during
    // active scan and by the VTIR-disable strobe ($E018).  The ROM's VIDTST waits
    // for bit 2 = 0 (retrace), then strobes VTIRDIS and expects bit 2 = 1 — which
    // works precisely because VTIRDIS clears the latch.  No per-read toggle.
    if (m->sched) {
        uint64_t now = scheduler_cpu_cycles(m->sched);
        uint64_t frame = now / LISA_FRAME_CYCLES;
        if ((now % LISA_FRAME_CYCLES) < LISA_RETRACE_CYCLES) {
            if (frame != m->last_retrace_frame) { // rising edge into a new frame's retrace
                m->vertical = true;
                m->last_retrace_frame = frame;
            }
        } else {
            m->vertical = false; // active scan
        }
    }
    // A latched VBL interrupt (vbl_active) forces "in retrace" so the OS's IPL-1
    // handler, which reads this bit to identify the source, confirms the VBL even
    // if it is serviced past the cycle-accurate retrace window (the kernel was
    // masked).  Reading the Status Register is the OS's VBL acknowledge: clear the
    // latch and drop the IRQ.  This matches LisaEm's edge-fired latched video IRQ
    // (reg68k_external_autovector(IRQ_VIDEO)) — our prior 458-cycle pulse dropped
    // the VBL whenever the kernel was masked through the window, so a freshly
    // dispatched process ran before the VBL-driven segment load.
    bool in_retrace = m->vertical || m->vbl_active;
    if (m->vbl_active) {
        m->vbl_active = false;
        if (m->vbl_ack_cb)
            m->vbl_ack_cb(m->vbl_ack_ctx);
    }
    return in_retrace ? 0u : (uint8_t)(1u << 2); // active-low: 0 in retrace, bit-2 set otherwise
}

// Dispatch an I/O-space read at physical I/O address `phys` (size bytes).
static uint32_t lisa_io_read(lisa_mmu_t *m, uint32_t phys, unsigned size) {
    // Control / video / status block ($E000-$FFFF).
    if (phys >= 0xE000) {
        if ((phys & 0xFFE0) == 0xE000) { // control strobes $E000-$E01F
            lisa_strobe(m, phys);
            return 0xFFFFFFFFu >> ((4 - size) * 8);
        }
        if (phys >= 0xF800) // Status Register ($F800/$F801)
            return lisa_status_byte(m);
        if (phys >= 0xF000) // Memory Error Address latch (physical addr >> 5)
            return m->mealtch;
        // $E800-$EFFF Video Address Latch is write-only; reads float.
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    }
    // Registered peripheral device (VIA/SCC/floppy/Widget in later steps).
    for (int i = 0; i < m->io_count; i++) {
        lisa_io_dev_t *d = &m->io[i];
        if (phys >= d->base && phys < d->base + d->size) {
            uint32_t off = phys - d->base;
            if (size == 1)
                return d->iface->read_uint8(d->dev, off);
            if (size == 2)
                return d->iface->read_uint16(d->dev, off);
            return d->iface->read_uint32(d->dev, off);
        }
    }
    // Unmapped I/O: floating bus reads all-ones (a real Lisa eventually bus-
    // errors on absent I/O; that refinement waits until devices are wired).
    return 0xFFFFFFFFu >> ((4 - size) * 8);
}

// Dispatch an I/O-space write at physical I/O address `phys`.
static void lisa_io_write(lisa_mmu_t *m, uint32_t phys, unsigned size, uint32_t value) {
    if (phys >= 0xE000) {
        if ((phys & 0xFFE0) == 0xE000) { // control strobes
            lisa_strobe(m, phys);
            return;
        }
        if (phys >= 0xE800 && phys < 0xF000) { // Video Address Latch
            if (getenv("GSFB") && m->vidlatch != (uint8_t)value)
                fprintf(stderr, "GSVID latch=%02x fb_phys=%06x pc=%06x i=%llu\n", (uint8_t)value,
                        ((uint32_t)(uint8_t)value) << 15, cpu_get_pc(system_cpu()) & 0xffffff,
                        (unsigned long long)cpu_instr_count());
            m->vidlatch = (uint8_t)value;
            return;
        }
        return; // status / mem-err latches are read-only
    }
    for (int i = 0; i < m->io_count; i++) {
        lisa_io_dev_t *d = &m->io[i];
        if (phys >= d->base && phys < d->base + d->size) {
            uint32_t off = phys - d->base;
            if (size == 1)
                d->iface->write_uint8(d->dev, off, (uint8_t)value);
            else if (size == 2)
                d->iface->write_uint16(d->dev, off, (uint16_t)value);
            else
                d->iface->write_uint32(d->dev, off, value);
            return;
        }
    }
    // Unmapped I/O write: dropped.
}

// === Serial-number PROM ($00FE8000, map-land reads) =========================
//
// The boot ROM's RDSERN reads the machine serial number bit-serially from this
// special-I/O location, sampling bit 15 of each word over two 56-word blocks.
// We synthesise a valid stream: each 56-bit half begins with an 8-bit sync
// ($FF) followed by zero bits.  The ROM then extracts all-zero data nibbles;
// its checksum (Σbytes[0..23] + byte27 − 60 == 100·b24+10·b25+b26) reduces to
// the four sync nibbles (4×$0F = 60) cancelling the −60, i.e. 0 == 0, so it
// validates with no error.  (Modeling the real engraved serial would only
// change the displayed/stored number, not the boot outcome.)

#define LISA_SERIAL_HALF_BITS  56 // bits per block
#define LISA_SERIAL_TOTAL_BITS 112 // two blocks

// One serial-stream bit: sync (1) for the first 8 bits of each half, else 0.
static int lisa_serial_bit(uint32_t index) {
    return ((index % LISA_SERIAL_HALF_BITS) < 8) ? 1 : 0;
}

// Read of the serial PROM region: bit 15 of each returned word carries the
// next stream bit (RDSERN reads via MOVEM, sampling bit 15).
static uint32_t lisa_serial_read(lisa_mmu_t *m, unsigned size) {
    if (size == 4) {
        int hi = lisa_serial_bit(m->serial_ctr++ % LISA_SERIAL_TOTAL_BITS);
        int lo = lisa_serial_bit(m->serial_ctr++ % LISA_SERIAL_TOTAL_BITS);
        return ((uint32_t)hi << 31) | ((uint32_t)lo << 15);
    }
    int b = lisa_serial_bit(m->serial_ctr++ % LISA_SERIAL_TOTAL_BITS);
    return (uint32_t)b << 15; // word: bit 15
}

// True for reads the serial PROM should service: $00FE8000-$00FE800F in
// map-land (in START mode this window is the MMU-127 descriptor register).
static bool lisa_is_serial(const lisa_mmu_t *m, uint32_t addr) {
    return !m->start && (addr & 0x00FFFFF0u) == 0x00FE8000u;
}

// === Translation ===========================================================

// Resolve a CPU logical access to a route + physical/descriptor coordinates.
static lisa_resolved_t lisa_resolve(lisa_mmu_t *m, uint32_t addr, bool supervisor, bool is_write) {
    lisa_resolved_t r = {0};
    addr &= 0x00FFFFFF;
    int latch_ctx = (m->seg2 << 1) | m->seg1;

    // START mode, bit14=0 → special-I/O directly (MMU bypassed).
    if (m->start && !(addr & 0x4000)) {
        if (addr & 0x8000) { // descriptor RAM (raw latch context)
            r.route = L_MMUREG;
            r.seg = (addr >> 17) & 0x7F;
            r.ctx = latch_ctx;
            r.reg_is_sor = (addr >> 3) & 1; // +8 = SOR, +0 = SLR
        } else {
            r.route = L_ROM;
            r.phys = addr & 0x3FFF; // 16 KB ROM
        }
        return r;
    }

    // Normal segment translation. Supervisor mode forces context 0.
    int ctx = supervisor ? 0 : latch_ctx;
    int seg = (addr >> 17) & 0x7F;
    uint32_t page = (addr >> 9) & 0xFF;
    uint32_t byte_off = addr & 0x1FF;
    uint16_t slr = m->slr[ctx][seg];
    int acc = (slr >> 8) & 0xF;
    uint32_t limit = slr & 0xFF;

    if (acc == ACC_INVALID || acc < ACC_MEM_RO_STK) {
        r.route = L_FAULT; // invalid / unprogrammed segment
        return r;
    }

    bool is_mem = (acc & 0xC) == 0x4; // 01xx
    bool is_io = (acc == ACC_IO);
    bool is_special = (acc == ACC_SPECIAL);
    bool stack = is_mem && !(acc & 0x1); // bit0=0 → stack segment
    bool read_only = is_mem && !(acc & 0x2); // bit1=0 → read-only

    // Limit check: the hardware adds the page displacement to the (two's-
    // complement) length byte and faults on a carry mismatch.  Non-stack:
    // valid iff page + limit does not overflow 8 bits (limit $00 = 256 pages).
    // Stack segments grow downward; the carry sense inverts.
    bool in_range;
    if (stack)
        in_range = (page + limit) >= 0x100; // best-effort; refined with LOS
    else
        in_range = (page + limit) < 0x100;
    if (!in_range) {
        r.route = L_FAULT;
        return r;
    }
    if (is_write && read_only) {
        r.route = L_FAULT;
        return r;
    }

    uint32_t phys_page = (m->sor[ctx][seg] + page) & 0xFFF; // high nibble forced 0
    uint32_t phys = (phys_page << 9) | byte_off; // 21-bit physical

    if (is_mem) {
        r.route = L_RAM;
        r.phys = phys;
    } else if (is_io) {
        r.route = L_IO;
        r.phys = phys;
    } else if (is_special) {
        // Segment 127 (SOR=0) → boot ROM; offset is the low translated address.
        // The descriptor RAM is reached only via the START bit14=0 path above,
        // so map-land special-I/O resolves to ROM.
        r.route = L_ROM;
        r.phys = phys & 0x3FFF;
    } else {
        r.route = L_FAULT;
    }
    return r;
}

// Latch a 68000 bus error for the faulting access (group-0 exception; not a
// PMMU descriptor retry, so the decoder skips the faulting instruction).
static void lisa_raise_bus_error(uint32_t addr, bool is_read, bool supervisor) {
    if (g_bus_error_pending)
        return;
    g_bus_error_pending = true;
    g_bus_error_address = addr;
    g_bus_error_rw = is_read;
    g_bus_error_fc = supervisor ? 5 : 1;
    g_bus_error_is_pmmu = false;
    if (g_bus_error_instr_ptr)
        *g_bus_error_instr_ptr = 0; // force decoder loop exit
}

// Read a big-endian value of `size` bytes from the boot ROM at byte offset.
static uint32_t lisa_rom_read(const lisa_mmu_t *m, uint32_t off, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t idx = (off + i) & 0x3FFF;
        v = (v << 8) | (idx < m->rom_size ? m->rom[idx] : 0xFF);
    }
    return v;
}

// Read a big-endian value from physical RAM, or all-ones if beyond installed
// RAM (matches floating-bus behaviour the ROM's memory sizing depends on).
// A read of a location carrying deliberately-bad parity (PARTST) latches the
// failing address and raises the level-7 parity NMI.
static uint32_t lisa_ram_read(lisa_mmu_t *m, uint32_t phys, unsigned size) {
    if (m->parity_detect && (phys >> 5) == m->bad_par_gran) {
        m->mealtch = phys >> 5; // Memory Error Address latch (64-byte resolution)
        if (m->nmi_cb)
            m->nmi_cb(m->nmi_ctx, true); // parity NMI (level 7)
    }
    if (phys < m->ram_min || phys + size > m->ram_max) {
        if (getenv("GSOOB")) {
            uint32_t pc = cpu_get_pc(system_cpu()) & 0xffffff;
            static int n = 0;
            if (pc < 0xfe0000 && cpu_instr_count() > 19886000 && n < 80) { // post-R56, non-ROM
                n++;
                fprintf(stderr, "GSOOB read phys=%06x size=%u pc=%06x i=%llu\n", phys, size, pc,
                        (unsigned long long)cpu_instr_count());
            }
        }
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    }
    uint32_t off = phys - m->ram_min, v = 0;
    for (unsigned i = 0; i < size; i++)
        v = (v << 8) | m->ram[off + i];
    return v;
}

static void lisa_ram_write(lisa_mmu_t *m, uint32_t phys, unsigned size, uint32_t value) {
    // Write-wrong-parity mode marks this 32-byte granule bad; a normal write
    // restores good parity.
    if (m->wwp_on)
        m->bad_par_gran = phys >> 5;
    else if ((phys >> 5) == m->bad_par_gran)
        m->bad_par_gran = 0xFFFFFFFFu;
    if (phys < m->ram_min || phys + size > m->ram_min + m->ram_size) {
        if (getenv("GSWILD")) {
            uint32_t pc = cpu_get_pc(system_cpu()) & 0xffffff;
            static int n = 0;
            if (pc < 0xfe0000 && n < 60) { // skip the boot ROM's memory-sizing probe
                n++;
                fprintf(stderr, "GSWILD write phys=%06x val=%08x pc=%06x i=%llu\n", phys, value, pc,
                        (unsigned long long)cpu_instr_count());
            }
        }
        return; // outside installed RAM: dropped
    }
    uint32_t off = phys - m->ram_min;
    for (unsigned i = 0; i < size; i++)
        m->ram[off + i] = (uint8_t)(value >> ((size - 1 - i) * 8));
}

// 12-bit descriptor register read (word-sized on real hardware).
static uint16_t lisa_mmureg_read(const lisa_mmu_t *m, const lisa_resolved_t *r) {
    uint16_t v = r->reg_is_sor ? m->sor[r->ctx][r->seg] : m->slr[r->ctx][r->seg];
    return v & 0x0FFF;
}

static void lisa_mmureg_write(lisa_mmu_t *m, const lisa_resolved_t *r, uint16_t value) {
    if (r->reg_is_sor)
        m->sor[r->ctx][r->seg] = value & 0x0FFF;
    else
        m->slr[r->ctx][r->seg] = value & 0x0FFF;
    if (getenv("GSSEG24") && r->seg == 24) {
        extern uint64_t cpu_instr_count(void);
        FILE *_f = fopen("/tmp/gsseg24.out", "a");
        if (_f) {
            fprintf(_f, "WR seg24 ctx=%d %s=%03x acc=%x lim=%02x pc=%06x i=%llu\n", r->ctx,
                    r->reg_is_sor ? "SOR" : "SLR", value & 0xFFF, r->reg_is_sor ? 0 : ((value >> 8) & 0xF),
                    r->reg_is_sor ? 0 : (value & 0xFF), cpu_get_pc(system_cpu()) & 0xffffff,
                    (unsigned long long)cpu_instr_count());
            fclose(_f);
        }
    }
    if (getenv("GSSEG101") && (r->seg == 100 || r->seg == 101 || r->seg == 102)) {
        extern uint64_t cpu_instr_count(void);
        fprintf(stderr, "GSSEG seg=%d ctx=%d %s=%03x acc=%x lim=%02x pc=%06x i=%llu\n", r->seg, r->ctx,
                r->reg_is_sor ? "SOR" : "SLR", value & 0xFFF, r->reg_is_sor ? 0 : ((value >> 8) & 0xF),
                r->reg_is_sor ? 0 : (value & 0xFF), cpu_get_pc(system_cpu()) & 0xffffff,
                (unsigned long long)cpu_instr_count());
    }
}

// === CPU slow-path delegates ===============================================

static uint32_t lisa_read(uint32_t addr, unsigned size, bool supervisor) {
    lisa_mmu_t *m = g_lisa_mmu;
    if (__builtin_expect(lisa_is_serial(m, addr & 0x00FFFFFFu), 0))
        return lisa_serial_read(m, size); // serial-number PROM bit-stream
    if (getenv("GSMAXMEM") && (addr & 0x00FFFFFF) == 0x294 && size == 4) {
        uint32_t pc = cpu_get_pc(system_cpu()) & 0xffffff;
        if (pc < 0xfe0000) { // reserve the 32 KB screen below MAXMEM (non-ROM reads only)
            uint32_t top = m->ram_min + m->ram_size;
            if (top > 0x200000u)
                top = 0x200000u;
            return top - 0x8000u;
        }
    }
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, false);
    if (getenv("GSSEG17") && (((addr & 0x00FFFFFF) >= 0x220000 && (addr & 0x00FFFFFF) < 0x240000) ||
                              ((addr & 0x00FFFFFF) >= 0xf78000 && (addr & 0x00FFFFFF) < 0xf78400))) {
        extern uint64_t cpu_instr_count(void);
        int latch = (m->seg2 << 1) | m->seg1;
        int ctx = supervisor ? 0 : latch;
        int seg = (addr >> 17) & 0x7F;
        FILE *_f = fopen("/tmp/gsseg17.out", "a");
        if (_f) {
            fprintf(_f,
                    "RD addr=%06x sz=%u sup=%d latch=%d ctx=%d seg=%d route=%d sor=%03x slr=%03x phys=%06x i=%llu\n",
                    addr & 0xFFFFFF, size, supervisor, latch, ctx, seg, (int)r.route, m->sor[ctx][seg],
                    m->slr[ctx][seg], r.phys, (unsigned long long)cpu_instr_count());
            fclose(_f);
        }
    }
    switch (r.route) {
    case L_RAM:
        return lisa_ram_read(m, r.phys, size);
    case L_ROM:
        return lisa_rom_read(m, r.phys, size);
    case L_IO:
        return lisa_io_read(m, r.phys, size);
    case L_MMUREG:
        return lisa_mmureg_read(m, &r);
    case L_FAULT:
    default:
        if (getenv("GSSEG24") && (addr & 0x00FFFFFF) >= 0x300000 && (addr & 0x00FFFFFF) < 0x320000) {
            extern uint64_t cpu_instr_count(void);
            int latch = (m->seg2 << 1) | m->seg1;
            int ctx = supervisor ? 0 : latch;
            int seg = ((addr >> 17) & 0x7F);
            uint16_t slr = m->slr[ctx][seg];
            FILE *_f = fopen("/tmp/gsseg24.out", "a");
            if (_f) {
                fprintf(_f, "FLT addr=%06x sup=%d latch=%d ctx=%d seg=%d slr=%03x acc=%x sor=%03x i=%llu\n",
                        addr & 0xFFFFFF, supervisor, latch, ctx, seg, slr, (slr >> 8) & 0xF, m->sor[ctx][seg],
                        (unsigned long long)cpu_instr_count());
                fclose(_f);
            }
        }
        lisa_raise_bus_error(addr, true, supervisor);
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    }
}

static void lisa_write(uint32_t addr, unsigned size, bool supervisor, uint32_t value) {
    lisa_mmu_t *m = g_lisa_mmu;
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, true);
    switch (r.route) {
    case L_RAM:
        lisa_ram_write(m, r.phys, size, value);
        return;
    case L_IO:
        lisa_io_write(m, r.phys, size, value);
        return;
    case L_MMUREG:
        lisa_mmureg_write(m, &r, (uint16_t)value);
        return;
    case L_ROM:
        return; // writes to ROM space are ignored
    case L_FAULT:
    default:
        lisa_raise_bus_error(addr, false, supervisor);
        return;
    }
}

uint8_t lisa_mmu_read8(uint32_t addr, bool supervisor) {
    return (uint8_t)lisa_read(addr, 1, supervisor);
}
uint16_t lisa_mmu_read16(uint32_t addr, bool supervisor) {
    return (uint16_t)lisa_read(addr, 2, supervisor);
}
uint32_t lisa_mmu_read32(uint32_t addr, bool supervisor) {
    return lisa_read(addr, 4, supervisor);
}
void lisa_mmu_write8(uint32_t addr, bool supervisor, uint8_t value) {
    lisa_write(addr, 1, supervisor, value);
}
void lisa_mmu_write16(uint32_t addr, bool supervisor, uint16_t value) {
    lisa_write(addr, 2, supervisor, value);
}
void lisa_mmu_write32(uint32_t addr, bool supervisor, uint32_t value) {
    lisa_write(addr, 4, supervisor, value);
}

uint32_t lisa_mmu_debug_read(uint32_t addr, unsigned size, bool supervisor) {
    lisa_mmu_t *m = g_lisa_mmu;
    if (!m)
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, false);
    switch (r.route) {
    case L_RAM:
        return lisa_ram_read(m, r.phys, size);
    case L_ROM:
        return lisa_rom_read(m, r.phys, size);
    case L_MMUREG:
        return lisa_mmureg_read(m, &r);
    // Skip I/O device dispatch (side effects) and faults for debug reads.
    case L_IO:
    case L_FAULT:
    default:
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    }
}

bool lisa_mmu_debug_write(uint32_t addr, unsigned size, bool supervisor, uint32_t value) {
    lisa_mmu_t *m = g_lisa_mmu;
    if (!m)
        return false;
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, true);
    if (r.route != L_RAM)
        return false; // only RAM is poke-able without side effects
    lisa_ram_write(m, r.phys, size, value);
    return true;
}
