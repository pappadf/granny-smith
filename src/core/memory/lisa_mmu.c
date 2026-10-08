// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_mmu.c
// Apple Lisa custom segment MMU. See lisa_mmu.h and docs/reference/machines/lisa/lisa.md §4-7.
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
//    $00FCE012 = SETUP OFF.  (docs/reference/machines/lisa/lisa.md §6.1 lists these reversed — the ROM
//    source is authoritative.)

#include "lisa_mmu.h"

#include "checkpoint.h" // system_{read,write}_checkpoint_data are macros
#include "cpu.h"
#include "debug.h"
#include "debug_mmu.h"
#include "memory.h"
#include "object.h"
#include "scheduler.h"

#include "log.h"
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("memory");

// Lisa video timing (docs/reference/machines/lisa/lisa.md §8): the state machine scans 379 lines of 720
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
    // Lisa memory boards populate the address space down from $100000: the
    // board in slot 2 ends there and the slot-1 board sits above it, so RAM
    // starts at $100000 less the slot-2 board -- 512 KB (one 512 KB board)
    // [$80000,$100000), 1 MB (two 512 KB) [$80000,$180000), 1.5 MB (a 1 MB
    // board over a 512 KB one) [$80000,$200000), 2 MB (two 1 MB) [0,$200000).
    // The boot ROM's MEMSIZ scans up from 0 and reports this base as MINMEM
    // ($2A4); the OS lays out real memory (and the kernel stack at the top of
    // it) relative to that base.  With RAM wrongly based at 0 below 2 MB, the
    // OS placed the kernel stack on a non-existent high page.
    // The Macintosh XL (ram_high=false) keeps RAM at 0: MacWorks' framebuffer and
    // boot path live in low memory.
    uint32_t slot2_board = ram_size >= 0x200000u ? 0x100000u : 0x80000u;
    m->ram_min = ram_high ? 0x100000u - slot2_board : 0u;
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
    if (cp)
        lisa_mmu_checkpoint_restore(m, cp); // same field order as the save
    return m;
}

// Power the MMU up again without rebuilding it: the guest-visible fields
// lisa_mmu_init sets (calloc plus the explicit START/bad-granule values),
// leaving the wiring -- RAM/ROM pointers, I/O devices, callbacks -- alone.
void lisa_mmu_power_on(lisa_mmu_t *m) {
    if (!m)
        return;
    memset(m->sor, 0, sizeof(m->sor)); // descriptor RAM, as at construction
    memset(m->slr, 0, sizeof(m->slr));
    m->start = true; // "satisfied automatically at power-on time"
    m->seg1 = m->seg2 = 0;
    m->vidlatch = 0;
    m->vtir_enabled = m->sfmsk = m->hdmsk = false;
    m->vbl_active = false;
    m->status_toggle = 0;
    m->vertical = false;
    m->last_retrace_frame = 0;
    m->serial_ctr = 0;
    m->wwp_on = m->parity_detect = false;
    m->bad_par_gran = 0xFFFFFFFFu; // no bad-parity location
    m->mealtch = 0;
}

void lisa_mmu_delete(lisa_mmu_t *m) {
    if (!m)
        return;
    if (g_lisa_mmu == m)
        g_lisa_mmu = NULL;
    free(m);
}

// Serialise the Lisa segment MMU's guest state, in one canonical order shared
// by save and restore.
//
// This was a symmetric NO-OP -- `(void)m; (void)cp;` on both sides -- which is
// why nothing ever noticed: a save wrote zero bytes and a restore read zero
// bytes, so the stream stayed consistent and only the MACHINE came back wrong.
// Lost were sor[4][128] and slr[4][128], i.e. 4 KiB of descriptor RAM covering
// the entire address space, plus every latch below.  lisa_mmu_init hard-sets
// start = true and zeroes the descriptors, so a restored machine resumed a
// mid-boot CPU against a POWER-ON MMU: `start` re-routes every low access to
// ROM/descriptor RAM and every mapped segment reads slr = 0, which is below
// ACC_MEM_RO_STK, so it faults.
//
// Field-by-field rather than a POD blob because the struct interleaves host
// pointers (rom, sched, the NMI and VBL-ack callbacks, the io[] table) with
// guest state, so there is no single prefix to take.  Both directions walk the
// same list; the stream is build-ID gated, so widening it later is free as long
// as both halves move together.
#define LISA_MMU_CP_FIELDS(OP, m, cp)                                                                                  \
    OP(cp, &(m)->sor, sizeof((m)->sor)); /* descriptor RAM: origin per context */                                      \
    OP(cp, &(m)->slr, sizeof((m)->slr)); /* descriptor RAM: limit/access */                                            \
    OP(cp, &(m)->start, sizeof((m)->start)); /* START/SETUP latch */                                                   \
    OP(cp, &(m)->seg1, sizeof((m)->seg1)); /* context selector bits */                                                 \
    OP(cp, &(m)->seg2, sizeof((m)->seg2));                                                                             \
    OP(cp, &(m)->vidlatch, sizeof((m)->vidlatch)); /* framebuffer base A15-A20 */                                      \
    OP(cp, &(m)->vtir_enabled, sizeof((m)->vtir_enabled));                                                             \
    OP(cp, &(m)->sfmsk, sizeof((m)->sfmsk));                                                                           \
    OP(cp, &(m)->hdmsk, sizeof((m)->hdmsk));                                                                           \
    OP(cp, &(m)->vbl_active, sizeof((m)->vbl_active));                                                                 \
    OP(cp, &(m)->status_toggle, sizeof((m)->status_toggle));                                                           \
    OP(cp, &(m)->vertical, sizeof((m)->vertical)); /* retrace latch */                                                 \
    OP(cp, &(m)->last_retrace_frame, sizeof((m)->last_retrace_frame));                                                 \
    OP(cp, &(m)->serial_ctr, sizeof((m)->serial_ctr)); /* RDSERN bit-stream cursor */                                  \
    OP(cp, &(m)->wwp_on, sizeof((m)->wwp_on)); /* parity-test state */                                                 \
    OP(cp, &(m)->parity_detect, sizeof((m)->parity_detect));                                                           \
    OP(cp, &(m)->bad_par_gran, sizeof((m)->bad_par_gran));                                                             \
    OP(cp, &(m)->mealtch, sizeof((m)->mealtch))

void lisa_mmu_checkpoint(lisa_mmu_t *m, checkpoint_t *cp) {
    if (!m || !cp)
        return;
    LISA_MMU_CP_FIELDS(system_write_checkpoint_data, m, cp);
}

void lisa_mmu_checkpoint_restore(lisa_mmu_t *m, checkpoint_t *cp) {
    if (!m || !cp)
        return;
    LISA_MMU_CP_FIELDS(system_read_checkpoint_data, m, cp);
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
// model only the *frame-accurate* VBL (cycle-exact video dot timing is a
// non-goal), so rather than reproduce the
// exact dot-clock phase the ROM's video-logic self-test (VIDTST) samples, we
// present a retrace bit that simply *changes* over time — alternating on each
// Status Register read.  VIDTST waits for the bit low then expects it high, so
// an alternating bit satisfies it; `vbl_active` additionally forces the bit set
// across a real retrace window for VBL-aware readers.  The rest read 0 (no
// memory errors, diagnostics inactive).
static uint8_t lisa_status_byte(lisa_mmu_t *m) {
    // Status Register bit 2 = vertical retrace, modelled as the real hardware
    // (docs/reference/machines/lisa/lisa.md §7.4).  The bit is ACTIVE-LOW:
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
    // latch and drop the IRQ.  This matches the real hardware's edge-fired latched
    // video IRQ — our prior 458-cycle pulse dropped
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
            lisa_strobe(m, phys); // a long access is two word bus cycles → two strobes
            if (size == 4)
                lisa_strobe(m, phys + 2);
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
    // Unmapped I/O outside the slot decodes (empty slots fault in
    // lisa_resolve): the on-board range floats and reads all-ones.
    LOG(3, "unclaimed I/O read $%04X size=%u", (unsigned)phys, size);
    return 0xFFFFFFFFu >> ((4 - size) * 8);
}

// Dispatch an I/O-space write at physical I/O address `phys`.
static void lisa_io_write(lisa_mmu_t *m, uint32_t phys, unsigned size, uint32_t value) {
    if (phys >= 0xE000) {
        if ((phys & 0xFFE0) == 0xE000) { // control strobes
            // A control strobe fires on the address decode of each bus cycle.  A
            // 68000 long access is two word bus cycles (phys, then phys+2), so a
            // MOVE.L to a strobe pair fires BOTH strobes — e.g. MacWorks' VBL
            // re-arm `MOVE.L D0,$FCE018` strobes VTIRDIS ($E018) then VTIRENB
            // ($E01A), net-enabling the video interrupt.  Modelling it as one
            // strobe left the video VBL stuck disabled.
            lisa_strobe(m, phys);
            if (size == 4)
                lisa_strobe(m, phys + 2);
            return;
        }
        if (phys >= 0xE800 && phys < 0xF000) { // Video Address Latch
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
    LOG(3, "unclaimed I/O write $%04X size=%u value=$%X", (unsigned)phys, size, (unsigned)value);
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

// End of the expansion-slot decodes in I/O space: slots 1-3 each own a low
// and a high 8 KB decode in $0000-$BFFF (lisa.md, the I/O space map).
#define LISA_SLOT_IO_END 0xC000u

// True when a registered device answers physical I/O address `phys`.
static bool lisa_io_claimed(const lisa_mmu_t *m, uint32_t phys) {
    for (int i = 0; i < m->io_count; i++)
        if (phys >= m->io[i].base && phys < m->io[i].base + m->io[i].size)
            return true;
    return false;
}

// Name of an SLR access/space code, for the debugger.
static const char *lisa_acc_name(int acc) {
    switch (acc) {
    case ACC_MEM_RO_STK:
        return "ram-ro-stack";
    case ACC_MEM_RO:
        return "ram-ro";
    case ACC_MEM_RW_STK:
        return "ram-rw-stack";
    case ACC_MEM_RW:
        return "ram-rw";
    case ACC_IO:
        return "io";
    case ACC_SPECIAL:
        return "special";
    default:
        return "invalid";
    }
}

// Resolve a CPU logical access to a route + physical/descriptor coordinates.
// `x` and `trace` serve the debugger (lisa_mmu_debug_translate): when given,
// they receive the access the segment allows, how large a region resolves
// the same way, and the segment descriptor consulted.  Every other caller
// goes through lisa_resolve, which passes NULL and so compiles them away.
static inline __attribute__((always_inline)) lisa_resolved_t lisa_resolve_traced(lisa_mmu_t *m, uint32_t addr,
                                                                                 bool supervisor, bool is_write,
                                                                                 mmu_xlate_t *x, mmu_trace_t *trace) {
    lisa_resolved_t r = {0};
    addr &= 0x00FFFFFF;
    int latch_ctx = (m->seg2 << 1) | m->seg1;

    // START mode, bit14=0 → special-I/O directly (MMU bypassed).
    if (m->start && !(addr & 0x4000)) {
        if (x) {
            x->via = "identity";
            x->access = (addr & 0x8000) ? "rw" : "ro"; // descriptor RAM, or the ROM
            x->span_bits = 14; // bit 14 alternates the bypass every 16 KB
        }
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
    mmu_trace_step_t *ts = mmu_trace_step(trace, "segment");
    if (ts) {
        mmu_trace_str(ts, "name", "segment");
        mmu_trace_uint(ts, "index", (uint32_t)seg);
        mmu_trace_uint(ts, "context", (uint32_t)ctx);
        mmu_trace_hex(ts, "sor", m->sor[ctx][seg]);
        mmu_trace_hex(ts, "slr", slr);
        mmu_trace_str(ts, "type", lisa_acc_name(acc));
        mmu_trace_uint(ts, "page", page);
        mmu_trace_uint(ts, "limit", limit);
    }
    if (x) {
        x->via = "segment";
        x->span_bits = 9; // one 512-byte page
    }

    if (acc == ACC_INVALID || acc < ACC_MEM_RO_STK) {
        r.route = L_FAULT; // invalid / unprogrammed segment
        if (x)
            x->span_bits = m->start ? 14 : 17; // the whole segment, but for START's 16 KB bypass windows
        mmu_trace_str(ts, "reason", "invalid");
        mmu_trace_outcome(ts, "fault");
        return r;
    }

    bool is_mem = (acc & 0xC) == 0x4; // 01xx
    bool is_io = (acc == ACC_IO);
    bool is_special = (acc == ACC_SPECIAL);
    bool stack = is_mem && !(acc & 0x1); // bit0=0 → stack segment
    bool read_only = is_mem && !(acc & 0x2); // bit1=0 → read-only

    // Limit check.  The SLR low byte is the segment length encoded so the valid
    // page window pivots on the boundary page B = (limit ^ 0xFF) = 0xFF - limit:
    //   - Non-stack (grows up):   valid pages [0, B]      → page <= 0xFF - limit
    //   - Stack    (grows down):  valid pages [B, 0xFF]   → page >= 0xFF - limit
    // The boundary page B itself is valid in both senses (MEM pageend=B; STK
    // pagestart=B, pageend=255).  A stack with limit $00 (B=0xFF) therefore has
    // exactly the top page valid — which is where the Xenix boot loader relocates
    // and jumps (seg 122, SLR=400: page 0xFF).  The previous stack test
    // (page+limit >= 0x100) was one page too strict and faulted that jump.
    bool in_range;
    if (stack)
        in_range = (page + limit) >= 0xFF;
    else
        in_range = (page + limit) < 0x100;
    if (!in_range) {
        r.route = L_FAULT;
        mmu_trace_str(ts, "reason", "limit");
        mmu_trace_outcome(ts, "fault");
        return r;
    }
    if (is_write && read_only) {
        r.route = L_FAULT;
        return r;
    }

    uint32_t phys_page = (m->sor[ctx][seg] + page) & 0xFFF; // high nibble forced 0
    uint32_t phys = (phys_page << 9) | byte_off; // 21-bit physical
    if (x)
        x->access = (read_only || is_special) ? "ro" : "rw";

    if (is_mem) {
        r.route = L_RAM;
        r.phys = phys;
    } else if (is_io) {
        // An expansion slot with no card never answers: the CPU times out
        // after 30-300 us and takes a bus error (Lisa Hardware Manual 1983,
        // bus handshaking).  The boot ROM's RDSLOTS and the OS's EXISTS_CARD
        // both detect an empty slot by that bus error; a floating read of
        // $FF instead made the OS see a card with ID $FFF in every slot.
        r.route = (phys < LISA_SLOT_IO_END && !lisa_io_claimed(m, phys)) ? L_FAULT : L_IO;
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
    if (r.route == L_FAULT) {
        mmu_trace_str(ts, "reason", is_io ? "empty slot" : "invalid");
        mmu_trace_outcome(ts, "fault");
    } else {
        mmu_trace_hex(ts, "phys", r.phys);
        mmu_trace_outcome(ts, "hit");
    }
    return r;
}

// The resolver every access path uses (no debugger outputs).
static lisa_resolved_t lisa_resolve(lisa_mmu_t *m, uint32_t addr, bool supervisor, bool is_write) {
    return lisa_resolve_traced(m, addr, supervisor, is_write, NULL, NULL);
}

// Names of the physical spaces a route lands in.
static const char *const lisa_space_names[] = {[L_RAM] = "ram", [L_IO] = "io", [L_ROM] = "rom", [L_MMUREG] = "mmureg"};

void lisa_mmu_debug_translate(lisa_mmu_t *m, uint32_t addr, bool supervisor, mmu_xlate_t *x, mmu_trace_t *trace) {
    x->via = "segment";
    x->access = "rw";
    x->space = NULL;
    x->span_bits = 9;
    lisa_resolved_t r = lisa_resolve_traced(m, addr, supervisor, false, x, trace);
    x->valid = r.route != L_FAULT;
    x->phys = !x->valid ? addr : r.route == L_MMUREG ? (addr & 0x00FFFFFF) : r.phys;
    if (x->valid)
        x->space = lisa_space_names[r.route];
}

bool lisa_mmu_translate(lisa_mmu_t *m, uint32_t addr, bool supervisor, uint32_t *phys, const char **space) {
    if (!m)
        return false;
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, false);
    if (r.route == L_FAULT)
        return false;
    if (phys)
        *phys = r.route == L_MMUREG ? addr : r.phys;
    if (space)
        *space = lisa_space_names[r.route];
    return true;
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
    memory_end_sprint(g_bus_error_instr_ptr); // force decoder loop exit
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
        return; // outside installed RAM: dropped
    }
    uint32_t off = phys - m->ram_min;
    for (unsigned i = 0; i < size; i++)
        m->ram[off + i] = (uint8_t)(value >> ((size - 1 - i) * 8));
}

// === Absolute cursor positioning (mouse.move ... "global") =================
//
// The LisaOS keeps the live on-screen cursor position in two word globals in the
// OS globals block: $CC00F0 = X (horizontal pixel) and $CC00F2 = Y (vertical
// pixel).  They are written by the cursor tracker (rev-H LOS 3.1 routine at
// $520DA2/$520DA8) in SUPERVISOR mode — i.e. context 0 — and re-asserted on every
// VBL, so a supervisor read always returns the live cursor.  (These were found by
// instrumenting the writes that track injected COPS deltas; the $486/$488 globals
// some references cite are a different, stale copy in our model and never move.)  The COPS
// "warp" closed loop reads these and emits corrective deltas to reach a target.
#define LISA_CURSOR_X_ADDR 0x00CC00F0u
#define LISA_CURSOR_Y_ADDR 0x00CC00F2u

// Read the live LisaOS cursor X/Y in supervisor context.  Returns false if the
// globals block is not currently mapped (e.g. before the OS is up).  `ctx` is
// accepted for API compatibility but ignored — the cursor lives in supervisor
// (context 0) space.
bool lisa_mmu_get_cursor(int ctx, int *x, int *y) {
    (void)ctx;
    lisa_mmu_t *m = g_lisa_mmu;
    if (!m)
        return false;
    lisa_resolved_t rx = lisa_resolve(m, LISA_CURSOR_X_ADDR, /*supervisor=*/true, /*is_write=*/false);
    lisa_resolved_t ry = lisa_resolve(m, LISA_CURSOR_Y_ADDR, /*supervisor=*/true, /*is_write=*/false);
    if (rx.route != L_RAM || ry.route != L_RAM)
        return false;
    if (x)
        *x = (int)(int16_t)lisa_ram_read(m, rx.phys, 2);
    if (y)
        *y = (int)(int16_t)lisa_ram_read(m, ry.phys, 2);
    return true;
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
}

// === CPU slow-path delegates ===============================================

// Fire a memory logpoint (debug.logpoints --read/--write) for a CPU access.
// The Lisa CPU path bypasses the generic memory.c slow path (memory.c routes
// everything here when g_lisa_mmu != NULL), so the logpoint hook is invoked
// here instead.  Gated on the per-(logical-)page refcount the install path
// maintains, so it costs one array lookup per access when no logpoint is set
// and nothing more.  (Like the generic path, this only sees CPU accesses, not
// the device engines' own writes — there are none on the Lisa.)
static inline void lisa_logpoint_notify(uint32_t addr, unsigned size, uint32_t value, bool is_write) {
    if (!g_mem_logpoint_hook || !g_mem_logpoint_page_count)
        return;
    uint32_t page = addr >> PAGE_SHIFT;
    if (page < g_page_count && g_mem_logpoint_page_count[page])
        g_mem_logpoint_hook(addr, size, value, is_write);
}

static uint32_t lisa_read(uint32_t addr, unsigned size, bool supervisor) {
    lisa_mmu_t *m = g_lisa_mmu;
    if (__builtin_expect(lisa_is_serial(m, addr & 0x00FFFFFFu), 0))
        return lisa_serial_read(m, size); // serial-number PROM bit-stream
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, false);
    uint32_t v;
    switch (r.route) {
    case L_RAM:
        v = lisa_ram_read(m, r.phys, size);
        break;
    case L_ROM:
        v = lisa_rom_read(m, r.phys, size);
        break;
    case L_IO:
        v = lisa_io_read(m, r.phys, size);
        break;
    case L_MMUREG:
        v = lisa_mmureg_read(m, &r);
        break;
    case L_FAULT:
    default:
        lisa_raise_bus_error(addr, true, supervisor);
        return 0xFFFFFFFFu >> ((4 - size) * 8);
    }
    lisa_logpoint_notify(addr, size, v, false);
    return v;
}

static void lisa_write(uint32_t addr, unsigned size, bool supervisor, uint32_t value) {
    lisa_mmu_t *m = g_lisa_mmu;
    lisa_resolved_t r = lisa_resolve(m, addr, supervisor, true);
    switch (r.route) {
    case L_RAM:
        lisa_ram_write(m, r.phys, size, value);
        break;
    case L_IO:
        lisa_io_write(m, r.phys, size, value);
        break;
    case L_MMUREG:
        lisa_mmureg_write(m, &r, (uint16_t)value);
        break;
    case L_ROM:
        return; // writes to ROM space are ignored
    case L_FAULT:
    default:
        lisa_raise_bus_error(addr, false, supervisor);
        return;
    }
    lisa_logpoint_notify(addr, size, value, true);
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

// === Object model: machine.cpu.mmu on the Lisa ==============================
//
// The segment MMU is real hardware with its own translation; before this node
// it had no object at all and the debugger showed every address as mapped to
// itself.  translate/peek have the same signatures and result shapes as every
// other MMU kind's.

// Instance data: the lisa_mmu_t.
static lisa_mmu_t *lisa_mmu_from(struct object *self) {
    return (lisa_mmu_t *)object_data(self);
}

// Read `start`: the power-on START/SETUP latch (translation bypassed).
static value_t lisa_attr_start(struct object *self, const member_t *mb) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    return m ? val_bool(m->start) : val_err("mmu not present");
}

// Read `context`: the user context the SEG1/SEG2 latches select (0-3).
static value_t lisa_attr_context(struct object *self, const member_t *mb) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    return m ? val_int((m->seg2 << 1) | m->seg1) : val_err("mmu not present");
}

// The Lisa's debugger translation (debug_mmu_xlate_fn).  `fetch` is
// accepted for the uniform signature; the segment MMU makes no
// instruction/data distinction.
static void lisa_xlate(void *ctx, uint32_t addr, bool supervisor, bool fetch, mmu_xlate_t *out, mmu_trace_t *trace) {
    (void)fetch;
    lisa_mmu_debug_translate((lisa_mmu_t *)ctx, addr, supervisor, out, trace);
}

// translate(addr, [supervisor], [fetch]) -> {phys, valid, via, access,
// space}: "segment" (or "identity" for the START-mode bypass), and which
// physical space the address lands in.
static value_t lisa_method_translate(struct object *self, const member_t *mb, int argc, const value_t *argv) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    if (!m)
        return val_err("mmu not present");
    return debug_mmu_translate(lisa_xlate, m, debug_cpu_is_supervisor(), argc, argv);
}

// walk(addr, [supervisor], [fetch]) -> translate's map plus the one step
// the segment MMU takes: the segment descriptor (SOR/SLR) of the context.
static value_t lisa_method_walk(struct object *self, const member_t *mb, int argc, const value_t *argv) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    if (!m)
        return val_err("mmu not present");
    return debug_mmu_walk(lisa_xlate, m, debug_cpu_is_supervisor(), argc, argv);
}

// map([start], [end], [supervisor], [fetch], [limit]) over the 24-bit
// logical space, at 512-byte page granularity.
static value_t lisa_method_map(struct object *self, const member_t *mb, int argc, const value_t *argv) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    if (!m)
        return val_err("mmu not present");
    return debug_mmu_map(lisa_xlate, m, debug_cpu_is_supervisor(), 1ull << 24, 9, argc, argv);
}

// descriptor(segment, [count], [context]) -> the segment descriptors, which
// live in the MMU's own descriptor RAM rather than in memory: each one's
// SOR (origin) and SLR (limit and access), decoded.  `context` defaults to
// the one the SEG1/SEG2 latches select.
static value_t lisa_method_descriptor(struct object *self, const member_t *mb, int argc, const value_t *argv) {
    (void)mb;
    lisa_mmu_t *m = lisa_mmu_from(self);
    if (!m)
        return val_err("mmu not present");
    uint64_t first = argv[0].u;
    if (first > 127)
        return val_err("descriptor: segment must be 0-127");
    int ctx = (argc > 2 && argv[2].kind == VK_UINT) ? (int)argv[2].u : ((m->seg2 << 1) | m->seg1);
    if (ctx < 0 || ctx > 3)
        return val_err("descriptor: context must be 0-3");
    uint32_t count = debug_mmu_desc_count(argc, argv);
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (uint32_t seg = (uint32_t)first; seg < 128 && seg < first + count; seg++) {
        uint16_t sor = m->sor[ctx][seg];
        uint16_t slr = m->slr[ctx][seg];
        int acc = (slr >> 8) & 0xF;
        bool valid = !(acc == ACC_INVALID || acc < ACC_MEM_RO_STK);
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "segment", val_uint(4, seg));
        val_map_put(b, "context", val_uint(4, (uint32_t)ctx));
        debug_mmu_put_hex(b, "base", seg << 17);
        debug_mmu_put_hex(b, "sor", sor);
        debug_mmu_put_hex(b, "slr", slr);
        val_map_put(b, "type", val_str(lisa_acc_name(acc)));
        if (valid) {
            val_map_put(b, "limit", val_uint(4, slr & 0xFF));
            if ((acc & 0xC) == 0x4) { // memory segments carry the stack and read-only bits
                debug_mmu_put_bool(b, "stack", !(acc & 0x1));
                debug_mmu_put_bool(b, "ro", !(acc & 0x2));
            }
            debug_mmu_put_hex(b, "phys", (uint32_t)(sor & 0xFFF) << 9); // physical page 0 of the segment
        }
        val_list_push(&items, &len, &cap, val_map_finish(b));
    }
    return val_list(items, len);
}

// peek(addr, [size], [space]) -> the value, big-endian, logical only: the
// Lisa has three physical spaces, and a bare physical address names none.
static value_t lisa_method_peek(struct object *self, const member_t *mb, int argc, const value_t *argv) {
    (void)mb;
    if (!lisa_mmu_from(self))
        return val_err("mmu not present");
    unsigned size = (argc >= 2 && argv[1].kind == VK_UINT) ? (unsigned)argv[1].u : 4;
    bool physical;
    if (!debug_parse_space(argc, argv, 2, &physical))
        return val_err("peek: space must be \"logical\" or \"physical\"");
    if (physical)
        return val_err("peek: the Lisa has three physical spaces (RAM, I/O, ROM); read a logical address");
    if (size != 1 && size != 2 && size != 4)
        return val_err("peek: size must be 1, 2 or 4");
    return val_uint((uint8_t)size, lisa_mmu_debug_read((uint32_t)argv[0].u, size, debug_cpu_is_supervisor()));
}

// peek's default size.
static const value_t k_peek_size4 = {.kind = VK_UINT, .u = 4};

static const arg_decl_t lisa_desc_args[] = {
    {.name = "segment", .kind = VK_UINT, .doc = "first segment number (0-127; logical address bits 23-17)"},
    {.name = "count",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &debug_mmu_desc_count_default,
     .doc = "how many consecutive segments"},
    {.name = "context",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "the context's descriptor set (0-3; supervisor mode uses 0)",
     .default_doc = "the context the SEG1/SEG2 latches select"},
};
static const arg_decl_t lisa_peek_args[] = {
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "logical address"},
    {.name = "size",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_peek_size4,
     .doc = "1, 2 or 4 bytes"},
    {.name = "space",
     .kind = VK_ENUM,
     .enum_values = debug_space_values,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "\"logical\"; \"physical\" is refused on the Lisa",
     .default_doc = "logical"},
};

static const member_t lisa_mmu_members[] = {
    {.kind = MK_ATTR,
     .name = "start",
     .doc = "START/SETUP latch: set at power-on, translation bypassed while set",
     .attr = {.type = VK_BOOL, .get = lisa_attr_start}},
    {.kind = MK_ATTR,
     .name = "context",
     .doc = "User context selected by the SEG1/SEG2 latches (0-3); supervisor mode always uses 0",
     .attr = {.type = VK_INT, .get = lisa_attr_context}},
    {.kind = MK_METHOD,
     .name = "translate",
     .examples = EXAMPLES("machine.cpu.mmu.translate 0x20000"),
     .doc = "Translate an address, side-effect-free (same shape on every MMU kind, plus the segment space)",
     .method = {.result_doc = "{phys, valid, via, access, space}",
                .args = debug_mmu_xlate_args,
                .nargs = DEBUG_MMU_XLATE_NARGS,
                .result = VK_MAP,
                .fn = lisa_method_translate}},
    {.kind = MK_METHOD,
     .name = "walk",
     .examples = EXAMPLES("machine.cpu.mmu.walk 0x20000"),
     .doc = "Translate an address and show the segment descriptor it went through",
     .method = {.result_doc = "{phys, valid, via, access, space, steps: [{step, outcome, ...}]}",
                .args = debug_mmu_xlate_args,
                .nargs = DEBUG_MMU_XLATE_NARGS,
                .result = VK_MAP,
                .fn = lisa_method_walk}},
    {.kind = MK_METHOD,
     .name = "map",
     .examples = EXAMPLES("machine.cpu.mmu.map", "machine.cpu.mmu.map supervisor=false"),
     .doc = "List the mapped address ranges: runs that translate linearly with the same via, access and space",
     .method = {.result_doc = "[{start, size, phys, via, access, space}]",
                .args = debug_mmu_map_args,
                .nargs = DEBUG_MMU_MAP_NARGS,
                .result = VK_LIST,
                .fn = lisa_method_map}},
    {.kind = MK_METHOD,
     .name = "descriptor",
     .examples = EXAMPLES("machine.cpu.mmu.descriptor 0 4", "machine.cpu.mmu.descriptor 127 1 0"),
     .doc = "Decode segment descriptors (SOR/SLR) from the MMU's descriptor RAM",
     .method = {.result_doc = "[{segment, context, base, sor, slr, type, limit?, stack?, ro?, phys?}]",
                .args = lisa_desc_args,
                .nargs = 3,
                .result = VK_LIST,
                .fn = lisa_method_descriptor}},
    {.kind = MK_METHOD,
     .name = "peek",
     .examples = EXAMPLES("machine.cpu.mmu.peek 0x20000"),
     .doc = "Read memory through the segment MMU; side-effect-free",
     .method = {.args = lisa_peek_args, .nargs = 3, .result = VK_UINT, .fn = lisa_method_peek}},
};

static const class_desc_t lisa_mmu_class = {
    .name = "lisa_mmu",
    .doc = "The Lisa's segment MMU: setup latch, context; translate, walk, map, descriptor and peek",
    .members = lisa_mmu_members,
    .n_members = sizeof(lisa_mmu_members) / sizeof(lisa_mmu_members[0]),
};

void lisa_mmu_attach_object(lisa_mmu_t *m, struct cpu *cpu) {
    cpu_attach_mmu_node(cpu, &lisa_mmu_class, m);
}
