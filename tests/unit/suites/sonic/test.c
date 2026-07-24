// DP83932 SONIC chip-model tests (Quadra proposal Phase F).
//
// Each case mirrors one of Apple's ROM self-tests
// (OS/StartMgr/UnivTestEnv/SONIC_BitMarch.c / _CAMDMA.c / _Interrupt.c /
// _Loopback.c): the register bit-march quirks, the Load-CAM descriptor DMA
// + CAM port readback, ISR/IMR interrupt gating, and a full MAC loopback
// through the RRA/RDA/TDA linked-list buffer management — all against a
// mock flat guest memory installed through sonic_set_memory_hooks.

#include "sonic.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// === Register indices (mirror sonic.c / SonicEqu.a) ===
enum {
    S_CR = 0x00,
    S_DCR = 0x01,
    S_RCR = 0x02,
    S_TCR = 0x03,
    S_IMR = 0x04,
    S_ISR = 0x05,
    S_UTDA = 0x06,
    S_CTDA = 0x07,
    S_TPS = 0x08,
    S_TFC = 0x09,
    S_TSA0 = 0x0A,
    S_TSA1 = 0x0B,
    S_TFS = 0x0C,
    S_URDA = 0x0D,
    S_CRDA = 0x0E,
    S_CRBA0 = 0x0F,
    S_CRBA1 = 0x10,
    S_RBWC0 = 0x11,
    S_RBWC1 = 0x12,
    S_EOBC = 0x13,
    S_URRA = 0x14,
    S_RSA = 0x15,
    S_REA = 0x16,
    S_RRP = 0x17,
    S_RWP = 0x18,
    S_CEP = 0x21,
    S_CAP2 = 0x22,
    S_CAP1 = 0x23,
    S_CAP0 = 0x24,
    S_CE = 0x25,
    S_CDP = 0x26,
    S_CDC = 0x27,
    S_SR = 0x28,
    S_WT0 = 0x29,
    S_WT1 = 0x2A,
    S_DCR2 = 0x3F,
};

// ISR bits
#define I_LCD   0x1000
#define I_PINT  0x0800
#define I_PKTRX 0x0400
#define I_TXDN  0x0200
#define I_RDE   0x0040

// === Mock guest memory: 256 KB flat buffer based at MOCK_BASE ===

#define MOCK_BASE 0x00040000u
#define MOCK_SIZE 0x40000u
static uint8_t mem[MOCK_SIZE];

static uint32_t mock_read(void *ctx, uint32_t phys, unsigned width) {
    (void)ctx;
    uint32_t off = phys - MOCK_BASE;
    ASSERT_TRUE(off + width <= MOCK_SIZE);
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++)
        v = (v << 8) | mem[off + i];
    return v;
}

static void mock_write(void *ctx, uint32_t phys, uint32_t value, unsigned width) {
    (void)ctx;
    uint32_t off = phys - MOCK_BASE;
    ASSERT_TRUE(off + width <= MOCK_SIZE);
    for (unsigned i = 0; i < width; i++)
        mem[off + i] = (uint8_t)(value >> (8 * (width - 1 - i)));
}

// 32-bit-mode descriptor field helpers (16-bit value in the low half of a
// big-endian longword slot).
static void poke_field(uint32_t phys, uint16_t v) {
    mock_write(NULL, phys, v, 4);
}
static uint16_t peek_field(uint32_t phys) {
    return (uint16_t)mock_read(NULL, phys, 4);
}

// === IRQ line probe ===
static int irq_edges;
static bool irq_level;
static void irq_cb(void *ctx, bool active) {
    (void)ctx;
    irq_level = active;
    irq_edges++;
}

static sonic_t *fresh(void) {
    static sonic_t *s;
    if (s)
        sonic_delete(s);
    s = sonic_init(NULL);
    ASSERT_TRUE(s != NULL);
    sonic_set_memory_hooks(s, mock_read, mock_write, NULL);
    sonic_set_irq_callback(s, irq_cb, NULL);
    memset(mem, 0, sizeof(mem));
    irq_edges = 0;
    irq_level = false;
    return s;
}

// ============================================================
// BitMarch: register semantics (SONIC_BitMarch.c)
// ============================================================

TEST(test_bitmarch_register_semantics) {
    sonic_t *s = fresh();

    // Software reset latches and reads back exactly $0094.
    sonic_reg_write(s, S_CR, 0x0094);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CR), 0x0094);

    // 32-bit bus mode, rev>3 extended config (Spike values).
    sonic_reg_write(s, S_DCR2, 0x0000);
    sonic_reg_write(s, S_DCR, 0x832C);

    // IMR: write 0 reads 0.
    sonic_reg_write(s, S_IMR, 0x0000);
    ASSERT_EQ_INT(sonic_reg_read(s, S_IMR), 0);

    // ISR: write-1-to-clear the full $7FFF -> reads 0.
    sonic_reg_write(s, S_ISR, 0x7FFF);
    ASSERT_EQ_INT(sonic_reg_read(s, S_ISR), 0);

    // Plain no-rules registers: full 16-bit r/w walk.
    static const int plain[] = {S_UTDA,  S_CTDA,  S_TFC,   S_TSA0,  S_TSA1, S_URDA, S_CRDA,
                                S_CRBA0, S_CRBA1, S_RBWC0, S_RBWC1, S_URRA, S_WT0,  S_WT1};
    for (unsigned r = 0; r < sizeof(plain) / sizeof(plain[0]); r++) {
        for (uint32_t bit = 1; bit <= 0x8000u; bit <<= 1) {
            sonic_reg_write(s, plain[r], (uint16_t)bit);
            ASSERT_EQ_INT(sonic_reg_read(s, plain[r]), (int)bit);
        }
    }

    // TPS reads back inverted.
    sonic_reg_write(s, S_TPS, 0x1234);
    ASSERT_EQ_INT(sonic_reg_read(s, S_TPS), 0xEDCB);

    // TFS stores >>2 in 32-bit mode, >>1 in 16-bit mode.
    sonic_reg_write(s, S_TFS, 0x0040);
    ASSERT_EQ_INT(sonic_reg_read(s, S_TFS), 0x0010);
    sonic_reg_write(s, S_DCR, 0x830C); // 16-bit mode
    sonic_reg_write(s, S_TFS, 0x0040);
    ASSERT_EQ_INT(sonic_reg_read(s, S_TFS), 0x0020);
    sonic_reg_write(s, S_DCR, 0x832C);

    // Word-pointer registers force bit 0 to zero.
    static const int b0[] = {S_EOBC, S_RSA, S_REA, S_RRP, S_RWP};
    for (unsigned r = 0; r < sizeof(b0) / sizeof(b0[0]); r++) {
        sonic_reg_write(s, b0[r], 0xFFFF);
        ASSERT_EQ_INT(sonic_reg_read(s, b0[r]), 0xFFFE);
    }

    // CAM entry pointer keeps 4 bits, descriptor count 5 bits.
    sonic_reg_write(s, S_CEP, 0xFFFF);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CEP), 0x000F);
    sonic_reg_write(s, S_CDC, 0xFFFF);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CDC), 0x001F);

    // RCR/TCR: only the control halves are writable.
    sonic_reg_write(s, S_RCR, 0xFFFF);
    ASSERT_EQ_INT(sonic_reg_read(s, S_RCR) & 0xFE00, 0xFE00);
    sonic_reg_write(s, S_RCR, 0x0000);
    ASSERT_EQ_INT(sonic_reg_read(s, S_RCR) & 0xFE00, 0);
}

// ============================================================
// CAM DMA: Load CAM + port readback (SONIC_CAMDMA.c)
// ============================================================

TEST(test_camdma_load_and_readback) {
    sonic_t *s = fresh();

    sonic_reg_write(s, S_CR, 0x0094); // reset
    sonic_reg_write(s, S_IMR, 0x0000);
    sonic_reg_write(s, S_ISR, 0x7FFF);
    sonic_reg_write(s, S_DCR2, 0x0000);
    sonic_reg_write(s, S_DCR, 0x832C); // 32-bit mode

    // CAM descriptor area: two entries {index, ap0, ap1, ap2} + enable.
    uint32_t cda = MOCK_BASE + 0x1000;
    poke_field(cda + 0x00, 0); // entry 0
    poke_field(cda + 0x04, 0x0040);
    poke_field(cda + 0x08, 0x0090);
    poke_field(cda + 0x0C, 0xE080);
    poke_field(cda + 0x10, 1); // entry 1
    poke_field(cda + 0x14, 0xFFFF);
    poke_field(cda + 0x18, 0xFFFF);
    poke_field(cda + 0x1C, 0xFFFF);
    poke_field(cda + 0x20, 0x0003); // CAM enable mask

    sonic_reg_write(s, S_CDC, 2);
    sonic_reg_write(s, S_URRA, (uint16_t)((cda >> 16) & 0xFFFF));
    sonic_reg_write(s, S_CDP, (uint16_t)(cda & 0xFFFF));

    // Clear reset, then invoke Load CAM (the CAMDMA test's exact dance).
    uint16_t cr = sonic_reg_read(s, S_CR);
    sonic_reg_write(s, S_CR, cr & 0xFF7F);
    cr = sonic_reg_read(s, S_CR);
    sonic_reg_write(s, S_CR, cr | 0x0200);

    // LCD posted; CDC consumed; CDP past the enable field.
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & I_LCD);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CDC), 0);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CDP), (int)((cda & 0xFFFF) + 0x24));
    ASSERT_EQ_INT(sonic_reg_read(s, S_CE), 0x0003);

    // Back into reset for the port readback.
    sonic_reg_write(s, S_CR, 0x0094);
    sonic_reg_write(s, S_CEP, 0);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CAP0), 0x0040);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CAP1), 0x0090);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CAP2), 0xE080);
    sonic_reg_write(s, S_CEP, 1);
    ASSERT_EQ_INT(sonic_reg_read(s, S_CAP0), 0xFFFF);
}

// ============================================================
// Interrupt gating (SONIC_Interrupt.c / _Loopback.c interrupt mode)
// ============================================================

TEST(test_interrupt_mask_gating) {
    sonic_t *s = fresh();

    sonic_reg_write(s, S_CR, 0x0094);
    sonic_reg_write(s, S_ISR, 0x7FFF);
    sonic_reg_write(s, S_IMR, 0x0000);
    ASSERT_TRUE(!irq_level);

    // PINTR 0->1 in the TCR raises PINT (the Apple interrupt self-test's
    // generator) — masked, so the line stays low.
    sonic_reg_write(s, S_TCR, 0x0000);
    sonic_reg_write(s, S_TCR, 0x8000);
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & I_PINT);
    ASSERT_TRUE(!irq_level);

    // Unmask -> line rises; ack -> line falls.
    sonic_reg_write(s, S_IMR, 0x7FFF);
    ASSERT_TRUE(irq_level);
    sonic_reg_write(s, S_ISR, I_PINT);
    ASSERT_TRUE(!irq_level);
    ASSERT_TRUE(irq_edges >= 2);
}

// ============================================================
// MAC loopback (SONIC_Loopback.c): TDA -> frame -> RRA/RBA/RDA
// ============================================================

// Memory layout for the loopback scene (all inside the mock buffer).
#define LB_RDA1 (MOCK_BASE + 0x2000)
#define LB_RDA2 (MOCK_BASE + 0x2100)
#define LB_RRA  (MOCK_BASE + 0x2200)
#define LB_TDA  (MOCK_BASE + 0x2400)
#define LB_TBA  (MOCK_BASE + 0x3000)
#define LB_RBA1 (MOCK_BASE + 0x8000)
#define LB_RBA2 (MOCK_BASE + 0x9000)

static void lb_setup(sonic_t *s) {
    sonic_reg_write(s, S_CR, 0x0094);
    sonic_reg_write(s, S_DCR2, 0x0000);
    sonic_reg_write(s, S_DCR, 0x832C); // 32-bit, Spike config
    sonic_reg_write(s, S_IMR, 0x0000);
    sonic_reg_write(s, S_ISR, 0x7FFF);
    sonic_reg_write(s, S_EOBC, 0x02F8); // 760 words, the Apple value

    // RDA chain: RDA1 -> RDA2 (EOL).
    poke_field(LB_RDA1 + 0x14, (uint16_t)(LB_RDA2 & 0xFFFE)); // link, EOL=0
    poke_field(LB_RDA1 + 0x18, 0xFFFF); // in_use: available
    poke_field(LB_RDA2 + 0x14, (uint16_t)((LB_RDA1 & 0xFFFF) | 1)); // link, EOL=1
    poke_field(LB_RDA2 + 0x18, 0xFFFF);
    sonic_reg_write(s, S_URDA, (uint16_t)(LB_RDA1 >> 16));
    sonic_reg_write(s, S_CRDA, (uint16_t)(LB_RDA1 & 0xFFFF));

    // RRA with two resources of 800 words each.
    poke_field(LB_RRA + 0x00, (uint16_t)(LB_RBA1 & 0xFFFF));
    poke_field(LB_RRA + 0x04, (uint16_t)(LB_RBA1 >> 16));
    poke_field(LB_RRA + 0x08, 800);
    poke_field(LB_RRA + 0x0C, 0);
    poke_field(LB_RRA + 0x10, (uint16_t)(LB_RBA2 & 0xFFFF));
    poke_field(LB_RRA + 0x14, (uint16_t)(LB_RBA2 >> 16));
    poke_field(LB_RRA + 0x18, 800);
    poke_field(LB_RRA + 0x1C, 0);
    sonic_reg_write(s, S_URRA, (uint16_t)(LB_RRA >> 16));
    sonic_reg_write(s, S_RSA, (uint16_t)(LB_RRA & 0xFFFF));
    sonic_reg_write(s, S_RRP, (uint16_t)(LB_RRA & 0xFFFF));
    sonic_reg_write(s, S_REA, (uint16_t)((LB_RRA + 0x20) & 0xFFFF));
    sonic_reg_write(s, S_RWP, (uint16_t)((LB_RRA + 0x20) & 0xFFFF));

    // Prime the first resource (the test's explicit RRRA).
    uint16_t cr = sonic_reg_read(s, S_CR);
    sonic_reg_write(s, S_CR, cr & 0xFF7F); // leave reset
    cr = sonic_reg_read(s, S_CR);
    sonic_reg_write(s, S_CR, cr | 0x0100); // read RRA
    ASSERT_EQ_INT(sonic_reg_read(s, S_CRBA0), (int)(LB_RBA1 & 0xFFFF));
    ASSERT_EQ_INT(sonic_reg_read(s, S_CRBA1), (int)(LB_RBA1 >> 16));
    ASSERT_EQ_INT(sonic_reg_read(s, S_RBWC0), 800);
}

TEST(test_mac_loopback_single_fragment) {
    sonic_t *s = fresh();
    lb_setup(s);

    // One 1500-byte single-fragment packet, EOL TDA.
    for (int i = 0; i < 1500; i++)
        mem[(LB_TBA - MOCK_BASE) + i] = (uint8_t)(0xA5 ^ (i & 0x7F)); // nonzero pattern
    poke_field(LB_TDA + 0x00, 0); // status
    poke_field(LB_TDA + 0x04, 0x0000); // config
    poke_field(LB_TDA + 0x08, 1500); // pkt_size
    poke_field(LB_TDA + 0x0C, 1); // frag_count
    poke_field(LB_TDA + 0x10, (uint16_t)(LB_TBA & 0xFFFF));
    poke_field(LB_TDA + 0x14, (uint16_t)(LB_TBA >> 16));
    poke_field(LB_TDA + 0x18, 1500); // frag_size
    poke_field(LB_TDA + 0x1C, (uint16_t)((LB_TDA & 0xFFFF) | 1)); // link, EOL
    sonic_reg_write(s, S_UTDA, (uint16_t)(LB_TDA >> 16));
    sonic_reg_write(s, S_CTDA, (uint16_t)(LB_TDA & 0xFFFF));

    // MAC loopback + receiver on, then transmit (CR = $0008, $000A).
    sonic_reg_write(s, S_RCR, 0xFA00);
    sonic_reg_write(s, S_CR, 0x0008);
    sonic_reg_write(s, S_CR, 0x000A);

    // TX status: PTX, none of the error bits the test checks ($044E).
    uint16_t txs = peek_field(LB_TDA + 0x00);
    ASSERT_TRUE(txs & 0x0001);
    ASSERT_EQ_INT(txs & 0x044E, 0);

    // RX descriptor 1: LBK+PRX status, byte count = 1500+4 FCS, released.
    uint16_t rxs = peek_field(LB_RDA1 + 0x00);
    ASSERT_TRUE(rxs & 0x0002); // LBK
    ASSERT_TRUE(rxs & 0x0001); // PRX
    ASSERT_EQ_INT(rxs & 0x000C, 0); // no CRC/FAE errors
    ASSERT_EQ_INT(peek_field(LB_RDA1 + 0x04), 1504);
    ASSERT_EQ_INT(peek_field(LB_RDA1 + 0x08), (int)(LB_RBA1 & 0xFFFF));
    ASSERT_EQ_INT(peek_field(LB_RDA1 + 0x0C), (int)(LB_RBA1 >> 16));
    ASSERT_EQ_INT(peek_field(LB_RDA1 + 0x18), 0); // in_use released

    // Payload byte-exact in the RBA.
    for (int i = 0; i < 1500; i++)
        ASSERT_EQ_INT(mem[(LB_RBA1 - MOCK_BASE) + i], (uint8_t)(0xA5 ^ (i & 0x7F)));

    // CRDA advanced to RDA2; TXDN + PKTRX posted (the $1E00 check).
    ASSERT_EQ_INT(sonic_reg_read(s, S_CRDA), (int)(LB_RDA2 & 0xFFFE));
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & I_TXDN);
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & I_PKTRX);
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & 0x1E00);

    // The 1500+4-byte packet ate the 800-word RBA below EOBC: the next
    // resource was fetched automatically.
    ASSERT_EQ_INT(sonic_reg_read(s, S_CRBA0), (int)(LB_RBA2 & 0xFFFF));
}

TEST(test_mac_loopback_two_fragments_and_rde) {
    sonic_t *s = fresh();
    lb_setup(s);

    // A 1500-byte packet split 750+750 across two fragments (the Apple
    // second packet), transmitted twice: the second delivery lands in
    // RDA2 whose link carries EOL -> RDE.
    for (int i = 0; i < 750; i++)
        mem[(LB_TBA - MOCK_BASE) + i] = 0x11;
    for (int i = 0; i < 750; i++)
        mem[(LB_TBA - MOCK_BASE) + 0x800 + i] = 0x22;
    poke_field(LB_TDA + 0x00, 0);
    poke_field(LB_TDA + 0x04, 0x0000);
    poke_field(LB_TDA + 0x08, 1500);
    poke_field(LB_TDA + 0x0C, 2); // two fragments
    poke_field(LB_TDA + 0x10, (uint16_t)(LB_TBA & 0xFFFF));
    poke_field(LB_TDA + 0x14, (uint16_t)(LB_TBA >> 16));
    poke_field(LB_TDA + 0x18, 750);
    poke_field(LB_TDA + 0x1C, (uint16_t)((LB_TBA + 0x800) & 0xFFFF));
    poke_field(LB_TDA + 0x20, (uint16_t)((LB_TBA + 0x800) >> 16));
    poke_field(LB_TDA + 0x24, 750);
    poke_field(LB_TDA + 0x28, (uint16_t)((LB_TDA & 0xFFFF) | 1)); // link, EOL
    sonic_reg_write(s, S_UTDA, (uint16_t)(LB_TDA >> 16));
    sonic_reg_write(s, S_CTDA, (uint16_t)(LB_TDA & 0xFFFF));

    sonic_reg_write(s, S_RCR, 0xFA00);
    sonic_reg_write(s, S_CR, 0x0008);
    sonic_reg_write(s, S_CR, 0x000A);

    // First delivery: both fragments contiguous in the RBA.
    for (int i = 0; i < 750; i++)
        ASSERT_EQ_INT(mem[(LB_RBA1 - MOCK_BASE) + i], 0x11);
    for (int i = 750; i < 1500; i++)
        ASSERT_EQ_INT(mem[(LB_RBA1 - MOCK_BASE) + i], 0x22);

    // Second transmit of the same list: fills RDA2, then hits its EOL.
    poke_field(LB_TDA + 0x00, 0);
    sonic_reg_write(s, S_CTDA, (uint16_t)(LB_TDA & 0xFFFF));
    sonic_reg_write(s, S_CR, 0x000A);
    ASSERT_TRUE(peek_field(LB_RDA2 + 0x00) & 0x0002);
    ASSERT_TRUE(sonic_reg_read(s, S_ISR) & I_RDE);
}

int main(void) {
    RUN(test_bitmarch_register_semantics);
    RUN(test_camdma_load_and_readback);
    RUN(test_interrupt_mask_gating);
    RUN(test_mac_loopback_single_fragment);
    RUN(test_mac_loopback_two_fragments_and_rde);
    printf("All sonic tests passed\n");
    return 0;
}
