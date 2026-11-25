// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// MACE Ethernet core unit test.
//
// The real core/peripherals/mace.c with recording hooks standing in for the
// DMA engine and the cable.  Sequences follow Apple's AV driver
// (DeclData/DeclNet/Mace/Mace.a, MaceEnet.a): MaceInit's register writes,
// the IAC dance for PADR and LADRF, the level-3 handler's XMTRC-then-XMTFS
// status read, and the open-time loopback self-test's 79-byte frame.

#include "checkpoint.h"
#include "codec.h"
#include "mace.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording checkpoint stream
// ============================================================================

static uint8_t s_cp_buf[1 << 17];
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
// Hooks: INTR, the cable, the DMA engine's request lines
// ============================================================================

static bool s_irq;
static int s_wire_frames, s_wire_len;
static uint8_t s_wire[MACE_FRAME_MAX];
static int s_rx_ready, s_tx_ready;

static void on_irq(void *ctx, bool level) {
    (void)ctx;
    s_irq = level;
}
static void on_wire(void *ctx, const uint8_t *frame, int len) {
    (void)ctx;
    s_wire_frames++;
    s_wire_len = len;
    memcpy(s_wire, frame, (size_t)len);
}
static void on_rx_ready(void *ctx) {
    (void)ctx;
    s_rx_ready++;
}
static void on_tx_ready(void *ctx) {
    (void)ctx;
    s_tx_ready++;
}

static const uint8_t k_mac[6] = {0x02, 0x00, 0x00, 0x09, 0x07, 0x02};
static const uint8_t k_other[6] = {0x02, 0x00, 0x00, 0x47, 0x53, 0x21};
static const uint8_t k_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t k_atalk_bcast[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};

static mace_t *s_m;

// A fresh core with the hooks bound and the counters cleared.
static mace_t *fresh(void) {
    if (s_m)
        mace_delete(s_m);
    s_m = mace_init();
    ASSERT_TRUE(s_m != NULL);
    mace_set_irq(s_m, on_irq, NULL);
    static const mace_ops_t ops = {.wire_tx = on_wire, .rx_ready = on_rx_ready, .tx_ready = on_tx_ready};
    mace_set_ops(s_m, &ops, NULL);
    s_irq = false;
    s_wire_frames = s_wire_len = 0;
    s_rx_ready = s_tx_ready = 0;
    return s_m;
}

// MaceInit's configuration (Mace.a / MACEecfg.r): UTR $40, IMR $77, BIUCC
// $20, XMTFC $01, RCVFC $00, FIFOCC $2C, PLSCC $00, PADR via IAC, LADRF
// zeroed via IAC, MACCC $03, IMR $02.
static void driver_init(mace_t *m) {
    mace_write(m, MACE_UTR, 0x40);
    mace_write(m, MACE_IMR, 0x77);
    mace_write(m, MACE_BIUCC, 0x20);
    mace_write(m, MACE_XMTFC, 0x01);
    mace_write(m, MACE_RCVFC, 0x00);
    mace_write(m, MACE_FIFOCC, 0x2C);
    mace_write(m, MACE_PLSCC, 0x00);
    mace_write(m, MACE_IAC, 0x04); // PHYADDR
    for (int i = 0; i < 6; i++)
        mace_write(m, MACE_PADR, k_mac[i]);
    mace_write(m, MACE_IAC, 0x02); // LOGADDR
    for (int i = 0; i < 8; i++)
        mace_write(m, MACE_LADRF, 0x00);
    mace_write(m, MACE_MACCC, 0x03);
    mace_write(m, MACE_IMR, 0x02); // only RCVINT masked
}

// An 802.3/Ethernet frame: header then `payload` bytes of a counting
// pattern; returns its length.
static int make_frame(uint8_t *f, const uint8_t *dst, const uint8_t *src, int payload) {
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x80;
    f[13] = 0x9B;
    for (int i = 0; i < payload; i++)
        f[14 + i] = (uint8_t)(i * 7 + 1);
    return 14 + payload;
}

// The level-3 handler's status read: XMTRC first, then XMTFS.
static uint8_t read_xmt_status(mace_t *m) {
    (void)mace_read(m, MACE_XMTRC);
    return mace_read(m, MACE_XMTFS);
}

// ============================================================================
// Tests
// ============================================================================

// Reset values (data manual p. 74) and Curio's chip ID.
TEST(test_reset_values_and_chip_id) {
    mace_t *m = fresh();
    ASSERT_EQ_HEX(mace_read(m, MACE_CHIPIDLO), 0x40);
    ASSERT_EQ_HEX(mace_read(m, MACE_CHIPIDHI), 0x09);
    ASSERT_EQ_HEX(mace_read(m, MACE_XMTFC), 0x01);
    ASSERT_EQ_HEX(mace_read(m, MACE_RCVFC), 0x01);
    ASSERT_EQ_HEX(mace_read(m, MACE_BIUCC), 0x20);
    ASSERT_EQ_HEX(mace_read(m, MACE_FIFOCC), 0x20);
    ASSERT_EQ_HEX(mace_read(m, MACE_PHYCC), 0x80);
    ASSERT_EQ_HEX(mace_read(m, MACE_MACCC), 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_IR), 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_MPC), 0x00);
    ASSERT_TRUE(!s_irq);
    // The chip ID ignores writes.
    mace_write(m, MACE_CHIPIDLO, 0x55);
    ASSERT_EQ_HEX(mace_read(m, MACE_CHIPIDLO), 0x40);
}

// SWRST and the FIFO watermark-update bits clear themselves; RTRD sticks.
TEST(test_self_clearing_bits) {
    mace_t *m = fresh();
    mace_write(m, MACE_MACCC, 0x03);
    mace_write(m, MACE_BIUCC, 0x21); // SWRST, as ResetMACE writes it
    ASSERT_EQ_HEX(mace_read(m, MACE_BIUCC), 0x20); // the reset value: SWRST read back clear
    ASSERT_EQ_HEX(mace_read(m, MACE_MACCC), 0x00); // the reset turned RX/TX off
    mace_write(m, MACE_FIFOCC, 0x2C);
    ASSERT_EQ_HEX(mace_read(m, MACE_FIFOCC), 0x20); // XMTFWU/RCVFWU loaded and cleared
    mace_write(m, MACE_UTR, 0x40); // RTRD
    mace_write(m, MACE_UTR, 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_UTR), 0x40);
}

// PADR and LADRF load LS byte first behind PHYADDR / LOGADDR, which clear
// after six / eight accesses; ADDRCHG stops the receiver.
TEST(test_iac_address_ram) {
    mace_t *m = fresh();
    driver_init(m);
    ASSERT_EQ_HEX(mace_read(m, MACE_IAC), 0x00); // both pointers ran out
    uint8_t mac[6];
    mace_get_padr(m, mac);
    ASSERT_TRUE(memcmp(mac, k_mac, 6) == 0);
    mace_write(m, MACE_IAC, 0x04);
    ASSERT_EQ_HEX(mace_read(m, MACE_IAC), 0x04);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ_HEX(mace_read(m, MACE_PADR), k_mac[i]);
    ASSERT_EQ_HEX(mace_read(m, MACE_IAC), 0x00);
    // WriteLAF: ADDRCHG (polled clear at once) clears ENRCV; MACCC is put back.
    mace_write(m, MACE_IAC, 0x82);
    ASSERT_EQ_HEX(mace_read(m, MACE_IAC) & 0x80, 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_MACCC), 0x02);
}

// One frame transmitted: padded to 60 bytes on the cable, XMTSV in XMTFS
// (and in PR), XMTINT on the pin; a second frame waits until the first's
// status is read -- Curio's one-deep status buffer.
TEST(test_transmit_one_deep_status) {
    mace_t *m = fresh();
    driver_init(m);
    ASSERT_TRUE(mace_tx_ready(m));
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_other, k_mac, 20);
    mace_tx_write(m, f, n);
    mace_tx_eof(m);
    ASSERT_EQ_INT(s_wire_frames, 1);
    ASSERT_EQ_INT(s_wire_len, 60); // APADXMT
    ASSERT_TRUE(memcmp(s_wire, f, (size_t)n) == 0);
    ASSERT_TRUE(s_irq); // XMTINT is not masked by IMR $02
    ASSERT_EQ_HEX(mace_peek(m, MACE_PR) & 0xC0, 0x80); // XMTSV, no TDTREQ
    ASSERT_TRUE(!mace_tx_ready(m));

    // A second frame while the status is unread is held, not sent.
    mace_tx_write(m, f, n);
    mace_tx_eof(m);
    ASSERT_EQ_INT(s_wire_frames, 1);

    // MaceInterrupt: IR (read-to-clear), then XMTRC, XMTFS.
    ASSERT_EQ_HEX(mace_read(m, MACE_IR), 0x01);
    ASSERT_TRUE(!s_irq);
    ASSERT_EQ_HEX(read_xmt_status(m), 0x80);
    ASSERT_EQ_INT(s_wire_frames, 2); // the held frame went as the status was read
    ASSERT_EQ_HEX(read_xmt_status(m), 0x80);
    ASSERT_EQ_HEX(mace_read(m, MACE_XMTFS), 0x00); // read once per frame
    ASSERT_TRUE(mace_tx_ready(m));
    ASSERT_TRUE(s_tx_ready >= 1);
    ASSERT_EQ_INT((int)mace_tx_frames(m), 2);
}

// A frame written with ENXMT clear stays in the FIFO until it is set.
TEST(test_transmit_waits_for_enxmt) {
    mace_t *m = fresh();
    driver_init(m);
    mace_write(m, MACE_MACCC, 0x01); // receive only
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_other, k_mac, 100);
    mace_tx_write(m, f, n);
    mace_tx_eof(m);
    ASSERT_EQ_INT(s_wire_frames, 0);
    ASSERT_EQ_HEX(mace_peek(m, MACE_FIFOFC) & 0x0F, 1);
    mace_write(m, MACE_MACCC, 0x03);
    ASSERT_EQ_INT(s_wire_frames, 1);
    ASSERT_EQ_INT(s_wire_len, n);
}

// The open-time self-test (MaceEnet.a + Loopback.c): a 79-byte frame to our
// own address in each loopback mode comes back 83 bytes long (FCS appended,
// RCVFCSE clear) with clean status; only EXTLPB puts it on the cable.
TEST(test_loopback_self_test_frame) {
    static const uint8_t modes[3] = {0x44, 0x46, 0x42}; // RTRD | INTLPB, MENDECLPB, EXTLPB
    for (int k = 0; k < 3; k++) {
        mace_t *m = fresh();
        driver_init(m);
        mace_write(m, MACE_UTR, modes[k]);
        uint8_t f[MACE_FRAME_MAX];
        int n = make_frame(f, k_mac, k_mac, 65);
        ASSERT_EQ_INT(n, 79);
        mace_tx_write(m, f, n);
        mace_tx_eof(m);
        ASSERT_EQ_INT(s_wire_frames, k == 2 ? 1 : 0);
        ASSERT_EQ_INT(mace_rx_pending(m), 83);
        ASSERT_TRUE(s_rx_ready >= 1);
        uint8_t got[MACE_FRAME_MAX], rfs[4];
        ASSERT_EQ_INT(mace_rx_take(m, got, (int)sizeof got, rfs), 83);
        ASSERT_TRUE(memcmp(got, f, 79) == 0);
        uint32_t fcs = codec_crc32(0, f, 79);
        ASSERT_EQ_HEX(got[79], fcs & 0xFF);
        ASSERT_EQ_HEX(got[82], fcs >> 24);
        ASSERT_EQ_HEX(rfs[0], 83); // RCVCNT[7:0]
        ASSERT_EQ_HEX(rfs[1], 0x00); // no OFLO/CLSN/FRAM/FCS, RCVCNT[11:8] = 0
        ASSERT_EQ_HEX(rfs[2], 0x00);
        ASSERT_EQ_HEX(rfs[3], 0x00);
        ASSERT_EQ_HEX(mace_read(m, MACE_IR), 0x03); // XMTINT and RCVINT
        ASSERT_EQ_INT(mace_rx_pending(m), 0);
        // Internal loopback refuses the network.
        uint8_t w[64];
        int wn = make_frame(w, k_mac, k_other, 46);
        ASSERT_TRUE(mace_wire_rx(m, w, wn) == (k == 2));
    }
}

// The independent definition of the LADRF hash (data manual p. 70; the
// reference page's §3.3): the FCS generator over the destination address,
// bits fed LSB first, no final inversion; the six high-order bits of the
// reflected result.
static int ref_hash(const uint8_t *da) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++)
        for (int b = 0; b < 8; b++) {
            uint32_t top = (crc >> 31) & 1u;
            crc <<= 1;
            if (top ^ ((da[i] >> b) & 1u))
                crc ^= 0x04C11DB7u;
        }
    int h = 0;
    for (int b = 0; b < 6; b++)
        h |= (int)((crc >> b) & 1u) << (5 - b);
    return h;
}

// Receive matching: our address, broadcast, a multicast only with its
// LADRF bit, everything when promiscuous; nothing with ENRCV clear.
TEST(test_receive_address_filter) {
    mace_t *m = fresh();
    driver_init(m);
    uint8_t f[MACE_FRAME_MAX], got[MACE_FRAME_MAX], rfs[4];
    int n = make_frame(f, k_mac, k_other, 100);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    ASSERT_EQ_INT(mace_rx_take(m, got, (int)sizeof got, rfs), n + 4); // FCS appended
    n = make_frame(f, k_bcast, k_other, 100);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    n = make_frame(f, k_other, k_mac, 100); // someone else's
    ASSERT_TRUE(!mace_wire_rx(m, f, n));
    n = make_frame(f, k_atalk_bcast, k_other, 100);
    ASSERT_TRUE(!mace_wire_rx(m, f, n)); // multicast, no filter bit
    int h = ref_hash(k_atalk_bcast);
    mace_write(m, MACE_IAC, 0x82); // ADDRCHG | LOGADDR
    for (int i = 0; i < 8; i++)
        mace_write(m, MACE_LADRF, (uint8_t)(i == (h >> 3) ? 1u << (h & 7) : 0));
    mace_write(m, MACE_MACCC, 0x03);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    mace_write(m, MACE_MACCC, 0x83); // PROM
    n = make_frame(f, k_other, k_mac, 100);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    mace_write(m, MACE_MACCC, 0x02); // receiver off
    ASSERT_TRUE(!mace_wire_rx(m, f, n));
    // A short frame arrives padded to the 60-byte minimum, plus the FCS.
    m = fresh();
    driver_init(m);
    n = make_frame(f, k_mac, k_other, 10);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    ASSERT_EQ_INT(mace_rx_pending(m), 64);
}

// ASTRPRCV strips the pad (by the 802.3 length) and the FCS.
TEST(test_receive_strip) {
    mace_t *m = fresh();
    driver_init(m);
    mace_write(m, MACE_RCVFC, 0x01);
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_mac, k_other, 20);
    f[12] = 0x00;
    f[13] = 20; // an 802.3 length
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    ASSERT_EQ_INT(mace_rx_pending(m), 34);
}

// A full receive FIFO misses frames: MPC counts them, wraps, raises MPCO,
// and reads clear it.
TEST(test_receive_overflow_counts_missed) {
    mace_t *m = fresh();
    driver_init(m);
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_mac, k_other, 100);
    for (int i = 0; i < MACE_RXQ_DEPTH; i++)
        ASSERT_TRUE(mace_wire_rx(m, f, n));
    for (int i = 0; i < 256; i++)
        ASSERT_TRUE(!mace_wire_rx(m, f, n));
    ASSERT_EQ_HEX(mace_peek(m, MACE_FIFOFC) >> 4, MACE_RXQ_DEPTH);
    ASSERT_EQ_HEX(mace_read(m, MACE_MPC), 0x00); // wrapped
    ASSERT_EQ_HEX(mace_read(m, MACE_IR) & 0x04, 0x04); // MPCO
    ASSERT_TRUE(!mace_wire_rx(m, f, n));
    ASSERT_EQ_HEX(mace_read(m, MACE_MPC), 0x01);
    ASSERT_EQ_HEX(mace_read(m, MACE_MPC), 0x00); // read-to-clear
    // RCVFWU flushes the FIFO.
    mace_write(m, MACE_FIFOCC, 0x24);
    ASSERT_EQ_INT(mace_rx_pending(m), 0);
}

// Programmed-I/O receive: RCVFIFO byte reads, then the four RCVFS reads.
TEST(test_receive_programmed_io) {
    mace_t *m = fresh();
    driver_init(m);
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_mac, k_other, 46); // 60 bytes
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    for (int i = 0; i < 64; i++)
        (void)mace_read(m, MACE_RCVFIFO);
    ASSERT_EQ_HEX(mace_read(m, MACE_IR) & 0x02, 0x02); // RCVINT at the frame's end
    ASSERT_EQ_HEX(mace_read(m, MACE_RCVFS), 64);
    ASSERT_EQ_HEX(mace_read(m, MACE_RCVFS), 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_RCVFS), 0x00);
    ASSERT_EQ_HEX(mace_read(m, MACE_RCVFS), 0x00);
    ASSERT_EQ_INT((int)mace_rx_frames(m), 1);
}

// Everything the core holds survives a checkpoint: registers, the address
// RAM, a frame waiting in the receive FIFO, a pending transmit status.
TEST(test_checkpoint_roundtrip) {
    mace_t *m = fresh();
    driver_init(m);
    uint8_t f[MACE_FRAME_MAX];
    int n = make_frame(f, k_mac, k_other, 200);
    ASSERT_TRUE(mace_wire_rx(m, f, n));
    mace_tx_write(m, f, n);
    mace_tx_eof(m);
    s_cp_w = s_cp_r = 0;
    mace_checkpoint(m, NULL);
    mace_t *r = mace_init();
    bool level = false;
    mace_restore(r, NULL);
    ASSERT_EQ_INT((int)s_cp_r, (int)s_cp_w);
    mace_set_irq(r, on_irq, &level);
    ASSERT_TRUE(s_irq); // the restored XMTINT drives the pin
    ASSERT_EQ_HEX(mace_read(r, MACE_MACCC), 0x03);
    ASSERT_EQ_HEX(mace_read(r, MACE_IMR), 0x02);
    uint8_t mac[6];
    mace_get_padr(r, mac);
    ASSERT_TRUE(memcmp(mac, k_mac, 6) == 0);
    ASSERT_EQ_HEX(read_xmt_status(r), 0x80);
    ASSERT_EQ_INT(mace_rx_pending(r), n + 4);
    uint8_t got[MACE_FRAME_MAX], rfs[4];
    ASSERT_EQ_INT(mace_rx_take(r, got, (int)sizeof got, rfs), n + 4);
    ASSERT_TRUE(memcmp(got, f, (size_t)n) == 0);
    mace_delete(r);
}

int main(void) {
    RUN(test_reset_values_and_chip_id);
    RUN(test_self_clearing_bits);
    RUN(test_iac_address_ram);
    RUN(test_transmit_one_deep_status);
    RUN(test_transmit_waits_for_enxmt);
    RUN(test_loopback_self_test_frame);
    RUN(test_receive_address_filter);
    RUN(test_receive_strip);
    RUN(test_receive_overflow_counts_missed);
    RUN(test_receive_programmed_io);
    RUN(test_checkpoint_roundtrip);
    if (s_m)
        mace_delete(s_m);
    return 0;
}
