// NCR 53C96 chip-model tests (Quadra proposal Phase E).
//
// scsi_53c96.c drives its bus through the external-initiator API
// (scsi_external_* / scsi_pop_data_in_byte / scsi_push_data_out_byte /
// scsi_get_bus_phase).  This test provides a mock implementation of that
// API backed by a tiny scripted target: a single device that answers a
// READ(6) with block data, so the whole select → CDB → pseudo-DMA read →
// status/message → disconnect flow the boot ROM performs is exercised
// deterministically without the real scsi.c / image.c stack.

#include "scsi_53c96.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Bus phase values must match scsi.h's scsi_phase_t ordering.
enum {
    MB_free = 0,
    MB_arbitration,
    MB_selection,
    MB_reselection,
    MB_command,
    MB_data_in,
    MB_data_out,
    MB_status,
    MB_message_in,
    MB_message_out
};

// ============================================================
// Mock bus + one scripted target
// ============================================================

#define MOCK_BLOCK 512
static struct {
    int phase;
    bool present[8];
    int target;
    uint8_t cdb[16];
    int cdb_len;
    uint8_t data[64 * 1024];
    size_t data_len;
    size_t data_pos;
    uint8_t status;
    bool status_taken;
} mb;

static void mock_reset(void) {
    memset(&mb, 0, sizeof(mb));
    mb.phase = MB_free;
    mb.present[0] = true; // one disk at ID 0
}

// Fill the data buffer with `tl` blocks of a recognizable pattern
// (block b, byte i -> (b*7 + i) & 0xFF) and enter DATA IN.
static void mock_fill_read(uint32_t lba, uint32_t tl) {
    mb.data_len = (size_t)tl * MOCK_BLOCK;
    if (mb.data_len > sizeof(mb.data))
        mb.data_len = sizeof(mb.data);
    for (size_t i = 0; i < mb.data_len; i++) {
        uint32_t blk = lba + (uint32_t)(i / MOCK_BLOCK);
        mb.data[i] = (uint8_t)(blk * 7 + (i % MOCK_BLOCK));
    }
    mb.phase = MB_data_in;
}

// Run the received CDB: READ(6)/READ(10) return pattern blocks (see
// mock_fill_read); everything else → GOOD with no data.
static void mock_run_cdb(void) {
    mb.data_pos = 0;
    mb.data_len = 0;
    mb.status = 0x00; // GOOD
    if (mb.cdb[0] == 0x08) { // READ(6)
        uint32_t lba = ((uint32_t)(mb.cdb[1] & 0x1F) << 16) | ((uint32_t)mb.cdb[2] << 8) | mb.cdb[3];
        mock_fill_read(lba, mb.cdb[4] ? mb.cdb[4] : 256);
    } else if (mb.cdb[0] == 0x28) { // READ(10)
        uint32_t lba =
            ((uint32_t)mb.cdb[2] << 24) | ((uint32_t)mb.cdb[3] << 16) | ((uint32_t)mb.cdb[4] << 8) | mb.cdb[5];
        mock_fill_read(lba, ((uint32_t)mb.cdb[7] << 8) | mb.cdb[8]);
    } else if (mb.cdb[0] == 0x0A) { // WRITE(6) -- the target now expects bytes
        uint32_t tl = mb.cdb[4] ? mb.cdb[4] : 256;
        mb.data_len = (size_t)tl * MOCK_BLOCK;
        if (mb.data_len > sizeof(mb.data))
            mb.data_len = sizeof(mb.data);
        mb.phase = MB_data_out;
    } else {
        mb.phase = MB_status; // no data phase
    }
}

// --- external-initiator API the chip calls (mock implementations) ---

bool scsi_external_select(struct scsi *bus, int target) {
    (void)bus;
    if (target < 0 || target > 7 || !mb.present[target])
        return false;
    mb.target = target;
    mb.cdb_len = 0;
    mb.phase = MB_command;
    return true;
}

int scsi_get_bus_phase(const struct scsi *bus) {
    (void)bus;
    return mb.phase;
}

void scsi_push_data_out_byte(struct scsi *bus, uint8_t byte) {
    (void)bus;
    if (mb.phase == MB_command) {
        if (mb.cdb_len < (int)sizeof(mb.cdb))
            mb.cdb[mb.cdb_len++] = byte;
        int need = mb.cdb[0] < 0x20 ? 6 : 10;
        if (mb.cdb_len >= need)
            mock_run_cdb();
    } else if (mb.phase == MB_data_out) {
        // Record the payload in arrival order so a test can prove not just
        // that the right bytes arrived but that they arrived in sequence.
        if (mb.data_pos < mb.data_len)
            mb.data[mb.data_pos++] = byte;
        if (mb.data_pos >= mb.data_len)
            mb.phase = MB_status;
    }
}

bool scsi_pop_data_in_byte(struct scsi *bus, uint8_t *out) {
    (void)bus;
    if (mb.phase != MB_data_in || mb.data_pos >= mb.data_len)
        return false;
    *out = mb.data[mb.data_pos++];
    if (mb.data_pos >= mb.data_len)
        mb.phase = MB_status; // last byte consumed: target moves to STATUS
    return true;
}

void scsi_external_data_in_complete(struct scsi *bus) {
    (void)bus;
    if (mb.phase == MB_data_in && mb.data_pos >= mb.data_len)
        mb.phase = MB_status;
}

int scsi_external_status_byte(struct scsi *bus) {
    (void)bus;
    if (mb.phase != MB_status)
        return -1;
    mb.phase = MB_message_in;
    return mb.status;
}

int scsi_external_message_byte(struct scsi *bus) {
    (void)bus;
    if (mb.phase != MB_message_in)
        return -1;
    return 0x00; // COMMAND COMPLETE
}

void scsi_external_release(struct scsi *bus) {
    (void)bus;
    mb.phase = MB_free;
}

// The device side of a bus reset (F-17): the chip calls it, the bus implements
// it.  This suite drives the 53C96 against a mock bus with no scsi_t behind it,
// so there are no targets to return to a power-on state -- count the call, so a
// test can assert the chip made it.
int g_bus_resets;
void scsi_bus_reset(struct scsi *bus) {
    (void)bus;
    g_bus_resets++;
}

// The selection time-out wait belongs to the bus (F-18); the chip supplies the
// period and the reporting.  This suite has no bus and no scheduler, so there
// is no time for a wait to pass in -- report straight away, which is what the
// real helper does when it finds no scheduler underneath.
void scsi_bus_arm_select_timeout(struct scsi *bus, uint64_t ns, void (*fn)(void *), void *ctx) {
    (void)bus, (void)ns;
    if (fn)
        fn(ctx);
}

void scsi_bus_cancel_select_timeout(struct scsi *bus) {
    (void)bus;
}

// ============================================================
// Chip register helpers
// ============================================================

#define R_XFER_LO   0x0
#define R_XFER_HI   0x1
#define R_FIFO      0x2
#define R_COMMAND   0x3
#define R_STATUS    0x4
#define R_INTERRUPT 0x5
#define R_FIFOFLAGS 0x7

static scsi_53c96_t *chip;
static bool irq_level;
static void irq_cb(void *ctx, bool active) {
    (void)ctx;
    irq_level = active;
}

static uint8_t rd(uint32_t r) {
    return scsi_53c96_read(chip, r);
}
static void wr(uint32_t r, uint8_t v) {
    scsi_53c96_write(chip, r, v);
}

// Read + clear the interrupt register, returning its bits.
static uint8_t take_int(void) {
    return rd(R_INTERRUPT);
}

static void setup(void) {
    mock_reset();
    chip = scsi_53c96_init(NULL, 25000000, NULL);
    ASSERT_TRUE(chip != NULL);
    scsi_53c96_set_irq_callback(chip, irq_cb, NULL);
    scsi_53c96_attach_bus(chip, (struct scsi *)&mb);
}
static void teardown(void) {
    scsi_53c96_delete(chip);
    chip = NULL;
}

// ============================================================
// Tests
// ============================================================

TEST(reset_defaults) {
    setup();
    // After chip reset the status/interrupt are clear and INT is deasserted.
    ASSERT_EQ_INT(0, rd(R_STATUS) & 0x80);
    ASSERT_TRUE(!irq_level);
    teardown();
}

TEST(select_timeout_no_device) {
    setup();
    wr(R_STATUS, 5); // destination ID 5 (absent)
    wr(R_INTERRUPT, 0xA7); // time-out register
    wr(R_COMMAND, 0x42); // select with ATN
    // No scheduler in this harness: the time-out fires synchronously.
    ASSERT_TRUE(irq_level);
    uint8_t ir = take_int();
    ASSERT_TRUE(ir & 0x20); // disconnect (selection time-out)
    teardown();
}

// Perform a full READ(6) of `tl` blocks through the chip and verify the
// bytes drained from the pseudo-DMA aperture match the target pattern.
static void do_read(uint32_t lba, uint32_t tl) {
    // DMA select without ATN, FIFO flushed first — the boot ROM's flow:
    // the target answers and the bus enters command phase.  The select
    // sequence is still running at that point, so the chip asks for the
    // remaining command bytes with DREQ and stays silent; an interrupt here
    // would mean "the target went to an unexpected phase".
    wr(R_COMMAND, 0x01); // flush FIFO
    wr(R_STATUS, 0); // dest ID 0
    wr(R_INTERRUPT, 0xA7);
    wr(R_XFER_LO, 6);
    wr(R_XFER_HI, 0);
    wr(R_COMMAND, 0xC1); // DMA select without ATN
    ASSERT_TRUE(!irq_level);
    ASSERT_TRUE(scsi_53c96_dreq(chip));
    ASSERT_EQ_INT(MB_command, mb.phase);

    // Deliver the READ(6) CDB via the FIFO (paused select accepts it).
    uint8_t cdb[6] = {0x08, (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, (uint8_t)tl, 0x00};
    for (int i = 0; i < 6; i++)
        wr(R_FIFO, cdb[i]);
    // The CDB completes the command; the bus is now in DATA IN.
    ASSERT_EQ_INT(MB_data_in, mb.phase);
    (void)take_int();

    // Drain the data in 16-byte pseudo-DMA chunks.
    size_t total = (size_t)tl * MOCK_BLOCK;
    for (size_t off = 0; off < total; off += 16) {
        wr(R_XFER_LO, 16);
        wr(R_XFER_HI, 0);
        wr(R_COMMAND, 0x90); // DMA Transfer Information
        // DRQ asserted; drain 8 words through the aperture.
        for (int w = 0; w < 8; w++) {
            uint16_t word = scsi_53c96_pdma_read16(chip);
            size_t p = off + (size_t)w * 2;
            uint32_t blk0 = (uint32_t)(lba + p / MOCK_BLOCK);
            uint32_t blk1 = (uint32_t)(lba + (p + 1) / MOCK_BLOCK);
            uint8_t exp_hi = (uint8_t)(blk0 * 7 + (p % MOCK_BLOCK));
            uint8_t exp_lo = (uint8_t)(blk1 * 7 + ((p + 1) % MOCK_BLOCK));
            ASSERT_EQ_INT(exp_hi, (word >> 8) & 0xFF);
            ASSERT_EQ_INT(exp_lo, word & 0xFF);
        }
        (void)take_int(); // per-chunk bus-service interrupt
    }

    // Target moved to STATUS; ICCS pulls status + COMMAND COMPLETE.
    wr(R_COMMAND, 0x11); // initiator command complete sequence
    ASSERT_TRUE(irq_level);
    (void)take_int();
    uint8_t status = rd(R_FIFO); // GOOD
    ASSERT_EQ_INT(0x00, status);
    uint8_t msg = rd(R_FIFO); // COMMAND COMPLETE
    ASSERT_EQ_INT(0x00, msg);

    // Message accepted → target disconnects, bus free.
    wr(R_COMMAND, 0x12);
    ASSERT_EQ_INT(MB_free, mb.phase);
    (void)take_int();
}

TEST(read6_single_block) {
    setup();
    do_read(0, 1);
    teardown();
}

TEST(read6_multi_block) {
    setup();
    do_read(4, 3); // 3 blocks = 1536 bytes across 96 chunks
    teardown();
}

TEST(read6_partial_last_chunk) {
    setup();
    // A transfer whose byte total is not a multiple of 16: request 40
    // bytes via a DMA transfer larger than the (10-byte here) target so
    // the phase-change interrupt path is exercised.  Use a scripted
    // short read: TL=0 is treated as 256 blocks by the mock, so instead
    // read one block and issue an over-long final chunk.
    wr(R_COMMAND, 0x01); // flush
    wr(R_STATUS, 0);
    wr(R_INTERRUPT, 0xA7);
    wr(R_XFER_LO, 6);
    wr(R_XFER_HI, 0);
    wr(R_COMMAND, 0xC1); // DMA select without ATN
    (void)take_int();
    uint8_t cdb[6] = {0x08, 0, 0, 0, 1, 0}; // READ block 0, 1 block
    for (int i = 0; i < 6; i++)
        wr(R_FIFO, cdb[i]);
    (void)take_int();

    // Drain 31 full 16-byte chunks (496 bytes), then a final chunk that
    // asks for 32 bytes when only 16 remain: the chip transfers the 16
    // available bytes (byte-correct), the target runs out, and the
    // completion (bus-service) interrupt fires as the CPU reads past the
    // last real byte — the counter-driven, drain-completion timing both
    // ROM drain shapes rely on.
    for (int c = 0; c < 31; c++) {
        wr(R_XFER_LO, 16);
        wr(R_XFER_HI, 0);
        wr(R_COMMAND, 0x90);
        for (int w = 0; w < 8; w++)
            (void)scsi_53c96_pdma_read16(chip);
        (void)take_int();
    }
    // Over-long final chunk: count 32, only 16 bytes remain in the block.
    wr(R_XFER_LO, 32);
    wr(R_XFER_HI, 0);
    irq_level = false;
    wr(R_COMMAND, 0x90);
    // No interrupt yet — the transfer is armed; the CPU must drain.
    ASSERT_TRUE(!irq_level);
    // The 16 real bytes come out byte-correct, then reads past the end
    // return 0 and the completion interrupt fires.
    for (int b = 0; b < 16; b++) {
        uint8_t got = scsi_53c96_pdma_read8(chip);
        ASSERT_EQ_INT((uint8_t)(0 * 7 + (496 + b)), got);
    }
    (void)scsi_53c96_pdma_read8(chip); // read past end → target ran out
    ASSERT_TRUE(irq_level);
    teardown();
}

// Odd-length pseudo-DMA read: the 53C96 reserves the trailing byte in its
// FIFO, so the completion interrupt fires once (count-1) bytes have drained
// through the aperture and the final byte is read from the FIFO register.
// This is exactly the Duff's-device blind-drain shape the boot driver uses.
TEST(read_odd_length_fifo_residual) {
    setup();
    wr(R_COMMAND, 0x01); // flush
    wr(R_STATUS, 0);
    wr(R_INTERRUPT, 0xA7);
    wr(R_XFER_LO, 6);
    wr(R_XFER_HI, 0);
    wr(R_COMMAND, 0xC1); // DMA select without ATN
    (void)take_int();
    uint8_t cdb[6] = {0x08, 0, 0, 0, 1, 0}; // READ block 0, 1 block
    for (int i = 0; i < 6; i++)
        wr(R_FIFO, cdb[i]);
    (void)take_int();
    ASSERT_EQ_INT(MB_data_in, mb.phase);

    // Count 15 (odd): 14 bytes drain through the aperture, one is reserved.
    wr(R_XFER_LO, 15);
    wr(R_XFER_HI, 0);
    irq_level = false;
    wr(R_COMMAND, 0x90);
    ASSERT_TRUE(!irq_level); // armed; nothing drained yet
    for (int b = 0; b < 14; b++) {
        uint8_t got = scsi_53c96_pdma_read8(chip);
        ASSERT_EQ_INT((uint8_t)b, got); // block 0 byte i == i
    }
    // Completion interrupt fires at the residual (14 drained, 1 reserved).
    ASSERT_TRUE(irq_level);
    (void)take_int();
    // The reserved odd byte is delivered from the FIFO register.
    uint8_t last = rd(R_FIFO);
    ASSERT_EQ_INT((uint8_t)14, last);
    teardown();
}

// The status register's low three bits track the live bus phase on every
// read (they are combinational from the phase lines on real silicon), so a
// driver polling status after a select sees COMMAND phase, then DATA IN once
// the CDB is fed — without issuing another chip command in between.
TEST(status_reflects_live_phase) {
    setup();
    wr(R_COMMAND, 0x01); // flush
    wr(R_STATUS, 0);
    wr(R_INTERRUPT, 0xA7);
    wr(R_XFER_LO, 6);
    wr(R_XFER_HI, 0);
    wr(R_COMMAND, 0xC1); // DMA select without ATN → stops in command phase
    (void)take_int();
    ASSERT_EQ_INT(0x2, rd(R_STATUS) & 0x07); // COMMAND phase (010)
    uint8_t cdb[6] = {0x08, 0, 0, 0, 1, 0};
    for (int i = 0; i < 6; i++)
        wr(R_FIFO, cdb[i]);
    ASSERT_EQ_INT(0x1, rd(R_STATUS) & 0x07); // DATA IN (001) on the next read
    teardown();
}

// The System 7.1 HD driver's READ(10) flow: DMA select without ATN (chip
// pauses in COMMAND phase awaiting the CDB), then Flush FIFO, then the
// 10-byte CDB pushed through the pseudo-DMA aperture.  The flush must not
// abandon the paused select sequence — dropping the CDB left the target in
// COMMAND phase forever and every post-boot SCSI Manager read timed out.
TEST(flush_preserves_paused_select) {
    setup();
    wr(R_COMMAND, 0x01); // flush FIFO
    wr(R_STATUS, 0); // dest ID 0
    wr(R_INTERRUPT, 0xA7);
    wr(R_COMMAND, 0xC1); // DMA select without ATN → pauses in command phase
    ASSERT_TRUE(!irq_level); // DREQ, not an interrupt (see do_read)
    ASSERT_EQ_INT(MB_command, mb.phase);

    wr(R_COMMAND, 0x01); // flush FIFO again — must keep the select paused

    // READ(10), lba=2, tl=1, via the aperture (byte-wide driver push).
    uint8_t cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00};
    for (int i = 0; i < 10; i++)
        scsi_53c96_pdma_write8(chip, cdb[i]);

    // The full CDB reached the target: bus left COMMAND for DATA IN and
    // the select-complete interrupt fired.
    ASSERT_EQ_INT(MB_data_in, mb.phase);
    ASSERT_EQ_INT(10, mb.cdb_len);
    ASSERT_EQ_INT(0x28, mb.cdb[0]);
    ASSERT_TRUE(irq_level);
    (void)take_int();
    ASSERT_EQ_INT(0x1, rd(R_STATUS) & 0x07); // DATA IN phase visible
    teardown();
}

// A DATA OUT transfer whose first byte the driver preloaded into the FIFO.
//
// This is the Quadra ROM's Duff's-device blind write, read off a live Q700 at
// $00096400:
//
//     SUBQ.L #$1,D2           ; take one byte off the count...
//     MOVE.B (A2)+,$20(A3)    ; ...and preload it into the FIFO register
//     BRA    loop             ; then program the (now even) count and DMA the rest
//
// The FIFO is the chip's datapath, not a side buffer, so that byte is the
// first one on the wire.  We used to leave it sitting in the FIFO and send
// only the DMA stream: every block lost its leading byte, the FIFO filled
// after 16 of them and never drained, and every later preload set ST_GE --
// "the top of the FIFO is overwritten", the manual's gross error.  Measured on
// suite-quadra: 64 spurious gross errors, 124 status reads that saw the bit,
// and 75 whole blocks of write traffic the driver retried because of it.
TEST(dma_data_out_sends_the_byte_preloaded_into_the_fifo) {
    setup();

    // Select and deliver WRITE(6) of one block.
    wr(R_COMMAND, 0x01); // flush FIFO
    wr(R_STATUS, 0); // dest ID 0
    wr(R_INTERRUPT, 0xA7);
    wr(R_XFER_LO, 6);
    wr(R_XFER_HI, 0);
    wr(R_COMMAND, 0xC1); // DMA select without ATN
    uint8_t cdb[6] = {0x0A, 0, 0, 0, 1, 0};
    for (int i = 0; i < 6; i++)
        wr(R_FIFO, cdb[i]);
    ASSERT_EQ_INT(MB_data_out, mb.phase);
    (void)take_int();

    // The driver's shape: one byte into the FIFO, count = 511 for the rest.
    wr(R_FIFO, 0xA5);
    ASSERT_EQ_INT(1, rd(R_FIFOFLAGS) & 0x1F); // the chip is holding it
    wr(R_XFER_LO, (uint8_t)((MOCK_BLOCK - 1) & 0xFF));
    wr(R_XFER_HI, (uint8_t)((MOCK_BLOCK - 1) >> 8));
    wr(R_COMMAND, 0x90); // DMA Transfer Information

    // Arming the transfer must have put the preloaded byte on the wire, ahead
    // of anything the aperture carries, and emptied the FIFO.
    ASSERT_EQ_INT(1, (int)mb.data_pos);
    ASSERT_EQ_INT(0xA5, mb.data[0]);
    ASSERT_EQ_INT(0, rd(R_FIFOFLAGS) & 0x1F);
    ASSERT_TRUE(!(rd(R_STATUS) & 0x40)); // and no gross error

    // The remaining 511 arrive through the aperture, in order behind it.
    for (int i = 1; i < MOCK_BLOCK; i++)
        scsi_53c96_pdma_write8(chip, (uint8_t)(i * 3 + 1));
    ASSERT_EQ_INT(MOCK_BLOCK, (int)mb.data_pos); // the block is complete
    ASSERT_EQ_INT(0xA5, mb.data[0]);
    for (int i = 1; i < MOCK_BLOCK; i++)
        ASSERT_EQ_INT((uint8_t)(i * 3 + 1), mb.data[i]);
}

// Sixteen preloaded blocks in a row used to be exactly what it took to wedge
// the FIFO, so the seventeenth raised a gross error the guest could see.
TEST(repeated_preloads_do_not_wedge_the_fifo) {
    setup();

    for (int round = 0; round < 20; round++) {
        wr(R_COMMAND, 0x01);
        wr(R_STATUS, 0);
        wr(R_INTERRUPT, 0xA7);
        wr(R_XFER_LO, 6);
        wr(R_XFER_HI, 0);
        wr(R_COMMAND, 0xC1);
        uint8_t cdb[6] = {0x0A, 0, 0, 0, 1, 0};
        for (int i = 0; i < 6; i++)
            wr(R_FIFO, cdb[i]);
        (void)take_int();

        wr(R_FIFO, (uint8_t)(0x40 + round));
        wr(R_XFER_LO, (uint8_t)((MOCK_BLOCK - 1) & 0xFF));
        wr(R_XFER_HI, (uint8_t)((MOCK_BLOCK - 1) >> 8));
        wr(R_COMMAND, 0x90);
        for (int i = 1; i < MOCK_BLOCK; i++)
            scsi_53c96_pdma_write8(chip, 0x5A);

        ASSERT_EQ_INT((uint8_t)(0x40 + round), mb.data[0]);
        ASSERT_EQ_INT(0, rd(R_FIFOFLAGS) & 0x1F); // never accumulates
        ASSERT_TRUE(!(rd(R_STATUS) & 0x40)); // never a gross error
        (void)take_int();
        wr(R_COMMAND, 0x11);
        (void)take_int();
        (void)rd(R_FIFO);
        (void)rd(R_FIFO);
        wr(R_COMMAND, 0x12);
        (void)take_int();
    }
}

int main(void) {
    RUN(reset_defaults);
    RUN(select_timeout_no_device);
    RUN(read6_single_block);
    RUN(read6_multi_block);
    RUN(read6_partial_last_chunk);
    RUN(read_odd_length_fifo_residual);
    RUN(dma_data_out_sends_the_byte_preloaded_into_the_fifo);
    RUN(repeated_preloads_do_not_wedge_the_fifo);
    RUN(status_reflects_live_phase);
    RUN(flush_preserves_paused_select);
    printf("[scsi96] all tests passed\n");
    return 0;
}
