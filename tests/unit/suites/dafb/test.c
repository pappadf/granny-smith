// DAFB register-level tests (Quadra proposal Phase D; reference §22.9).
//
// Drives dafb.c through its memory interface exactly as the bus would.
// The Swatch/DP8531 values are the boot ROM's observed 640×480 mode set
// (local/gs-docs/DAFB/re/q700-rom-dafb-access.log), so the geometry
// derivations are pinned to real programming, not invented numbers.

#include "dafb.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// `has_event` is only referenced by dafb_attach_scheduler, which these
// bus-level tests never call; provide the missing stub.
#include "scheduler.h"
bool has_event(struct scheduler *s, event_callback_t cb) {
    (void)s;
    (void)cb;
    return false;
}

static dafb_t *make_dafb(void) {
    dafb_t *d = dafb_init(0x200000u, NULL);
    ASSERT_TRUE(d != NULL);
    return d;
}

static void w32(dafb_t *d, uint32_t off, uint32_t val) {
    const memory_interface_t *m = dafb_reg_interface(d);
    m->write_uint32((void *)d, off, val);
}

static uint32_t r32(dafb_t *d, uint32_t off) {
    const memory_interface_t *m = dafb_reg_interface(d);
    return m->read_uint32((void *)d, off);
}

// Program the ROM's observed 640×480 mode: DP8531 nibbles (25.175 MHz),
// Swatch geometry, base $1000, stride $100 (1024 bytes).
static void program_640x480(dafb_t *d) {
    static const uint8_t clk[16] = {0xF, 0x1, 0x1, 0x0, 0x9, 0x3, 0x0, 0x0, 0x0, 0x2, 0x5, 0x6, 0x4, 0x1, 0x0, 0x0};
    for (int i = 0; i < 16; i++)
        w32(d, 0x300u + (uint32_t)i * 0x10u, clk[i]);
    w32(d, 0x000, 0x008); // base hi → $1000
    w32(d, 0x004, 0x000);
    w32(d, 0x008, 0x100); // stride 256 words = 1024 bytes
    w32(d, 0x140, 0x088); // HAL
    w32(d, 0x144, 0x308); // HFP → width 640
    w32(d, 0x148, 0x31E); // HPIX → h_total 800
    w32(d, 0x15C, 0x044); // VAL
    w32(d, 0x160, 0x404); // VFP → height 480
    w32(d, 0x164, 0x408); // VFPEQ → v_total 516
    w32(d, 0x100, 0xFF2); // Swatch enable (mode-set commit point)
}

TEST(geometry_from_rom_mode_set) {
    dafb_t *d = make_dafb();
    program_640x480(d);
    w32(d, 0x220, 0x80); // PCBR0: 1bpp, divide 1 (the ROM's gray screen)
    display_t *disp = dafb_display(d);
    ASSERT_EQ_INT(640, (int)disp->width);
    ASSERT_EQ_INT(480, (int)disp->height);
    ASSERT_EQ_INT(1024, (int)disp->stride);
    ASSERT_EQ_INT(PIXEL_1BPP_MSB, (int)disp->format);
    ASSERT_TRUE(disp->bits == dafb_vram(d) + 0x1000);
    dafb_delete(d);
}

TEST(depth_matrix_pcbr0) {
    dafb_t *d = make_dafb();
    program_640x480(d);
    static const struct {
        uint8_t pcbr0;
        pixel_format_t fmt;
    } rows[] = {
        {0x00, PIXEL_1BPP_MSB  },
        {0x08, PIXEL_2BPP_MSB  },
        {0x10, PIXEL_4BPP_MSB  },
        {0x18, PIXEL_8BPP      },
        {0x1C, PIXEL_32BPP_XRGB},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        w32(d, 0x220, rows[i].pcbr0);
        ASSERT_EQ_INT((int)rows[i].fmt, (int)dafb_display(d)->format);
        ASSERT_EQ_INT(640, (int)dafb_display(d)->width);
    }
    // Undefined depth pattern: mode unchanged, no crash (Trap 24: logged)
    w32(d, 0x220, 0x04);
    ASSERT_EQ_INT((int)PIXEL_32BPP_XRGB, (int)dafb_display(d)->format);
    // PCBR0's VidClk field (bits 6:5) picks the RAMDAC's PixClk/1,/2,/4 tap,
    // which is what SWATCH is clocked from — so each SWATCH horizontal count
    // carries that many DOTS and the field MULTIPLIES the derived width.  It
    // used to divide here, which is why every monitor whose CRTC runs below
    // the dot clock (all of them wider or taller than 640x480) came out 1/2
    // or 1/4 size.  See dafb.c's pixel_multiplier() for the measured table.
    w32(d, 0x220, 0x18 | 0x20);
    ASSERT_EQ_INT(1280, (int)dafb_display(d)->width);
    dafb_delete(d);
}

TEST(clut_component_phase) {
    dafb_t *d = make_dafb();
    display_t *disp = dafb_display(d);
    w32(d, 0x200, 5); // index 5
    w32(d, 0x210, 0x11);
    w32(d, 0x210, 0x22);
    w32(d, 0x210, 0x33); // commit entry 5, auto-increment
    w32(d, 0x210, 0x44);
    w32(d, 0x210, 0x55);
    w32(d, 0x210, 0x66); // commit entry 6
    ASSERT_EQ_INT(0x11, disp->clut[5].r);
    ASSERT_EQ_INT(0x22, disp->clut[5].g);
    ASSERT_EQ_INT(0x33, disp->clut[5].b);
    ASSERT_EQ_INT(0x44, disp->clut[6].r);
    ASSERT_EQ_INT(0x66, disp->clut[6].b);
    // A partial triplet is real state: an address write resets the phase
    // without committing (Trap 11).
    w32(d, 0x200, 9);
    w32(d, 0x210, 0x77); // R only
    w32(d, 0x200, 9); // phase reset
    w32(d, 0x210, 0xAA);
    w32(d, 0x210, 0xBB);
    w32(d, 0x210, 0xCC);
    ASSERT_EQ_INT(0xAA, disp->clut[9].r);
    dafb_delete(d);
}

TEST(sense_protocol_13in) {
    dafb_t *d = make_dafb();
    dafb_set_monitor_sense(d, 6); // 13" RGB: line 0 grounded by the monitor
    // Reset state: drives tristate; read returns the inverted passive code.
    ASSERT_EQ_INT(0x1, (int)(r32(d, 0x01C) & 7)); // ~6 & 7
    // The extended cross-drive tuple (DepVideoEqu.a masks): each probe
    // drives one line low (bit clear) and reads the others.
    w32(d, 0x01C, 0x3); // drive line 2
    ASSERT_EQ_INT(0x5, (int)(r32(d, 0x01C) & 7)); // ~0b010
    w32(d, 0x01C, 0x5); // drive line 1
    ASSERT_EQ_INT(0x3, (int)(r32(d, 0x01C) & 7)); // ~0b100
    w32(d, 0x01C, 0x6); // drive line 0
    ASSERT_EQ_INT(0x1, (int)(r32(d, 0x01C) & 7)); // ~0b110
    w32(d, 0x01C, 0x7); // release all
    ASSERT_EQ_INT(0x1, (int)(r32(d, 0x01C) & 7)); // passive code again
    dafb_delete(d);
}

// The extended (tie-matrix) probe, replayed exactly as PrimaryInit.a's
// DoDAFBExtendedSense does it: drive one line, read the other two, three
// times, assembling "bc/ac/ab".  GoldFish (the Apple 16", raw $2D) is the
// one extended monitor this ROM family has timings for, and it is what the
// Quadra's 832x624 coverage rides on.
TEST(sense_protocol_extended_16in) {
    dafb_t *d = make_dafb();
    dafb_set_monitor_sense(d, DAFB_SENSE_INDEXED_GF);
    // Passive probe must read as no-connect ($7) — that is what makes the
    // ROM run the extended algorithm at all.  Register is inverted.
    ASSERT_EQ_INT(0x0, (int)(r32(d, 0x01C) & 7)); // ~7 & 7

    // $2D = 0b101101.  Reassemble it the way the ROM does and check the
    // answer end-to-end rather than asserting three magic register reads.
    w32(d, 0x01C, 0x3); // drive A -> released lines carry ext[5:4]
    unsigned a = (~r32(d, 0x01C)) & 0x3u; // ROM: ANDI #dafbAMask
    w32(d, 0x01C, 0x5); // drive B -> ext[3:2], A read in bit 2
    unsigned b = (~r32(d, 0x01C)) & 0x5u; // ROM: ANDI #dafbBMask
    b = (b & 0x1u) | ((b & 0x4u) ? 0x2u : 0u); // ROM: move bit A into bit B
    w32(d, 0x01C, 0x6); // drive C -> ext[1:0], in bits 2:1
    unsigned c = ((~r32(d, 0x01C)) & 0x6u) >> 1; // ROM: ANDI #dafbCMask, LSR #1
    ASSERT_EQ_INT(0x2D, (int)((a << 4) | (b << 2) | c));

    // A passive monitor must not answer the tie matrix at all.
    dafb_set_monitor_sense(d, 6);
    w32(d, 0x01C, 0x3);
    ASSERT_EQ_INT(0x5, (int)(r32(d, 0x01C) & 7));
    dafb_delete(d);
}

TEST(swatch_interrupt_clears) {
    dafb_t *d = make_dafb();
    // Seed pending bits as the frame event would.
    w32(d, 0x108, 0x5);
    ASSERT_EQ_INT(0x5, (int)r32(d, 0x108));
    w32(d, 0x10C, 0); // clear cursor
    ASSERT_EQ_INT(0x1, (int)r32(d, 0x108));
    w32(d, 0x114, 0); // clear VBL
    ASSERT_EQ_INT(0x0, (int)r32(d, 0x108));
    dafb_delete(d);
}

int main(void) {
    RUN(geometry_from_rom_mode_set);
    RUN(depth_matrix_pcbr0);
    RUN(clut_component_phase);
    RUN(sense_protocol_13in);
    RUN(sense_protocol_extended_16in);
    RUN(swatch_interrupt_clears);
    printf("[dafb] all tests passed\n");
    return 0;
}
