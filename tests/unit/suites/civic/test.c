// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// CIVIC serial-register + sense unit test (proposal-quadra-av.md Phase F).
//
// Links the real av/civic.c against recording stubs and pins the contracts
// from local/gs-docs/840av_660av/docs/civic.md and sebastian.md:
//
//  1. The bit-serial register codec: one bit per longword, only D[0]
//     significant, LSB at the lowest address, stride 4.  Writes stream
//     LSB->MSB ascending (dWriteCivic); reads walk MSB->LSB descending
//     from base + width*4 (dReadCivic).  Round-trips a 12-bit register.
//  2. The five 1-bit registers that are ALSO poked as plain longwords.
//  3. Inverted / computed slots: SyncClr reads inverted, VDCInt reads 1
//     (active-low idle), CntTest is a side-effect-free settle delay.
//  4. The monitor-sense drive/read protocol: the documented drive
//     patterns (CivicResetSenseLines / DriveA / DriveB / DriveC) against
//     the attached Hi-Res monitor's indexed code 6.
//  5. Sebastian: R,G,B,alpha quads with auto-increment after the 4th
//     byte, two CLUT banks selected by PCBR bit 6, and the depth code.
//  6. The VBL ack dance (VBLClr 0 then 1) and its PSC SInt bit 6 line.

#include "av.h"
#include "civic.h"
#include "psc.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording stubs
// ============================================================================

// --- the PSC slot-interrupt line CIVIC's VBL drives ---
static int s_slot_vbl = -1; // -1 = never driven
void av_psc_slot_source(av_psc_t *psc, int bit, bool active) {
    (void)psc;
    if (bit == AV_PSC_SINT_VBL)
        s_slot_vbl = active ? 1 : 0;
}

uint32_t cpu_get_pc(cpu_t *cpu) {
    (void)cpu;
    return 0;
}

// --- the video digitizer's clock-gate hook (st->vdc stays NULL here, but
// the symbol must resolve; the vdc suite covers the real interplay) ---
void av_vdc_clock_gate(struct av_vdc *vdc, bool clock_off) {
    (void)vdc;
    (void)clock_off;
}

// --- scheduler: capture the frame event so tests can tick it manually ---
static void (*s_frame_cb)(void *, uint64_t);
static void *s_frame_src;

event_t *scheduler_new_cpu_event(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)sch;
    (void)data;
    (void)cycles;
    (void)ns;
    s_frame_cb = (void (*)(void *, uint64_t))callback;
    s_frame_src = source;
    return NULL;
}
void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
    s_frame_cb = NULL;
}
void scheduler_new_event_type(scheduler_t *sch, const char *owner, void *source, const char *name,
                              event_callback_t callback) {
    (void)sch;
    (void)owner;
    (void)source;
    (void)name;
    (void)callback;
}

// --- memory: civic installs VRAM pages + the low alias; not exercised here
// (the page-table globals come from support/stub_memory.c) ---

void mac030_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    (void)page_index;
    (void)host_ptr;
    (void)writable;
}
void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host, uint32_t base, uint32_t size,
                            bool writable) {
    (void)m;
    (void)name;
    (void)host;
    (void)base;
    (void)size;
    (void)writable;
}
void memory_map_add(memory_map_t *m, uint32_t base, uint32_t size, const char *name, memory_interface_t *iface,
                    void *ctx) {
    (void)m;
    (void)base;
    (void)size;
    (void)name;
    (void)iface;
    (void)ctx;
}

// ============================================================================
// Register access helpers
// ============================================================================

#define CIVIC_BASE 0x50F36000u

static config_t s_cfg;
static av_state_t s_st;

// One longword slot: only byte lane 3 carries D[0].
static void slot_write(uint32_t off, uint32_t value) {
    av_civic_write(&s_cfg, CIVIC_BASE + off + 3, (uint8_t)(value & 1));
}

static uint32_t slot_read(uint32_t off) {
    return av_civic_read(&s_cfg, CIVIC_BASE + off + 3) & 1;
}

// dWriteCivic: stream the bits LSB->MSB ascending from the register base.
static void civic_write_reg(uint32_t off, int width, uint32_t value) {
    for (int i = 0; i < width; i++) {
        slot_write(off + (uint32_t)i * 4, value & 1);
        value >>= 1;
    }
}

// dReadCivic: walk MSB->LSB descending from base + width*4.
static uint32_t civic_read_reg(uint32_t off, int width) {
    uint32_t v = 0;
    for (int i = width - 1; i >= 0; i--)
        v = (v << 1) | slot_read(off + (uint32_t)i * 4);
    return v;
}

// Hardware offsets (civic.md §3).
#define R_VBLINT    0x000
#define R_ENABLE    0x004
#define R_VDCINT    0x008
#define R_SENSE0    0x05C
#define R_SENSE1    0x060
#define R_SENSE2    0x064
#define R_SYNCCLR   0x06C
#define R_READSENSE 0x080
#define R_ROWWORDS  0x08C
#define R_BASEADDR  0x0C0
#define R_RESET     0x10C
#define R_VBLENB    0x110
#define R_VBLCLR    0x120
#define R_CNTTEST   0x140
#define R_HAL       0x380

// Sebastian ($10 stride).
#define SEB_BASE 0x50F30800u
#define SEB_ADDR 0x00
#define SEB_DATA 0x10
#define SEB_PCBR 0x20

static void seb_write(uint32_t reg, uint8_t v) {
    av_civic_seb_write(&s_cfg, SEB_BASE + reg, v);
}

static uint8_t seb_read(uint32_t reg) {
    return av_civic_seb_read(&s_cfg, SEB_BASE + reg);
}

// ============================================================================
// Tests
// ============================================================================

// 1. The bit-serial codec round-trips a 12-bit register, and each bit
//    really does live in its own longword (HAL at $380 spans $380..$3AC).
TEST(test_serial_codec) {
    civic_write_reg(R_HAL, 12, 0x5A5);
    ASSERT_EQ_INT((int)civic_read_reg(R_HAL, 12), 0x5A5);

    // Bit 0 is the lowest address, bit 11 the highest: check placement.
    ASSERT_EQ_INT((int)slot_read(R_HAL + 0), 1); // $5A5 bit 0 = 1
    ASSERT_EQ_INT((int)slot_read(R_HAL + 4), 0); // bit 1 = 0
    ASSERT_EQ_INT((int)slot_read(R_HAL + 11 * 4), 0); // bit 11 = 0
    ASSERT_EQ_INT((int)slot_read(R_HAL + 10 * 4), 1); // bit 10 = 1

    // An 8-bit register is independent of its neighbours.
    civic_write_reg(R_ROWWORDS, 8, 32);
    ASSERT_EQ_INT((int)civic_read_reg(R_ROWWORDS, 8), 32);
    ASSERT_EQ_INT((int)civic_read_reg(R_HAL, 12), 0x5A5);

    // Only D[0] of the datum is wired: a write of $FE stores a 0 bit.
    av_civic_write(&s_cfg, CIVIC_BASE + R_HAL + 3, 0xFE);
    ASSERT_EQ_INT((int)slot_read(R_HAL + 0), 0);
}

// 2. Direct longword pokes of the five 1-bit registers reach the same
//    state as the serial path.
TEST(test_direct_registers) {
    slot_write(R_ENABLE, 1);
    ASSERT_EQ_INT((int)civic_read_reg(R_ENABLE, 1), 1);
    slot_write(R_RESET, 1);
    slot_write(R_RESET, 0);
    ASSERT_EQ_INT((int)slot_read(R_RESET), 0);
}

// 3. Inverted / computed slots.
TEST(test_computed_slots) {
    // SyncClr reads INVERTED (the driver's open treats 0 as "composite").
    slot_write(R_SYNCCLR, 1);
    ASSERT_EQ_INT((int)slot_read(R_SYNCCLR), 0);
    slot_write(R_SYNCCLR, 0);
    ASSERT_EQ_INT((int)slot_read(R_SYNCCLR), 1);

    // VDCInt is active LOW: 1 means "no video-in interrupt pending", so
    // the enabler's vdig never opens.
    ASSERT_EQ_INT((int)slot_read(R_VDCINT), 1);

    // CntTest reads are a pure settle delay — no side effects, and three
    // reads in a row are identical (the CivicResetDelay macro).
    ASSERT_EQ_INT((int)civic_read_reg(R_CNTTEST, 12), 0);
    ASSERT_EQ_INT((int)civic_read_reg(R_CNTTEST, 12), 0);
    ASSERT_EQ_INT((int)civic_read_reg(R_CNTTEST, 12), 0);
}

// 4. The sense protocol: the documented drive patterns against the
//    attached Hi-Res monitor (indexed code 6 = %110).
TEST(test_monitor_sense) {
    // CivicResetSenseLines: SenseCtl=0, Sense2=0, Sense1=0, Sense0=1.
    slot_write(R_SENSE2, 0);
    slot_write(R_SENSE1, 0);
    slot_write(R_SENSE0, 1);
    // Line C (bit 0) is driven low by the host; the monitor ties bit 0 low
    // anyway.  Lines A/B float to the monitor code's 1 bits.
    ASSERT_EQ_INT((int)civic_read_reg(R_READSENSE, 3), 6);

    // CivicDriveA: Sense2=1, Sense1=0, Sense0=1 — line A pulled low.
    slot_write(R_SENSE2, 1);
    slot_write(R_SENSE1, 0);
    slot_write(R_SENSE0, 1);
    ASSERT_EQ_INT((int)civic_read_reg(R_READSENSE, 3), 2);

    // CivicDriveB: 0,1,1 — line B pulled low.
    slot_write(R_SENSE2, 0);
    slot_write(R_SENSE1, 1);
    slot_write(R_SENSE0, 1);
    ASSERT_EQ_INT((int)civic_read_reg(R_READSENSE, 3), 4);

    // CivicDriveC: 0,0,0 — nothing driven; the bare monitor code shows.
    slot_write(R_SENSE2, 0);
    slot_write(R_SENSE1, 0);
    slot_write(R_SENSE0, 0);
    ASSERT_EQ_INT((int)civic_read_reg(R_READSENSE, 3), 6);
}

// 5. Sebastian: index + R,G,B,alpha with auto-increment; PCBR bit 6
//    selects the CLUT bank; the depth code drives the display format.
TEST(test_sebastian_clut) {
    seb_write(SEB_PCBR, 0x13); // graphics bank, depth code 3 = 8 bpp
    seb_write(SEB_ADDR, 10);
    seb_write(SEB_DATA, 0x11); // R
    seb_write(SEB_DATA, 0x22); // G
    seb_write(SEB_DATA, 0x33); // B
    seb_write(SEB_DATA, 0x44); // alpha — address auto-increments here
    // The next quad lands in entry 11.
    seb_write(SEB_DATA, 0xAA);
    seb_write(SEB_DATA, 0xBB);
    seb_write(SEB_DATA, 0xCC);
    seb_write(SEB_DATA, 0xDD);
    ASSERT_EQ_INT(seb_read(SEB_ADDR), 12);

    // Read entry 10 back through the same address/data pair.
    seb_write(SEB_ADDR, 10);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0x11);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0x22);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0x33);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0x44);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0xAA); // rolled into entry 11

    // Bank select: PCBR bit 6 switches to the video-in CLUT, which is a
    // different store — entry 10 there is still zero.
    seb_write(SEB_PCBR, 0x53); // bit 6 set
    seb_write(SEB_ADDR, 10);
    ASSERT_EQ_INT(seb_read(SEB_DATA), 0x00);

    // The display follows the depth code: 8 bpp indexed with a 256-entry
    // CLUT at 640x480.  test_serial_codec left RowWords = 32, so the row
    // pitch is 32 * 32 = 1024 bytes — NOT the 640 the visible width would
    // imply (see test_stride_follows_rowwords).
    seb_write(SEB_PCBR, 0x13);
    display_t *d = av_civic_display(s_st.civic);
    ASSERT_EQ_INT((int)d->width, 640);
    ASSERT_EQ_INT((int)d->height, 480);
    ASSERT_EQ_INT((int)d->format, PIXEL_8BPP);
    ASSERT_EQ_INT((int)d->stride, 1024);
    ASSERT_EQ_INT((int)d->clut_len, 256);

    // 1 bpp: two CLUT entries — which the driver writes at $7F (white) and
    // $FF (black), the documented start/skip placement.  The pitch is still
    // the programmed 1024; CIVIC's row pitch does not shrink with depth.
    seb_write(SEB_PCBR, 0x10); // depth code 0
    ASSERT_EQ_INT((int)d->format, PIXEL_1BPP_MSB);
    ASSERT_EQ_INT((int)d->stride, 1024);
    ASSERT_EQ_INT((int)d->clut_len, 2);

    // 32 bpp needs 2560 bytes for 640 visible pixels — more than RowWords
    // describes — so the packed row wins and the descriptor stays coherent.
    seb_write(SEB_PCBR, 0x05); // depth code 5 = 32 bpp, direct
    ASSERT_EQ_INT((int)d->format, PIXEL_32BPP_XRGB);
    ASSERT_EQ_INT((int)d->stride, 2560);
    ASSERT_EQ_INT((int)d->clut_len, 0);
    seb_write(SEB_PCBR, 0x13); // back to 8 bpp for the following rows
}

// 7. The row pitch comes from RowWords, not from width*bpp/8.
//
// This is the regression guard for a real bug: deriving the stride from the
// visible width rendered the ROM's 1024-byte rows at a 640-byte pitch and
// sheared the picture into diagonal bands.  The booted System reports the
// same number in ScreenRow, and PrimaryInit paints `cvpRowWords << 3`
// LONGWORDS per row (civic.md §4 step 15) — i.e. RowWords * 32 bytes.
TEST(test_stride_follows_rowwords) {
    display_t *d = av_civic_display(s_st.civic);
    seb_write(SEB_PCBR, 0x13); // 8 bpp: 640 visible bytes per row

    civic_write_reg(R_ROWWORDS, 8, 32); // the Hi-Res 640x480 value
    ASSERT_EQ_INT((int)d->stride, 1024);

    civic_write_reg(R_ROWWORDS, 8, 64); // a wider pitch must be honoured
    ASSERT_EQ_INT((int)d->stride, 2048);

    // Below the packed row the descriptor falls back to the packed width, so
    // it can never claim a stride that cannot hold a scanline.
    civic_write_reg(R_ROWWORDS, 8, 4); // 128 bytes < 640 visible
    ASSERT_EQ_INT((int)d->stride, 640);

    civic_write_reg(R_ROWWORDS, 8, 32); // restore
    ASSERT_EQ_INT((int)d->stride, 1024);
}

// 6. VBL: a frame with the timing generator + VBLEnb on latches VBLInt
//    and asserts the PSC slot line; the ack is VBLClr 0-then-1.
TEST(test_vbl_ack_dance) {
    slot_write(R_ENABLE, 1);
    slot_write(R_VBLENB, 1);
    slot_write(R_VBLCLR, 1); // arm
    s_slot_vbl = -1;

    s_frame_cb(s_frame_src, 0); // one frame
    ASSERT_EQ_INT((int)slot_read(R_VBLINT), 1); // active HIGH
    ASSERT_EQ_INT(s_slot_vbl, 1); // PSC SInt bit 6 asserted

    // Ack: write 0 (clear + disarm) then 1 (re-arm).
    slot_write(R_VBLCLR, 0);
    ASSERT_EQ_INT((int)slot_read(R_VBLINT), 0);
    ASSERT_EQ_INT(s_slot_vbl, 0);
    s_frame_cb(s_frame_src, 0); // disarmed — no interrupt
    ASSERT_EQ_INT((int)slot_read(R_VBLINT), 0);
    slot_write(R_VBLCLR, 1);
    s_frame_cb(s_frame_src, 0);
    ASSERT_EQ_INT((int)slot_read(R_VBLINT), 1);

    // With the timing generator off, frames raise nothing.
    slot_write(R_VBLCLR, 0);
    slot_write(R_VBLCLR, 1);
    slot_write(R_ENABLE, 0);
    s_frame_cb(s_frame_src, 0);
    ASSERT_EQ_INT((int)slot_read(R_VBLINT), 0);
}

int main(void) {
    s_cfg.machine_context = &s_st;
    // civic.c only drives the slot-interrupt line when a PSC exists; the
    // stub above records the calls, so any non-NULL handle will do.
    s_st.psc = (av_psc_t *)&s_st;
    s_st.civic = av_civic_init(&s_cfg, NULL);
    ASSERT_TRUE(s_st.civic != NULL);

    RUN(test_serial_codec);
    RUN(test_direct_registers);
    RUN(test_computed_slots);
    RUN(test_monitor_sense);
    RUN(test_sebastian_clut);
    RUN(test_vbl_ack_dance);
    RUN(test_stride_follows_rowwords);

    fprintf(stderr, "civic: all tests passed\n");
    return 0;
}
