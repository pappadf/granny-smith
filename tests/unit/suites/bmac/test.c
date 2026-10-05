// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// BMAC Ethernet cell unit test.
//
// The real core/peripherals/bmac.c on the real DBDMA engine, against a flat
// guest-memory array.  Register values on bmac_read/bmac_write are the
// little-endian register values a driver composes with sthbrx; descriptors
// are built in memory exactly as the drivers build them (Apple's
// AppleBMacEthernet, Linux bmac, the BSD bm driver).

#include "bmac.h"
#include "crc32.h"
#include "dbdma.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording checkpoint stream
// ============================================================================

static uint8_t s_cp_buf[1 << 16];
static size_t s_cp_w, s_cp_r;

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp;
    (void)tag;
    (void)file;
    (void)line;
    ASSERT_TRUE(s_cp_w + size <= sizeof s_cp_buf);
    memcpy(s_cp_buf + s_cp_w, data, size);
    s_cp_w += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp;
    (void)tag;
    (void)file;
    (void)line;
    memcpy(data, s_cp_buf + s_cp_r, size);
    s_cp_r += size;
}

// ============================================================================
// Guest memory, descriptors
// ============================================================================

#define MEM_SIZE 0x10000
static uint8_t s_mem[MEM_SIZE];

static void mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(buf, s_mem + phys, len);
}

static void mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(s_mem + phys, buf, len);
}

static void poke32(uint32_t addr, uint32_t v) {
    for (int i = 0; i < 4; i++)
        s_mem[addr + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static uint32_t peek32(uint32_t addr) {
    return (uint32_t)s_mem[addr] | ((uint32_t)s_mem[addr + 1] << 8) | ((uint32_t)s_mem[addr + 2] << 16) |
           ((uint32_t)s_mem[addr + 3] << 24);
}

static void desc(uint32_t addr, uint32_t op, uint32_t data_addr, uint32_t cmd_dep) {
    poke32(addr + 12, 0);
    poke32(addr + 8, cmd_dep);
    poke32(addr + 4, data_addr);
    poke32(addr, op);
}

static uint32_t op(uint32_t cmd, uint32_t i, uint32_t b, uint32_t w, uint32_t req) {
    return (cmd << 28) | (i << 20) | (b << 18) | (w << 16) | (req & 0xFFFFu);
}

static uint16_t xfer_status(uint32_t d) {
    return (uint16_t)(peek32(d + 12) >> 16);
}

static uint16_t res_count(uint32_t d) {
    return (uint16_t)peek32(d + 12);
}

#define OUTPUT_MORE 0u
#define OUTPUT_LAST 1u
#define INPUT_MORE  2u
#define INPUT_LAST  3u
#define NOP_CMD     6u
#define STOP_CMD    7u

#define NEVER  0u
#define IFSET  1u
#define IFCLR  2u
#define ALWAYS 3u

#define TX 2
#define RX 3

// ============================================================================
// Fixture
// ============================================================================

static const uint8_t k_mac[6] = {0x00, 0x05, 0x02, 0xA7, 0x0D, 0x8C};
static dbdma_t *s_d;
static bmac_t *s_b;
static int s_irq_level, s_irq_edges;
static int s_dma_irqs[16];

static void cell_irq(void *ctx, bool level) {
    (void)ctx;
    if (level && !s_irq_level)
        s_irq_edges++;
    s_irq_level = level;
}

static void dma_irq(void *ctx, int chan) {
    (void)ctx;
    s_dma_irqs[chan]++;
}

static void fixture(void) {
    if (s_b)
        bmac_delete(s_b);
    if (s_d)
        dbdma_delete(s_d);
    memset(s_mem, 0, sizeof s_mem);
    s_irq_level = s_irq_edges = 0;
    memset(s_dma_irqs, 0, sizeof s_dma_irqs);
    s_d = dbdma_init(NULL, DBDMA_CHANNELS_HEATHROW);
    static const dma_mem_port_t mem_port = {.read_block = mem_read, .write_block = mem_write};
    dbdma_set_memory_port(s_d, &mem_port);
    dbdma_set_irq_hook(s_d, dma_irq, NULL);
    s_b = bmac_init(NULL, k_mac);
    bmac_set_irq(s_b, cell_irq, NULL);
    bmac_attach_dbdma(s_b, s_d, TX, RX);
}

// The drivers' stop-then-start (a parked channel keeps RUN set, and a RUN
// write that changes nothing starts nothing).
static void start(int chan, uint32_t cmdptr) {
    dbdma_reg_write(s_d, chan, DBDMA_REG_CONTROL, (uint32_t)(DBDMA_RUN | DBDMA_ACTIVE | DBDMA_DEAD) << 16);
    dbdma_reg_write(s_d, chan, DBDMA_REG_CMDPTRLO, cmdptr);
    dbdma_reg_write(s_d, chan, DBDMA_REG_CONTROL, (DBDMA_RUN << 16) | DBDMA_RUN);
}

// The common init core (Apple _initChip / Linux bmac_init_registers): the
// station address into MADD0-2, RXCFG = $0B00 (FCS kept, reject own,
// hash filter) and enable, the transmitter enabled.
static void chip_init(uint16_t rxcfg) {
    bmac_write(s_b, BMAC_TXRST, 1);
    bmac_write(s_b, BMAC_RXRST, 0);
    bmac_write(s_b, BMAC_XCVRIF, 0x000E);
    bmac_write(s_b, BMAC_MADD0, (uint16_t)(k_mac[0] << 8 | k_mac[1]));
    bmac_write(s_b, BMAC_MADD1, (uint16_t)(k_mac[2] << 8 | k_mac[3]));
    bmac_write(s_b, BMAC_MADD2, (uint16_t)(k_mac[4] << 8 | k_mac[5]));
    bmac_write(s_b, BMAC_RXCFG, rxcfg);
    bmac_write(s_b, BMAC_RXCFG, (uint16_t)(rxcfg | 1u));
    bmac_write(s_b, BMAC_TXCFG, 1);
}

// A minimal frame: DA, SA, type, payload.
static int frame(uint8_t *f, const uint8_t *da, const uint8_t *sa, int payload) {
    memcpy(f, da, 6);
    memcpy(f + 6, sa, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    for (int i = 0; i < payload; i++)
        f[14 + i] = (uint8_t)(0xA0 + i);
    return 14 + payload;
}

static const uint8_t k_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t k_other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

// ============================================================================
// Serial EEPROM: Apple's BMacEnetHW.cpp protocol, edge for edge
// ============================================================================

static void srom_clock_in(int bit) {
    uint16_t data = (uint16_t)((bit ? 1u : 0u) << 3 | 1u);
    bmac_write(s_b, BMAC_SROMCSR, data);
    bmac_write(s_b, BMAC_SROMCSR, (uint16_t)(data | 2u));
    bmac_write(s_b, BMAC_SROMCSR, data);
}

static int srom_clock_out(void) {
    bmac_write(s_b, BMAC_SROMCSR, 1u | 2u);
    int d = (bmac_read(s_b, BMAC_SROMCSR) >> 2) & 1;
    bmac_write(s_b, BMAC_SROMCSR, 1u);
    return d;
}

static uint16_t srom_read_word(int addr) {
    bmac_write(s_b, BMAC_SROMCSR, 0); // reset_and_select_srom
    srom_clock_in(1);
    srom_clock_in(1);
    srom_clock_in(0); // "read command 110"
    for (int i = 5; i >= 0; i--)
        srom_clock_in((addr >> i) & 1);
    uint16_t w = 0;
    for (int i = 0; i < 16; i++)
        w = (uint16_t)(w << 1 | srom_clock_out());
    bmac_write(s_b, BMAC_SROMCSR, 0);
    return w;
}

static uint8_t rev8(uint8_t v) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (v & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return r;
}

TEST(test_srom_station_address) {
    fixture();
    uint8_t ea[6];
    for (int i = 0; i < 3; i++) {
        uint16_t w = srom_read_word(10 + i);
        ea[2 * i] = rev8((uint8_t)(w & 0xFF));
        ea[2 * i + 1] = rev8((uint8_t)(w >> 8));
    }
    ASSERT_TRUE(memcmp(ea, k_mac, 6) == 0);
    // Leading zeros before the start bit are not a command.
    bmac_write(s_b, BMAC_SROMCSR, 0);
    srom_clock_in(0);
    srom_clock_in(0);
    srom_clock_in(1);
    srom_clock_in(1);
    srom_clock_in(0);
    for (int i = 5; i >= 0; i--)
        srom_clock_in((11 >> i) & 1);
    uint16_t w = 0;
    for (int i = 0; i < 16; i++)
        w = (uint16_t)(w << 1 | srom_clock_out());
    ASSERT_EQ_INT(w, srom_read_word(11));
    uint8_t got[6];
    bmac_get_mac(s_b, got);
    ASSERT_TRUE(memcmp(got, k_mac, 6) == 0);
}

// ============================================================================
// Registers the init sequences lean on
// ============================================================================

TEST(test_registers) {
    fixture();
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_CHIPID) & 0xFF, BMAC_CHIPID_HEATHROW);
    bmac_write(s_b, BMAC_TXRST, 1);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_TXRST), 0); // every driver polls for 0
    // No PHY drives MDIO: DataIn reads 0 whatever is written.
    bmac_write(s_b, BMAC_MIFCSR, 0x000F);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_MIFCSR), 0x0007);
    // Link up reads 0 in bit 8; the rest of XCVRIF is the driver's.
    bmac_write(s_b, BMAC_XCVRIF, 0x000E);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_XCVRIF), 0x000E);
    bmac_set_link(s_b, false);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_XCVRIF), 0x010E);
    bmac_set_link(s_b, true);
    // Plain scratch registers read back.
    bmac_write(s_b, BMAC_TXTH, 0xFF);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_TXTH), 0xFF);
    // A cell reset returns the registers, not the EEPROM.
    bmac_reset(s_b);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_TXTH), 0);
    ASSERT_EQ_INT(srom_read_word(10), (uint16_t)(rev8(k_mac[0]) | rev8(k_mac[1]) << 8));
}

// ============================================================================
// Transmit
// ============================================================================

// Apple's slot: OUTPUT_MORE header + OUTPUT_LAST payload waiting on S5
// (waitSelect $00200020, WAIT_IF_FALSE), then STOP.  The cell interrupt is
// FrameSent with Apple's mask $FCFF.
TEST(test_transmit_apple_slot) {
    fixture();
    chip_init(0x0B00);
    bmac_write(s_b, BMAC_INTDISABLE, 0xFCFF);
    uint8_t f[64];
    int n = frame(f, k_bcast, k_mac, 20);
    memcpy(s_mem + 0x1000, f, 14);
    memcpy(s_mem + 0x1100, f + 14, (size_t)(n - 14));
    desc(0x100, op(OUTPUT_MORE, NEVER, NEVER, NEVER, 14), 0x1000, 0);
    desc(0x110, op(OUTPUT_LAST, NEVER, NEVER, IFCLR, (uint32_t)(n - 14)), 0x1100, 0);
    desc(0x120, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    dbdma_reg_write(s_d, TX, DBDMA_REG_WAITSEL, 0x00200020);
    start(TX, 0x100);
    ASSERT_EQ_INT((int)bmac_tx_frames(s_b), 1);
    uint8_t sent[128];
    int len = bmac_last_tx(s_b, sent, sizeof sent);
    ASSERT_EQ_INT(len, 60); // padded to the minimum
    ASSERT_TRUE(memcmp(sent, f, (size_t)n) == 0);
    ASSERT_EQ_INT(sent[n], 0);
    // Completion as the drivers test it: ACTIVE in the last descriptor's status.
    ASSERT_TRUE(xfer_status(0x110) & DBDMA_ACTIVE);
    ASSERT_EQ_INT(res_count(0x110), 0);
    ASSERT_EQ_INT((int)dbdma_reg_read(s_d, TX, DBDMA_REG_CMDPTRLO), 0x120);
    // FrameSent raised the cell interrupt; reading STATUS clears it, and
    // inspecting it (bmac_peek, the debugger's read) does not.
    ASSERT_EQ_INT(s_irq_level, 1);
    ASSERT_EQ_INT(bmac_peek(s_b, BMAC_STATUS) & BMAC_ST_FRAME_SENT, BMAC_ST_FRAME_SENT);
    ASSERT_EQ_INT(bmac_peek(s_b, BMAC_STATUS) & BMAC_ST_FRAME_SENT, BMAC_ST_FRAME_SENT);
    ASSERT_EQ_INT(s_irq_level, 1);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_STATUS) & BMAC_ST_FRAME_SENT, BMAC_ST_FRAME_SENT);
    ASSERT_EQ_INT(s_irq_level, 0);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_STATUS), 0);
}

TEST(test_transmit_disabled_sends_nothing) {
    fixture();
    chip_init(0x0B00);
    bmac_write(s_b, BMAC_TXCFG, 0);
    uint8_t f[64];
    int n = frame(f, k_bcast, k_mac, 46);
    memcpy(s_mem + 0x1000, f, (size_t)n);
    desc(0x100, op(OUTPUT_LAST, NEVER, NEVER, NEVER, (uint32_t)n), 0x1000, 0);
    desc(0x110, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(TX, 0x100);
    ASSERT_EQ_INT((int)bmac_tx_frames(s_b), 0);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_STATUS), 0);
}

// ============================================================================
// Receive
// ============================================================================

// Linux's slot: one INPUT_LAST | INTR_ALWAYS of RX_BUFLEN.  The frame ends
// the descriptor early: resCount is the unused tail, xferStatus is
// RUN | ACTIVE | S6 ($8440, the value the BSD driver's debug filter shows
// real hardware writing), and the buffer holds frame, FCS, trailer.
TEST(test_receive_ends_descriptor_with_trailer) {
    fixture();
    chip_init(0x0B00);
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2000, 0);
    desc(0x210, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(RX, 0x200);
    ASSERT_EQ_INT(xfer_status(0x200), 0); // nothing yet: the channel waits
    uint8_t f[64];
    int n = frame(f, k_mac, k_other, 28); // 42 bytes, padded to 60
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(xfer_status(0x200), 0x8440);
    ASSERT_EQ_INT(res_count(0x200), 1520 - (60 + 4 + 2));
    ASSERT_EQ_INT(s_dma_irqs[RX], 1);
    ASSERT_TRUE(memcmp(s_mem + 0x2000, f, (size_t)n) == 0);
    ASSERT_EQ_INT(s_mem[0x2000 + n], 0); // the pad
    uint32_t fcs = gs_crc32(0, s_mem + 0x2000, 60);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ_INT(s_mem[0x2000 + 60 + i], (uint8_t)(fcs >> (8 * i)));
    // Trailer: big-endian frame length including the FCS, no abort bit.
    ASSERT_EQ_INT(s_mem[0x2000 + 64] << 8 | s_mem[0x2000 + 65], 64);
    ASSERT_EQ_INT((int)bmac_rx_frames(s_b), 1);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_STATUS) & BMAC_ST_FRAME_RECEIVED, BMAC_ST_FRAME_RECEIVED);
    // Without RXCFG $0100 the FCS is stripped and the trailer counts 60.
    fixture();
    chip_init(0x0A00);
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2000, 0);
    start(RX, 0x200);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(res_count(0x200), 1520 - (60 + 2));
    ASSERT_EQ_INT(s_mem[0x2000 + 60] << 8 | s_mem[0x2000 + 61], 60);
}

// Apple's slot: INPUT_MORE (INT_IF_TRUE, BRANCH_IF_TRUE -> next slot) for
// the first segment, INPUT_LAST INT_ALWAYS for the second; branch and
// interrupt select S6.  A frame that fits the first segment skips the
// second; one that does not runs on into it and ends there.
TEST(test_receive_apple_two_segment_slots) {
    fixture();
    chip_init(0x0B00);
    // slot 0 at $300: first segment 128 bytes, second 1400
    desc(0x300, op(INPUT_MORE, IFSET, IFSET, NEVER, 128), 0x3000, 0x320);
    desc(0x310, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1400), 0x3080, 0);
    // slot 1 at $320
    desc(0x320, op(INPUT_MORE, IFSET, IFSET, NEVER, 128), 0x4000, 0x340);
    desc(0x330, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1400), 0x4080, 0);
    desc(0x340, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    dbdma_reg_write(s_d, RX, DBDMA_REG_BRSEL, 0x00400040);
    dbdma_reg_write(s_d, RX, DBDMA_REG_INTRSEL, 0x00400040);
    start(RX, 0x300);

    uint8_t f[300];
    int n = frame(f, k_bcast, k_other, 50); // 64 + FCS + trailer = 70: fits
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(xfer_status(0x300), 0x8540); // RUN | ACTIVE | BT | S6
    ASSERT_EQ_INT(res_count(0x300), 128 - 70);
    ASSERT_EQ_INT(xfer_status(0x310), 0); // skipped
    ASSERT_EQ_INT(s_dma_irqs[RX], 1);
    ASSERT_EQ_INT((int)dbdma_reg_read(s_d, RX, DBDMA_REG_CMDPTRLO), 0x320);

    n = frame(f, k_bcast, k_other, 200); // 214 + 4 + 2 = 220: spills
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(xfer_status(0x320) & 0xFF, 0); // filled, no frame end yet
    ASSERT_EQ_INT(res_count(0x320), 0);
    ASSERT_EQ_INT(xfer_status(0x330), 0x8440);
    ASSERT_EQ_INT(res_count(0x330), 1400 - (220 - 128));
    ASSERT_EQ_INT(s_dma_irqs[RX], 2);
    ASSERT_TRUE(memcmp(s_mem + 0x4000, f, 128) == 0);
    ASSERT_TRUE(memcmp(s_mem + 0x4080, f + 128, (size_t)(n - 128)) == 0);
}

// A frame that ends on the descriptor's last byte ends that descriptor
// only: the next one waits for the next frame instead of completing empty.
TEST(test_receive_exact_fill) {
    fixture();
    chip_init(0x0B00);
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 66), 0x2000, 0);
    desc(0x210, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2100, 0);
    desc(0x220, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(RX, 0x200);
    uint8_t f[64];
    int n = frame(f, k_bcast, k_other, 46); // 60 + 4 + 2 = 66
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(xfer_status(0x200), 0x8440);
    ASSERT_EQ_INT(res_count(0x200), 0);
    ASSERT_EQ_INT(xfer_status(0x210), 0);
    ASSERT_EQ_INT(s_dma_irqs[RX], 1);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    ASSERT_EQ_INT(xfer_status(0x210), 0x8440);
    ASSERT_EQ_INT(res_count(0x210), 1520 - 66);
}

TEST(test_receive_filter) {
    fixture();
    uint8_t f[64];
    int n = frame(f, k_mac, k_other, 46);
    ASSERT_TRUE(!bmac_receive(s_b, f, n)); // receiver not enabled
    chip_init(0x0B00);
    ASSERT_TRUE(bmac_receive(s_b, f, n)); // to us
    n = frame(f, k_bcast, k_other, 46);
    ASSERT_TRUE(bmac_receive(s_b, f, n)); // broadcast
    static const uint8_t elsewhere[6] = {0x00, 0x05, 0x02, 0x00, 0x00, 0x01};
    n = frame(f, elsewhere, k_other, 46);
    ASSERT_TRUE(!bmac_receive(s_b, f, n)); // someone else's
    static const uint8_t mcast[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF}; // AppleTalk broadcast
    n = frame(f, mcast, k_other, 46);
    ASSERT_TRUE(!bmac_receive(s_b, f, n)); // hash table empty
    for (uint32_t r = BMAC_HASH3; r <= BMAC_HASH0; r += 0x10)
        bmac_write(s_b, r, 0xFFFF);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    // Our own frames come back only without RxRejectOwnPackets.
    n = frame(f, k_bcast, k_mac, 46);
    ASSERT_TRUE(!bmac_receive(s_b, f, n));
    bmac_write(s_b, BMAC_RXCFG, 0x0901);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
    // Promiscuous takes anyone's.
    bmac_write(s_b, BMAC_RXCFG, 0x0B41);
    n = frame(f, elsewhere, k_other, 46);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
}

// Loopback (XIFC XIFLoopback): a transmitted frame comes back through the
// receive filter — except the drivers' self-addressed warm-up frame,
// which RxRejectOwnPackets keeps out.
TEST(test_loopback) {
    fixture();
    chip_init(0x0B00);
    bmac_write(s_b, BMAC_XIFC, 0x0003); // TxOutputEnable | XIFLoopback
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2000, 0);
    desc(0x210, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(RX, 0x200);

    uint8_t f[64];
    int n = frame(f, k_mac, k_mac, 46); // OpenBSD's: to and from itself
    memcpy(s_mem + 0x1000, f, (size_t)n);
    desc(0x100, op(OUTPUT_LAST, NEVER, NEVER, NEVER, (uint32_t)n), 0x1000, 0);
    desc(0x110, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(TX, 0x100);
    ASSERT_EQ_INT((int)bmac_tx_frames(s_b), 1);
    ASSERT_EQ_INT((int)bmac_rx_frames(s_b), 0);

    n = frame(f, k_mac, k_other, 46);
    memcpy(s_mem + 0x1000, f, (size_t)n);
    desc(0x100, op(OUTPUT_LAST, NEVER, NEVER, NEVER, (uint32_t)n), 0x1000, 0);
    start(TX, 0x100);
    ASSERT_EQ_INT((int)bmac_tx_frames(s_b), 2);
    ASSERT_EQ_INT((int)bmac_rx_frames(s_b), 1);
    ASSERT_EQ_INT(xfer_status(0x200), 0x8440);
    ASSERT_TRUE(memcmp(s_mem + 0x2000, f, (size_t)n) == 0);

    // Loopback off: the frame goes to the wire, nothing comes back.
    bmac_write(s_b, BMAC_XIFC, 0x0001);
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2000, 0);
    start(RX, 0x200);
    start(TX, 0x100);
    ASSERT_EQ_INT((int)bmac_tx_frames(s_b), 3);
    ASSERT_EQ_INT((int)bmac_rx_frames(s_b), 1);
}

TEST(test_receive_overflow) {
    fixture();
    chip_init(0x0B00);
    bmac_write(s_b, BMAC_INTDISABLE, 0xFFDF); // only RxOverFlow
    uint8_t f[64];
    int n = frame(f, k_bcast, k_other, 46);
    int queued = 0;
    while (bmac_receive(s_b, f, n))
        queued++;
    ASSERT_TRUE(queued > 0);
    ASSERT_EQ_INT(s_irq_level, 1);
    ASSERT_EQ_INT(bmac_read(s_b, BMAC_STATUS) & BMAC_ST_RX_OVERFLOW, BMAC_ST_RX_OVERFLOW);
    // RXRST drops what was waiting.
    bmac_write(s_b, BMAC_RXRST, 0);
    ASSERT_TRUE(bmac_receive(s_b, f, n));
}

TEST(test_checkpoint_roundtrip) {
    fixture();
    chip_init(0x0B00);
    uint8_t f[64];
    int n = frame(f, k_bcast, k_other, 46);
    ASSERT_TRUE(bmac_receive(s_b, f, n)); // one frame waiting
    s_cp_w = s_cp_r = 0;
    bmac_checkpoint(s_b, NULL);
    bmac_t *r = bmac_init((checkpoint_t *)1, k_mac);
    ASSERT_EQ_INT(s_cp_r, s_cp_w);
    ASSERT_EQ_INT(bmac_read(r, BMAC_RXCFG), 0x0B01);
    bmac_attach_dbdma(r, s_d, TX, RX);
    desc(0x200, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 1520), 0x2000, 0);
    start(RX, 0x200);
    ASSERT_EQ_INT(xfer_status(0x200), 0x8440); // the restored frame arrives
    bmac_delete(r);
    bmac_attach_dbdma(s_b, s_d, TX, RX);
}

int main(void) {
    RUN(test_srom_station_address);
    RUN(test_registers);
    RUN(test_transmit_apple_slot);
    RUN(test_transmit_disabled_sends_nothing);
    RUN(test_receive_ends_descriptor_with_trailer);
    RUN(test_receive_apple_two_segment_slots);
    RUN(test_receive_exact_fill);
    RUN(test_receive_filter);
    RUN(test_loopback);
    RUN(test_receive_overflow);
    RUN(test_checkpoint_roundtrip);
    if (s_b)
        bmac_delete(s_b);
    if (s_d)
        dbdma_delete(s_d);
    return 0;
}
