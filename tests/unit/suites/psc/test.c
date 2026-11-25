// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// PSC DMA-engine unit test.
//
// Links the real av/psc.c against recording stubs and replays the three
// known-good client sequences verbatim from Apple's drivers:
//
//  1. The SCSI HAL's StartPSC / PausePSC / Wt4PSCComplete / StopPSCRead:
//     pause-then-FROZEN, active-set indexed arming, SENSE-bit CmdStat
//     writes, DMAFLUSH self-clear, writable residual Cnt.
//  2. The MACE driver's channel bring-up / per-set arm / ResetMACE:
//     CIE + IE interrupt gating into PSC_ISR (BFFFO bit order) and the
//     L4 DMA source, SWRESET disarming both sets.
//  3. The New Age driver's read dance: programming the *other* set
//     mid-transfer and the automatic set switch at terminal count.
//  4. The Ethernet channels as the AV Ethernet driver runs them: ENetRd's
//     chain mode (buffers, not bytes; status at +0, frame at +16; one IF
//     per frame) and ENetWr's one-frame-per-set double buffering.
//
// Plus a round trip of the PSC's checkpoint field list.

#include "av.h"
#include "psc.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
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

// --- checkpoint stream: a recording buffer, so the PSC's CP_FIELD list can
// be round-tripped (test_checkpoint_roundtrip) ---
static uint8_t s_cp_buf[4096];
static size_t s_cp_w, s_cp_r;

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp, (void)tag, (void)file, (void)line;
    ASSERT_TRUE(s_cp_w + size <= sizeof s_cp_buf);
    memcpy(s_cp_buf + s_cp_w, data, size);
    s_cp_w += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp, (void)tag, (void)file, (void)line;
    ASSERT_TRUE(s_cp_r + size <= s_cp_w);
    memcpy(data, s_cp_buf + s_cp_r, size);
    s_cp_r += size;
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
// Register access helpers (byte-lane shaped, like the board_io engine)
// ============================================================================

// Handler rows take (cfg, win_off, addr): the engine decodes the
// window-relative offset once and hands it over, instead of each handler
// re-deriving it from the island mirror mask by hand.  For this window win_off
// is exactly `off`.
#define PSC_BASE 0x50F31000u

static config_t s_cfg;
static av_state_t s_st;

static void w8(uint32_t off, uint8_t v) {
    av_psc_reg_write(&s_cfg, off, PSC_BASE + off, v);
}

static uint8_t r8(uint32_t off) {
    return av_psc_reg_read(&s_cfg, off, PSC_BASE + off);
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
// 6. The ROM's LocalTalk transmit (.MPP): one frame per SWRESET.  After a
//    frame's terminal count (which flips the active set and raises IF),
//    SENSE|SWRESET must leave the word reading exactly $C400 -- set 0, no
//    CIRQ -- or the driver spins on it forever before its next frame.
TEST(test_swreset_restarts_at_set_0) {
    for (int frame = 0; frame < 2; frame++) {
        w16(CTRL(5), HW_SENSE | HW_SWRESET);
        ASSERT_EQ_INT(r16(CTRL(5)), 0xC400);
        w32(SET(5, 0) + 4, 9);
        w32(SET(5, 0) + 0, 0x6000);
        w16(SET(5, 0) + 8, HW_DIR); // memory -> device
        w16(SET(5, 0) + 8, HW_IF);
        w16(SET(5, 0) + 8, HW_SENSE | HW_ENABLED);
        w16(CTRL(5), HW_PAUSE); // un-pause
        uint8_t out[9];
        ASSERT_EQ_INT(av_psc_dma_device_out(s_st.psc, 5, out, 9), 9);
        ASSERT_EQ_INT(r16(CTRL(5)) & (HW_KMSET | HW_CIRQ), HW_KMSET | HW_CIRQ); // set flipped, IF up
    }
}

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

// 7. The sound block's last longword: dspOverRun is lane 0 of $21C, and
// lanes 1-3 hold nothing.  They once read past the end of the sound-latch
// array into the next member.
TEST(test_dsp_overrun_lanes) {
    w8(0x21C, 0x87); // sense convention: set bits 0-2
    ASSERT_EQ_INT(r8(0x21C) & 0x07, 0x07);
    for (uint32_t off = 0x21D; off <= 0x21F; off++) {
        w8(off, 0xA5); // dropped
        ASSERT_EQ_INT(r8(off), 0);
    }
    w8(0x21C, 0x07); // clear them again
    ASSERT_EQ_INT(r8(0x21C) & 0x07, 0);
}

// 8. ENetRd chain mode (Mace.a InitRecv / PSCInterrupt / MaceRecv): Addr is
// the base of a chain of 2048-byte buffers and Cnt the number of buffers;
// each frame fills one -- the word-duplicated RCVFS status at +0, the frame
// at +16 -- steps Addr, drops Cnt and sets IF; the buffer that empties Cnt
// is the set's terminal count, and the next frame lands in the other set.
#define CHAIN0 0x8000u
#define CHAIN1 0xA000u

// InitRecv on set `s`: clear IF, the chain's base and Limit, then
// SENSE|IE|ENABLED -- DIR is never written on this channel.
static void init_recv(int s, uint32_t chain, uint32_t limit) {
    w16(SET(1, s) + 8, HW_IF);
    w32(SET(1, s) + 0, chain);
    w32(SET(1, s) + 4, limit);
    w16(SET(1, s) + 8, HW_SENSE | HW_IE | HW_ENABLED);
}

// One received frame of `len` bytes (pattern `seed`) through the chain.
static bool chain_frame(uint8_t seed, int len) {
    uint8_t frame[256], head[8];
    for (int i = 0; i < len; i++)
        frame[i] = (uint8_t)(seed + i);
    for (int i = 0; i < 4; i++)
        head[2 * i] = head[2 * i + 1] = (uint8_t)(i == 0 ? len : 0);
    return av_psc_dma_chain_in(s_st.psc, 1, head, 8, frame, len, 2048, 16);
}

TEST(test_enet_receive_chain_mode) {
    memset(s_mem, 0xEE, sizeof s_mem);
    w8(0x144, 0x88); // L4IER: DMA
    w16(CTRL(1), HW_SENSE | HW_SWRESET);
    w16(CTRL(1), HW_SENSE | HW_CIE); // InitRecvChl
    init_recv(0, CHAIN0, 2);
    init_recv(1, CHAIN1, 2);
    ASSERT_TRUE(!chain_frame(0x10, 83)); // still paused: nothing moves
    w16(CTRL(1), HW_PAUSE); // un-pause

    // Frame 1 into buffer 0 of set 0's chain.
    ASSERT_TRUE(chain_frame(0x10, 83));
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 0], 83); // RFS0 on both halves of the word
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 1], 83);
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 2], 0x00);
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 8], 0xEE); // the fill granule is not written
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 16], 0x10); // the frame from +16
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 16 + 82], (uint8_t)(0x10 + 82));
    ASSERT_EQ_INT((int)r32(SET(1, 0) + 4), 1); // one buffer left: MaceRecv's In = Limit - Cnt = 1
    ASSERT_EQ_HEX(r32(SET(1, 0) + 0), CHAIN0 + 2048);
    ASSERT_TRUE(r16(SET(1, 0) + 8) & HW_IF); // every frame interrupts
    ASSERT_TRUE(!(r16(SET(1, 0) + 8) & HW_TERMCNT));
    ASSERT_TRUE(r32(0x804) & (1u << 30)); // PSC_ISR: channel 1
    ASSERT_EQ_INT(s_l4_active, 1);

    // PSCInterrupt, chain not full: clear the set's IF and IE; a frame
    // arriving while the deferred task runs sets IF without interrupting.
    w16(SET(1, 0) + 8, HW_IF | HW_IE);
    ASSERT_EQ_INT(s_l4_active, 0);
    ASSERT_TRUE(chain_frame(0x40, 64)); // buffer 1: the set's terminal count
    ASSERT_EQ_HEX(s_mem[CHAIN0 + 2048 + 16], 0x40);
    ASSERT_EQ_INT(s_l4_active, 0); // IE is off
    uint16_t cs = r16(SET(1, 0) + 8);
    ASSERT_TRUE((cs & (HW_IF | HW_TERMCNT)) == (HW_IF | HW_TERMCNT));
    ASSERT_TRUE(!(cs & HW_ENABLED));
    ASSERT_EQ_INT(r16(CTRL(1)) & HW_KMSET, 1); // the PSC switched to set 1
    // MaceRecv's exit re-enables IE: the pending IF interrupts at once.
    w16(SET(1, 0) + 8, HW_SENSE | HW_IE);
    ASSERT_EQ_INT(s_l4_active, 1);
    // PSCInterrupt finds Cnt 0 (chain full), PrimeRecv re-primes set 0.
    ASSERT_EQ_INT((int)r32(SET(1, 0) + 4), 0);
    init_recv(0, CHAIN0, 2);
    ASSERT_EQ_INT(s_l4_active, 0);

    // The next frame lands in set 1's chain.
    ASSERT_TRUE(chain_frame(0x70, 100));
    ASSERT_EQ_HEX(s_mem[CHAIN1 + 16], 0x70);
    ASSERT_EQ_INT((int)r32(SET(1, 1) + 4), 1);
    ASSERT_TRUE(r32(0x804) & (1u << 30));
    w16(SET(1, 1) + 8, HW_IF | HW_IE);

    // ResetMACE tears the channel down again.
    w16(CTRL(1), HW_SENSE | HW_SWRESET);
    w16(CTRL(1), HW_CIE);
    w16(SET(1, 0) + 8, HW_IE | HW_IF);
    w16(SET(1, 1) + 8, HW_IE | HW_IF);
    ASSERT_TRUE(!chain_frame(0x10, 60));
}

// 9. ENetWr (PrimeXmitRS): one frame per set, Cnt in bytes, both sets kept
// armed; each set's terminal count completes it and flips to the other.
TEST(test_enet_transmit_both_sets) {
    memcpy(&s_mem[0x9000], "FRAME-ZERO......", 16);
    memcpy(&s_mem[0x9800], "FRAME-ONE.......", 16);
    w16(CTRL(2), HW_SENSE | HW_SWRESET);
    w16(CTRL(2), HW_SENSE | HW_CIE);
    w16(CTRL(2), HW_PAUSE);
    for (int s = 0; s < 2; s++) {
        w32(SET(2, s) + 0, s ? 0x9800 : 0x9000);
        w32(SET(2, s) + 4, 16);
        w16(SET(2, s) + 8, HW_SENSE | HW_IE | HW_ENABLED);
    }
    uint8_t out[16];
    ASSERT_EQ_INT((int)av_psc_dma_remaining(s_st.psc, 2), 16);
    ASSERT_EQ_INT(av_psc_dma_device_out(s_st.psc, 2, out, 16), 16);
    ASSERT_EQ_INT(memcmp(out, "FRAME-ZERO......", 16), 0);
    ASSERT_TRUE(r32(0x804) & (1u << 29)); // PSC_ISR: channel 2
    ASSERT_EQ_INT(av_psc_dma_device_out(s_st.psc, 2, out, 16), 16);
    ASSERT_EQ_INT(memcmp(out, "FRAME-ONE.......", 16), 0);
    // PSCInterrupt's ChkXmitDone clears IF set by set.
    w16(SET(2, 0) + 8, HW_IF);
    ASSERT_TRUE(r32(0x804) & (1u << 29));
    w16(SET(2, 1) + 8, HW_IF);
    ASSERT_TRUE(!(r32(0x804) & (1u << 29)));
    ASSERT_EQ_INT((int)av_psc_dma_remaining(s_st.psc, 2), 0);
}

// 10. Writes to the Ethernet channels' registers wake the Ethernet pump;
// the other channels' do not.
static int s_enet_touches;
static void enet_touch(void *ctx) {
    (void)ctx;
    s_enet_touches++;
}

TEST(test_enet_touch_hook) {
    av_psc_set_enet_touch_hook(s_st.psc, enet_touch, NULL);
    s_enet_touches = 0;
    w16(CTRL(3), HW_PAUSE);
    w32(SET(0, 0) + 4, 0);
    ASSERT_EQ_INT(s_enet_touches, 0);
    w16(CTRL(1), HW_PAUSE); // two byte lanes, two touches
    ASSERT_EQ_INT(s_enet_touches, 2);
    w8(SET(2, 1) + 8, 0x80);
    ASSERT_EQ_INT(s_enet_touches, 3);
    av_psc_set_enet_touch_hook(s_st.psc, NULL, NULL);
}

// 11. The PSC's state round-trips through its CP_FIELD list: the interrupt
// files and every channel's sets, active set and control bits.
TEST(test_checkpoint_roundtrip) {
    w8(0x134, 0x81); // L3IER: MACE
    w16(CTRL(1), HW_SENSE | HW_SWRESET);
    w16(CTRL(1), HW_SENSE | HW_CIE);
    init_recv(0, CHAIN0, 3);
    init_recv(1, CHAIN1, 4);
    w16(CTRL(1), HW_PAUSE);
    ASSERT_TRUE(chain_frame(0x22, 70));
    av_psc_level_source(s_st.psc, AV_PSC_L3, 0, true);

    s_cp_w = s_cp_r = 0;
    av_psc_checkpoint(s_st.psc, (checkpoint_t *)1);
    av_psc_t *live = s_st.psc;
    s_st.psc = av_psc_init(&s_cfg, (checkpoint_t *)1);
    ASSERT_EQ_INT((int)s_cp_r, (int)s_cp_w);
    ASSERT_EQ_HEX(r8(0x134), 0x81);
    ASSERT_EQ_HEX(r8(0x130), 0x81); // MACE pending on L3
    ASSERT_EQ_HEX(r32(SET(1, 0) + 0), CHAIN0 + 2048);
    ASSERT_EQ_INT((int)r32(SET(1, 0) + 4), 2);
    ASSERT_EQ_HEX(r32(SET(1, 1) + 0), CHAIN1);
    ASSERT_EQ_INT((int)r32(SET(1, 1) + 4), 4);
    ASSERT_EQ_HEX(r16(SET(1, 0) + 8) & (HW_IF | HW_IE | HW_ENABLED), HW_IF | HW_IE | HW_ENABLED);
    ASSERT_EQ_HEX(r16(CTRL(1)) & (HW_CIE | HW_PAUSE | HW_KMSET | HW_CIRQ), HW_CIE | HW_CIRQ);
    av_psc_delete(s_st.psc);
    s_st.psc = live;
    av_psc_level_source(s_st.psc, AV_PSC_L3, 0, false);
}

int main(void) {
    s_cfg.machine_context = &s_st;
    s_st.psc = av_psc_init(&s_cfg, NULL);
    ASSERT_TRUE(s_st.psc != NULL);
    static const dma_mem_port_t port = {.read = mem_read, .write = mem_write};
    av_psc_set_memory_port(s_st.psc, &port);

    RUN(test_scsi_start_and_complete);
    RUN(test_scsi_stop_read);
    RUN(test_mace_isr_and_reset);
    RUN(test_fdc_double_buffer);
    RUN(test_swreset_restarts_at_set_0);
    RUN(test_write_direction);
    RUN(test_no_dma_above_rom_base);
    RUN(test_dsp_overrun_lanes);
    RUN(test_enet_receive_chain_mode);
    RUN(test_enet_transmit_both_sets);
    RUN(test_enet_touch_hook);
    RUN(test_checkpoint_roundtrip);

    fprintf(stderr, "psc: all tests passed\n");
    return 0;
}
