// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// PSC DMA-engine unit test (proposal-quadra-av.md Phase D).
//
// Links the real av/psc.c against recording stubs and replays the three
// known-good client sequences the dossier quotes verbatim from Apple's
// drivers (psc.md §3):
//
//  1. The SCSI HAL's StartPSC / PausePSC / Wt4PSCComplete / StopPSCRead:
//     pause-then-FROZEN, active-set indexed arming, SENSE-bit CmdStat
//     writes, DMAFLUSH self-clear, writable residual Cnt.
//  2. The MACE driver's channel bring-up / per-set arm / ResetMACE:
//     CIE + IE interrupt gating into PSC_ISR (BFFFO bit order) and the
//     L4 DMA source, SWRESET disarming both sets.
//  3. The New Age driver's read dance: programming the *other* set
//     mid-transfer and the automatic set switch at terminal count.

#include "av.h"
#include "psc.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording stubs
// ============================================================================

// --- av_update_ipl: record the last L4 state the engine derived ---
static int s_l4_active = -1; // -1 = never called for L4
void av_update_ipl(config_t *cfg, int source, bool active) {
    (void)cfg;
    if (source == AV_IRQ_L4)
        s_l4_active = active ? 1 : 0;
}

// --- cpu_get_pc: log-line decoration only ---
uint32_t cpu_get_pc(cpu_t *cpu) {
    (void)cpu;
    return 0;
}

// --- scheduler_time_ns: drives sndPhase/UTSC (unused by the DMA tests) ---
double scheduler_time_ns(struct scheduler *sched) {
    (void)sched;
    return 0.0;
}

// --- guest-physical memory: a flat 64 KB array ---
static uint8_t s_mem[0x10000];

static uint32_t mem_read(void *ctx, uint32_t phys, unsigned width) {
    (void)ctx;
    (void)width;
    return s_mem[phys & 0xFFFF];
}

static void mem_write(void *ctx, uint32_t phys, uint32_t value, unsigned width) {
    (void)ctx;
    (void)width;
    s_mem[phys & 0xFFFF] = (uint8_t)value;
}

// ============================================================================
// Register access helpers (byte-lane shaped, like the mac030 engine)
// ============================================================================

#define PSC_BASE 0x50F31000u

static config_t s_cfg;
static av_state_t s_st;

static void w8(uint32_t off, uint8_t v) {
    av_psc_reg_write(&s_cfg, PSC_BASE + off, v);
}

static uint8_t r8(uint32_t off) {
    return av_psc_reg_read(&s_cfg, PSC_BASE + off);
}

static void w16(uint32_t off, uint16_t v) {
    w8(off, (uint8_t)(v >> 8));
    w8(off + 1, (uint8_t)v);
}

static uint16_t r16(uint32_t off) {
    return (uint16_t)((r8(off) << 8) | r8(off + 1));
}

static void w32(uint32_t off, uint32_t v) {
    for (int i = 0; i < 4; i++)
        w8(off + (uint32_t)i, (uint8_t)(v >> (8 * (3 - i))));
}

static uint32_t r32(uint32_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | r8(off + (uint32_t)i);
    return v;
}

// Hardware bit values (as the drivers write them).
#define CTRL(n)   (0xC00u + (uint32_t)(n) * 0x10u)
#define SET(n, s) (0x1000u + (uint32_t)(n) * 0x20u + (uint32_t)(s) * 0x10u)

#define HW_SENSE    0x8000
#define HW_CIRQ     0x0100
#define HW_DMAFLUSH 0x0200
#define HW_PAUSE    0x0400
#define HW_SWRESET  0x0800
#define HW_CIE      0x1000
#define HW_BERR     0x2000
#define HW_FROZEN   0x4000
#define HW_KMSET    0x0001

#define HW_IF      0x0100
#define HW_DIR     0x0200
#define HW_TERMCNT 0x0400
#define HW_ENABLED 0x0800
#define HW_IE      0x1000

// The SCSI HAL's PausePSC: SENSE|PAUSE, then spin until FROZEN reads 1.
static void pause_psc(int n) {
    w16(CTRL(n), HW_SENSE | HW_PAUSE);
    int spins = 0;
    while (!(r16(CTRL(n)) & HW_FROZEN)) {
        ASSERT_TRUE(++spins < 10); // must assert promptly or drivers hang
    }
}

// The SCSI HAL's StartPSC: arm the ACTIVE set for a device→memory read.
static int start_psc_read(int n, uint32_t addr, uint32_t cnt) {
    pause_psc(n);
    int s = r16(CTRL(n)) & HW_KMSET;
    w32(SET(n, s) + 4, cnt);
    w32(SET(n, s) + 0, addr);
    w16(SET(n, s) + 8, HW_SENSE | HW_DIR); // read in: SENSE=1 sets DIR
    w16(SET(n, s) + 8, HW_IF); // SENSE=0 clears the interrupt flag
    w16(SET(n, s) + 8, HW_SENSE | HW_ENABLED); // arm (IE deliberately NOT set)
    w16(CTRL(n), HW_PAUSE); // SENSE=0 un-pauses
    return s;
}

// ============================================================================
// Tests
// ============================================================================

// 1. StartPSC → full transfer → Wt4PSCComplete semantics.
TEST(test_scsi_start_and_complete) {
    int s = start_psc_read(0, 0x1000, 32);
    ASSERT_TRUE(av_psc_dma_ready(s_st.psc, 0));

    uint8_t data[32];
    for (int i = 0; i < 32; i++)
        data[i] = (uint8_t)(0xA0 + i);
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 0, data, 32), 32);
    ASSERT_EQ_INT(memcmp(&s_mem[0x1000], data, 32), 0);

    // Completion: IF + TERMCNT + CIRQ set, ENABLED cleared, set switched.
    uint16_t cs = r16(SET(0, s) + 8);
    ASSERT_TRUE(cs & HW_IF);
    ASSERT_TRUE(cs & HW_TERMCNT);
    ASSERT_TRUE(!(cs & HW_ENABLED));
    ASSERT_TRUE(r16(CTRL(0)) & HW_CIRQ);
    ASSERT_EQ_INT(r16(CTRL(0)) & HW_KMSET, s ^ 1);
    // Residual is zero; the HAL then clears IF with a SENSE=0 write.
    ASSERT_EQ_INT((int)r32(SET(0, s) + 4), 0);
    w16(SET(0, s) + 8, HW_IF);
    ASSERT_TRUE(!(r16(CTRL(0)) & HW_CIRQ));
}

// 2. StopPSCRead: partial transfer, DMAFLUSH self-clears, residual Cnt
//    readable and writable-to-zero in a verify loop.
TEST(test_scsi_stop_read) {
    int s = start_psc_read(0, 0x2000, 64);
    uint8_t data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 0, data, 16), 16);

    // TERMCNT not set → flush, poll until bit 9 self-clears.
    ASSERT_TRUE(!(r16(SET(0, s) + 8) & HW_TERMCNT));
    w16(CTRL(0), HW_SENSE | HW_DMAFLUSH);
    ASSERT_TRUE(!(r16(CTRL(0)) & HW_DMAFLUSH));

    pause_psc(0);
    ASSERT_EQ_INT((int)r32(SET(0, s) + 4), 48); // residual
    // The HAL zeroes Cnt in a read-verify loop.
    w32(SET(0, s) + 4, 0);
    ASSERT_EQ_INT((int)r32(SET(0, s) + 4), 0);
    // Disarm for the next test.
    w16(SET(0, s) + 8, HW_ENABLED);
}

// 3. MACE bring-up: CIE + per-set IE gate completions into PSC_ISR
//    (bit 31−n) and the L4 DMA source; ResetMACE disarms both sets.
TEST(test_mace_isr_and_reset) {
    s_l4_active = -1;

    // MaceInit enables the L4 DMA gateway: L4IER := (1<<L4B7)+(1<<DMA).
    w8(0x144, 0x88);

    // Channel bring-up: SENSE|CIE, then un-pause (Mace.a InitRecvChl).
    w16(CTRL(1), HW_SENSE | HW_CIE);
    w16(CTRL(1), HW_PAUSE);

    int s = r16(CTRL(1)) & HW_KMSET;
    w32(SET(1, s) + 0, 0x3000);
    w32(SET(1, s) + 4, 16);
    w16(SET(1, s) + 8, HW_IF); // clear IF
    w16(SET(1, s) + 8, HW_SENSE | HW_IE | HW_ENABLED | HW_DIR); // per-set arm

    uint8_t frame[16] = {0};
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 1, frame, 16), 16);

    // Channel 1 → PSC_ISR bit 30; L4 DMA source raised.
    ASSERT_TRUE(r32(0x804) & (1u << 30));
    ASSERT_EQ_INT(s_l4_active, 1);

    // ResetMACE: SENSE|SWRESET → paused + both sets disarmed; then clear
    // CIE and IE+IF on both CmdStats.
    w16(CTRL(1), HW_SENSE | HW_SWRESET);
    ASSERT_TRUE(!(r16(SET(1, 0) + 8) & HW_ENABLED));
    ASSERT_TRUE(!(r16(SET(1, 1) + 8) & HW_ENABLED));
    ASSERT_TRUE(r16(CTRL(1)) & HW_FROZEN);
    w16(CTRL(1), HW_CIE);
    w16(SET(1, 0) + 8, HW_IE | HW_IF);
    w16(SET(1, 1) + 8, HW_IE | HW_IF);
    ASSERT_TRUE(!(r32(0x804) & (1u << 30)));
    ASSERT_EQ_INT(s_l4_active, 0);
}

// 4. The New Age dance: read the active-set bit, program the OTHER set
//    mid-transfer, and stream across the automatic set switch.
TEST(test_fdc_double_buffer) {
    pause_psc(3);
    int s = r16(CTRL(3)) & HW_KMSET;

    // Prime the active set (16 bytes at $4000)…
    w32(SET(3, s) + 0, 0x4000);
    w32(SET(3, s) + 4, 16);
    w16(SET(3, s) + 8, HW_IF);
    w16(SET(3, s) + 8, HW_SENSE | HW_DIR | HW_ENABLED);
    // …and the other set (16 bytes at $5000) — the driver's Bchg #4 path.
    w32(SET(3, s ^ 1) + 0, 0x5000);
    w32(SET(3, s ^ 1) + 4, 16);
    w16(SET(3, s ^ 1) + 8, HW_IF);
    w16(SET(3, s ^ 1) + 8, HW_SENSE | HW_DIR | HW_ENABLED);
    w16(CTRL(3), HW_PAUSE); // un-pause

    uint8_t chunk[32];
    for (int i = 0; i < 32; i++)
        chunk[i] = (uint8_t)i;
    // First 16 bytes land in set s's buffer, and the engine flips sets…
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 3, chunk, 16), 16);
    ASSERT_EQ_INT(r16(CTRL(3)) & HW_KMSET, s ^ 1);
    // …so the next 16 continue seamlessly into the other buffer.
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 3, chunk + 16, 16), 16);
    ASSERT_EQ_INT(memcmp(&s_mem[0x4000], chunk, 16), 0);
    ASSERT_EQ_INT(memcmp(&s_mem[0x5000], chunk + 16, 16), 0);
}

// 5. Memory→device direction, and the direction gate.
TEST(test_write_direction) {
    memcpy(&s_mem[0x6000], "PSC WRITE DATA!!", 16);
    pause_psc(0);
    int s = r16(CTRL(0)) & HW_KMSET;
    w32(SET(0, s) + 0, 0x6000);
    w32(SET(0, s) + 4, 16);
    w16(SET(0, s) + 8, HW_DIR); // SENSE=0 clears DIR (memory→device)
    w16(SET(0, s) + 8, HW_IF);
    w16(SET(0, s) + 8, HW_SENSE | HW_ENABLED);
    w16(CTRL(0), HW_PAUSE);

    uint8_t out[16] = {0};
    // A device_in against a write-armed set must move nothing.
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 0, out, 16), 0);
    ASSERT_EQ_INT(av_psc_dma_device_out(s_st.psc, 0, out, 16), 16);
    ASSERT_EQ_INT(memcmp(out, "PSC WRITE DATA!!", 16), 0);
    ASSERT_TRUE(r16(SET(0, s) + 8) & HW_TERMCNT);
    w16(SET(0, s) + 8, HW_IF);
}

// 6. The ≥$40000000 restriction latches BERR instead of transferring.
TEST(test_no_dma_above_rom_base) {
    pause_psc(0);
    int s = r16(CTRL(0)) & HW_KMSET;
    w32(SET(0, s) + 0, 0x40000000u);
    w32(SET(0, s) + 4, 16);
    w16(SET(0, s) + 8, HW_SENSE | HW_DIR);
    w16(SET(0, s) + 8, HW_SENSE | HW_ENABLED);
    w16(CTRL(0), HW_PAUSE);

    uint8_t data[16] = {0};
    ASSERT_EQ_INT(av_psc_dma_device_in(s_st.psc, 0, data, 16), 0);
    ASSERT_TRUE(r16(CTRL(0)) & HW_BERR);
    // Recovery is SENSE|SWRESET, then the BERR bit clears with SENSE=0.
    w16(CTRL(0), HW_SENSE | HW_SWRESET);
    w16(CTRL(0), HW_BERR);
    ASSERT_TRUE(!(r16(CTRL(0)) & HW_BERR));
}

int main(void) {
    s_cfg.machine_context = &s_st;
    s_st.psc = av_psc_init(&s_cfg, NULL);
    ASSERT_TRUE(s_st.psc != NULL);
    av_psc_set_memory_hooks(s_st.psc, mem_read, mem_write, NULL);

    RUN(test_scsi_start_and_complete);
    RUN(test_scsi_stop_read);
    RUN(test_mace_isr_and_reset);
    RUN(test_fdc_double_buffer);
    RUN(test_write_direction);
    RUN(test_no_dma_above_rom_base);

    fprintf(stderr, "psc: all tests passed\n");
    return 0;
}
