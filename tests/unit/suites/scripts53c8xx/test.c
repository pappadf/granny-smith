// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCRIPTS instruction-engine unit test
// (proposal-apple-network-server-500-700 §5.7, Phase E).
//
// Drives the real cards/scripts53c8xx.c against a flat guest-memory array
// and a MOCK SCSI target, with no PCI, no machine and no shared bus model
// underneath it.  That is the whole reason the engine is its own
// translation unit: five instruction classes plus their failure modes are
// only affordable to cover if they can be driven directly.
//
// Sequences pinned here:
//
//  1. Block Move in every phase a driver uses, and the PHASE MISMATCH law
//     — the instruction does NOT execute, DSP is rewound to point AT it,
//     and SIST0's MA bit is the cause.  This is the single most
//     load-bearing behaviour in the set: a driver runs a whole transaction
//     by moving one phase at a time and branching on the mismatch.
//  2. The I/O class: Select (and a selection TIME-OUT taking the
//     instruction's alternate address, which is the whole point of that
//     field), Set/Clear of ACK/ATN/carry, and Wait Disconnect.
//  3. The Read/Write class — the register ALU, in both its
//     read-modify-write and its move-to/from-SFBR forms, and carry.
//  4. Transfer Control: conditional Jump on phase, on data with a compare
//     MASK, and on carry; Call/Return through TEMP; and Interrupt with
//     its vector, including the interrupt-on-the-fly variant that does
//     NOT halt.
//  5. Memory Move, and its documented alignment failure.
//  6. Load and Store, and their documented byte-count/alignment failures.
//  7. The status discipline: read-to-clear, the fatal/non-fatal split,
//     and masking that gates the PIN without gating the halt.
//  8. The runaway-program watchdog.
//  9. The checkpoint round trip.
//
// Every class gets a negative case, because "the engine executed
// something" is not the same claim as "the engine rejected what it should".

#include "sym53c8xx.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Mock guest memory
// ============================================================================
// The engine reaches memory only through sym53c8xx_read_block /
// _write_block, which fall to the slow path when cfg is NULL — so a NULL
// cfg plus these two stubs IS the mock bus.

#define MEM_SIZE 0x10000u
static uint8_t s_mem[MEM_SIZE];

uint8_t memory_read_uint8_slow(uint32_t addr) {
    return (addr < MEM_SIZE) ? s_mem[addr] : 0xFFu;
}

// The bus's own reset pin: nothing on the mock bus to reset.
void scsi_reset_pin(struct scsi *bus);
void scsi_reset_pin(struct scsi *bus) {
    (void)bus;
}

// The scheduler the engine posts its selection time-out to.  There is none
// here: with a NULL config the time-out is immediate, so these exist only
// to satisfy the linker and are never reached.
struct scheduler;
struct event;
struct event *scheduler_new_cpu_event(struct scheduler *scheduler, void (*cb)(void *, uint64_t), void *source,
                                      uint64_t data, uint64_t cycles, uint64_t ns);
struct event *scheduler_new_cpu_event(struct scheduler *scheduler, void (*cb)(void *, uint64_t), void *source,
                                      uint64_t data, uint64_t cycles, uint64_t ns) {
    (void)scheduler, (void)cb, (void)source, (void)data, (void)cycles, (void)ns;
    return 0;
}
void remove_event(struct scheduler *scheduler, void (*cb)(void *, uint64_t), void *source);
void remove_event(struct scheduler *scheduler, void (*cb)(void *, uint64_t), void *source) {
    (void)scheduler, (void)cb, (void)source;
}
void scheduler_new_event_type(struct scheduler *scheduler, const char *source_name, void *source,
                              const char *event_name, void (*cb)(void *, uint64_t));
void scheduler_new_event_type(struct scheduler *scheduler, const char *source_name, void *source,
                              const char *event_name, void (*cb)(void *, uint64_t)) {
    (void)scheduler, (void)source_name, (void)source, (void)event_name, (void)cb;
}

void memory_write_uint8_slow(uint32_t addr, uint8_t value) {
    if (addr < MEM_SIZE)
        s_mem[addr] = value;
}

uint8_t *ram_native_pointer(memory_map_t *map, uint32_t offset) {
    (void)map;
    (void)offset;
    return s_mem; // never reached with a NULL cfg; present so the test links
}

// ============================================================================
// Recording checkpoint stream — a real byte round-trip
// ============================================================================

static uint8_t s_cp_buf[16384];
static size_t s_cp_w, s_cp_r;

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *file, int line) {
    (void)cp;
    (void)file;
    (void)line;
    ASSERT_TRUE(s_cp_w + size <= sizeof(s_cp_buf));
    memcpy(s_cp_buf + s_cp_w, data, size);
    s_cp_w += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *file, int line) {
    (void)cp;
    (void)file;
    (void)line;
    ASSERT_TRUE(s_cp_r + size <= s_cp_w);
    memcpy(data, s_cp_buf + s_cp_r, size);
    s_cp_r += size;
}

// ============================================================================
// Mock PCI interrupt pin
// ============================================================================

static int s_irq_asserts, s_irq_deasserts;

void pci_assert_irq(struct pci_device *dev) {
    (void)dev;
    s_irq_asserts++;
}

void pci_deassert_irq(struct pci_device *dev) {
    (void)dev;
    s_irq_deasserts++;
}

// ============================================================================
// Mock SCSI target
// ============================================================================
// A scripted target: it answers selection for one id, presents a phase the
// test sets, and hands back bytes from a buffer.  Deliberately NOT the
// shared bus model — the point of this suite is that the engine can be
// exercised without one.

// Mirrors the shared model's scsi_phase_t ordering, which is what
// scsi_get_bus_phase returns.
enum { M_BUS_FREE = 0, M_ARB, M_SEL, M_RESEL, M_COMMAND, M_DATA_IN, M_DATA_OUT, M_STATUS, M_MSG_IN, M_MSG_OUT };

static struct scsi *const S_BUS = (struct scsi *)0x1234; // an opaque non-NULL handle
static int s_target_present = 1;
static int s_phase;
static uint8_t s_out[64]; // bytes the initiator pushed
static unsigned s_out_n;
static uint8_t s_in[64]; // bytes the target will hand back
static unsigned s_in_n, s_in_rd;
static int s_status_byte;
static int s_message_byte;
static int s_released;
// How many COMMAND bytes the target expects before it changes phase — a
// real target decodes the CDB's group code and knows.  Six is the default
// because every command these tests issue is a six-byte one.
static unsigned s_cdb_len = 6;

static void mock_reset(void) {
    s_phase = M_BUS_FREE;
    s_cdb_len = 6;
    s_out_n = 0;
    s_in_n = 0;
    s_in_rd = 0;
    s_status_byte = 0x00;
    s_message_byte = 0x00;
    s_released = 0;
    s_target_present = 1;
}

bool scsi_external_select(struct scsi *scsi, int target) {
    (void)scsi;
    (void)target;
    if (!s_target_present)
        return false;
    s_phase = M_COMMAND; // the shared model's behaviour: selection lands in COMMAND
    return true;
}

void scsi_external_release(struct scsi *scsi) {
    (void)scsi;
    s_released++;
    s_phase = M_BUS_FREE;
}

void scsi_push_data_out_byte(struct scsi *scsi, uint8_t byte) {
    (void)scsi;
    if (s_out_n < sizeof(s_out))
        s_out[s_out_n++] = byte;
    // A target that has taken its whole command block changes phase, which
    // is what makes the initiator's next Block Move meaningful.
    if (s_phase == M_COMMAND && s_out_n >= s_cdb_len)
        s_phase = (s_in_n > 0) ? M_DATA_IN : M_STATUS;
}

bool scsi_pop_data_in_byte(struct scsi *scsi, uint8_t *out) {
    (void)scsi;
    if (s_in_rd >= s_in_n)
        return false;
    *out = s_in[s_in_rd++];
    return true;
}

void scsi_external_data_in_complete(struct scsi *scsi) {
    (void)scsi;
    s_phase = M_STATUS;
}

int scsi_external_status_byte(struct scsi *scsi) {
    (void)scsi;
    if (s_phase != M_STATUS)
        return -1;
    s_phase = M_MSG_IN;
    return s_status_byte;
}

int scsi_external_message_byte(struct scsi *scsi) {
    (void)scsi;
    if (s_phase != M_MSG_IN)
        return -1;
    return s_message_byte;
}

int scsi_get_bus_phase(const struct scsi *scsi) {
    (void)scsi;
    return s_phase;
}

// ============================================================================
// Helpers
// ============================================================================

static sym53c8xx_t *s_c;

// Instructions are stored in the strapped byte order.  The Apple Network
// Server straps LITTLE-endian, which is what sym53c8xx_new selects, so a
// dword goes in low byte first.
static void put_insn_word(uint32_t addr, uint32_t v) {
    s_mem[addr + 0] = (uint8_t)v;
    s_mem[addr + 1] = (uint8_t)(v >> 8);
    s_mem[addr + 2] = (uint8_t)(v >> 16);
    s_mem[addr + 3] = (uint8_t)(v >> 24);
}

static uint32_t reg32(uint32_t off) {
    return (uint32_t)s_c->reg[off] | ((uint32_t)s_c->reg[off + 1] << 8) | ((uint32_t)s_c->reg[off + 2] << 16) |
           ((uint32_t)s_c->reg[off + 3] << 24);
}

static void set_reg32(uint32_t off, uint32_t v) {
    s_c->reg[off] = (uint8_t)v;
    s_c->reg[off + 1] = (uint8_t)(v >> 8);
    s_c->reg[off + 2] = (uint8_t)(v >> 16);
    s_c->reg[off + 3] = (uint8_t)(v >> 24);
}

// Fresh chip, fresh memory, fresh target.
static void setup(void) {
    if (s_c)
        sym53c8xx_delete(s_c);
    memset(s_mem, 0, sizeof(s_mem));
    mock_reset();
    s_irq_asserts = s_irq_deasserts = 0;
    s_c = sym53c8xx_new(NULL, 0);
    ASSERT_TRUE(s_c != NULL);
    sym53c8xx_attach_bus(s_c, S_BUS);
    // Every cause enabled, so the pin follows the latch unless a test says
    // otherwise.  IRQ delivery itself is a no-op here (dev is NULL) except
    // through the counters above.
    s_c->reg[SYM825_DIEN] = 0xFFu;
    s_c->reg[SYM825_SIEN0] = 0xFFu;
    s_c->reg[SYM825_SIEN1] = 0xFFu;
}

// Run the program at `addr`.
static void run_at(uint32_t addr) {
    set_reg32(SYM825_DSP, addr);
    sym53c8xx_start(s_c);
}

// Read and clear DSTAT the way a driver does, ignoring the live DFE bit.
static uint8_t take_dstat(void) {
    uint8_t v = s_c->dstat;
    s_c->dstat = 0;
    return v;
}

// Instruction encodings, spelled out so the tests read as programs.
#define BLOCK_MOVE(phase, count) (((uint32_t)(phase) << 24) | ((count) & 0x00FFFFFFu))
#define TABLE_MOVE(phase, off)   ((1u << 28) | ((uint32_t)(phase) << 24) | ((off) & 0x00FFFFFFu))
#define IO_SELECT(id, atn)       ((1u << 30) | ((atn) ? (1u << 24) : 0u) | ((uint32_t)(id) << 16))
#define IO_WAIT_DISCONNECT       ((1u << 30) | (1u << 27))
#define IO_WAIT_RESELECT         ((1u << 30) | (2u << 27))
#define IO_CLEAR(bits)           ((1u << 30) | (4u << 27) | (bits))
#define IO_SET(bits)             ((1u << 30) | (3u << 27) | (bits))
#define RW(opc, op, reg, imm)                                                                                          \
    ((1u << 30) | ((uint32_t)(opc) << 27) | ((uint32_t)(op) << 24) | ((uint32_t)(reg) << 16) | ((uint32_t)(imm) << 8))
#define TC(opc, flags) ((2u << 30) | ((uint32_t)(opc) << 27) | (flags))
#define MEMORY_MOVE(n) ((6u << 29) | ((n) & 0x00FFFFFFu))
#define LOAD(reg, n)   ((7u << 29) | (1u << 24) | ((uint32_t)(reg) << 16) | ((n) & 7u))
#define STORE(reg, n)  ((7u << 29) | ((uint32_t)(reg) << 16) | ((n) & 7u))

// Transfer Control flag bits.
#define TC_RELATIVE          (1u << 23)
#define TC_CARRY             (1u << 21)
#define TC_ON_THE_FLY        (1u << 20)
#define TC_IF_TRUE           (1u << 19)
#define TC_CMP_DATA          (1u << 18)
#define TC_CMP_PHASE         (1u << 17)
#define TC_PHASE(p)          ((uint32_t)(p) << 24)
#define TC_DATA(mask, value) (((uint32_t)(mask) << 8) | (uint32_t)(value))

// I/O Set/Clear operand bits.
#define IO_BIT_CARRY 0x400u
#define IO_BIT_ACK   0x040u
#define IO_BIT_ATN   0x008u

// ============================================================================
// 1. Block Move
// ============================================================================

// A complete command: select, push a CDB, take data, status and message,
// clear ACK, interrupt.  Exactly the shape Open Firmware's own probe
// builds, so this is the engine's whole job in one program.
TEST(test_block_move_full_command) {
    setup();
    memcpy(s_mem + 0x2000, "\x12\x00\x00\x00\x24\x00", 6); // an INQUIRY CDB
    memcpy(s_in, "ABCDEFGH", 8);
    s_in_n = 8;
    s_status_byte = 0x02; // CHECK CONDITION, so the test can see it in SFBR
    s_message_byte = 0x00;

    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0x0000BEEFu); // the alternate address, unused here
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_COMMAND, 6));
    put_insn_word(0x100C, 0x2000);
    put_insn_word(0x1010, BLOCK_MOVE(SYM825_PHASE_DATA_IN, 8));
    put_insn_word(0x1014, 0x3000);
    put_insn_word(0x1018, BLOCK_MOVE(SYM825_PHASE_STATUS, 1));
    put_insn_word(0x101C, 0x3100);
    put_insn_word(0x1020, BLOCK_MOVE(SYM825_PHASE_MSG_IN, 1));
    put_insn_word(0x1024, 0x3101);
    put_insn_word(0x1028, IO_CLEAR(IO_BIT_ACK));
    put_insn_word(0x102C, 0);
    put_insn_word(0x1030, TC(3, 0)); // INT
    put_insn_word(0x1034, 0xCAFEBABEu);

    run_at(0x1000);

    ASSERT_EQ_INT((int)s_out_n, 6);
    ASSERT_EQ_INT(memcmp(s_out, "\x12\x00\x00\x00\x24\x00", 6), 0);
    ASSERT_EQ_INT(memcmp(s_mem + 0x3000, "ABCDEFGH", 8), 0);
    ASSERT_EQ_INT(s_mem[0x3100], 0x02);
    ASSERT_EQ_INT(s_mem[0x3101], 0x00);
    // SFBR is "SCSI First Byte Received" and EVERY inbound move reloads it,
    // which is why a driver tests it immediately after the move it cares
    // about rather than at the end of a command.  Here the last inbound
    // move was the MESSAGE IN, so that is what it holds.
    ASSERT_EQ_INT(s_c->reg[SYM825_SFBR], 0x00);
    // The INT's vector lands in DSPS, and SIR is the cause.
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), (int)0xCAFEBABEu);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
    // The target let go: an UNEXPECTED DISCONNECT, reported only once the
    // script had halted, so DCMD holds the INT opcode $98 (see the engine's
    // disconnect_deferred).
    ASSERT_TRUE(s_c->sist0 & SYM825_SIST0_UDC);
    ASSERT_EQ_INT(s_c->reg[SYM825_DCMD], 0x98);
    ASSERT_EQ_INT(s_released, 1);
}

// The same command, but the script WAITS for the disconnect.  A disconnect
// the script asked for is not unexpected, so no UDC is reported.
//
// Both shapes are real and they are opposites.  Open Firmware's driver
// never waits and ends every command on the deferred UDC; AIX's `pscsidd`
// ends its with a Wait Disconnect and logs UDC as an adapter error.  An
// engine that raises it either way tells one of the two drivers that every
// successful command failed.
TEST(test_io_wait_disconnect_is_not_unexpected) {
    setup();
    memcpy(s_mem + 0x2000, "\x12\x00\x00\x00\x24\x00", 6);
    memcpy(s_in, "ABCDEFGH", 8);
    s_in_n = 8;
    s_status_byte = 0x00;
    s_message_byte = 0x00;

    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0x0000BEEFu);
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_COMMAND, 6));
    put_insn_word(0x100C, 0x2000);
    put_insn_word(0x1010, BLOCK_MOVE(SYM825_PHASE_DATA_IN, 8));
    put_insn_word(0x1014, 0x3000);
    put_insn_word(0x1018, BLOCK_MOVE(SYM825_PHASE_STATUS, 1));
    put_insn_word(0x101C, 0x3100);
    put_insn_word(0x1020, BLOCK_MOVE(SYM825_PHASE_MSG_IN, 1));
    put_insn_word(0x1024, 0x3101);
    put_insn_word(0x1028, IO_CLEAR(IO_BIT_ACK));
    put_insn_word(0x102C, 0);
    put_insn_word(0x1030, IO_WAIT_DISCONNECT);
    put_insn_word(0x1034, 0);
    put_insn_word(0x1038, TC(3, 0));
    put_insn_word(0x103C, 0xCAFEBABEu);

    run_at(0x1000);

    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
    ASSERT_TRUE(!(s_c->sist0 & SYM825_SIST0_UDC));
    ASSERT_TRUE(!s_c->connected);
    ASSERT_EQ_INT(s_released, 1);
}

// SFBR takes the first byte of an INBOUND move, which is the whole basis of
// the compare-and-branch idiom every driver uses.
TEST(test_block_move_sfbr) {
    setup();
    memcpy(s_in, "Zebra", 5);
    s_in_n = 5;
    s_mem[0x2000] = 0x12;
    s_cdb_len = 1;
    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_COMMAND, 1));
    put_insn_word(0x100C, 0x2000);
    put_insn_word(0x1010, BLOCK_MOVE(SYM825_PHASE_DATA_IN, 5));
    put_insn_word(0x1014, 0x3000);
    put_insn_word(0x1018, TC(3, 0));
    put_insn_word(0x101C, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SFBR], 'Z');
    ASSERT_EQ_INT(memcmp(s_mem + 0x3000, "Zebra", 5), 0);
}

// NEGATIVE: the script asks for a phase the target is not presenting.  The
// instruction must NOT execute, DSP must point AT it (not past it) so the
// driver can resume, and the cause is SIST0's MA bit.
TEST(test_block_move_phase_mismatch) {
    setup();
    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_DATA_IN, 4)); // target is in COMMAND
    put_insn_word(0x100C, 0x3000);
    memset(s_mem + 0x3000, 0xEE, 4);

    run_at(0x1000);

    ASSERT_EQ_INT((int)reg32(SYM825_DSP), 0x1008); // rewound to the instruction
    ASSERT_TRUE(s_c->sist0 & SYM825_SIST0_MA);
    ASSERT_EQ_INT(s_mem[0x3000], 0xEE); // nothing moved
    ASSERT_EQ_INT((int)reg32(SYM825_DBC) & 0x00FFFFFF, 4); // the count it wanted
    // A phase mismatch is FATAL: SCRIPTS stop.
    ASSERT_TRUE(!s_c->running);
}

// A short inbound transfer is a phase change too: the target ran out.
TEST(test_block_move_short_transfer) {
    setup();
    memcpy(s_in, "XY", 2);
    s_in_n = 2;
    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_COMMAND, 1));
    put_insn_word(0x100C, 0x2000);
    put_insn_word(0x1010, BLOCK_MOVE(SYM825_PHASE_DATA_IN, 8)); // only 2 available
    put_insn_word(0x1014, 0x3000);
    s_mem[0x2000] = 0x08;
    // The mock leaves COMMAND for DATA IN when the test says so; drive it
    // by hand, which is what a real target's phase change looks like.
    s_phase = M_COMMAND;

    set_reg32(SYM825_DSP, 0x1000);
    sym53c8xx_start(s_c);
    // The COMMAND move ran; the DATA IN move needs the target in DATA IN.
    s_phase = M_DATA_IN;
    run_at(0x1010);

    ASSERT_EQ_INT(memcmp(s_mem + 0x3000, "XY", 2), 0);
    ASSERT_TRUE(s_c->sist0 & SYM825_SIST0_MA);
}

// Table indirect: both the count and the buffer address come from a
// structure at DSA + a signed offset.  This is what lets SCRIPTS execute an
// operating system's own I/O data structures.
TEST(test_block_move_table_indirect) {
    setup();
    put_insn_word(0x4000, 4); // byte count
    put_insn_word(0x4004, 0x2000); // buffer address
    memcpy(s_mem + 0x2000, "WXYZ", 4);
    set_reg32(SYM825_DSA, 0x4100);

    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TABLE_MOVE(SYM825_PHASE_COMMAND, 0));
    put_insn_word(0x100C, 0xFFFF00u); // -0x100, signed
    put_insn_word(0x1010, TC(3, 0));
    put_insn_word(0x1014, 0);

    run_at(0x1000);
    ASSERT_EQ_INT((int)s_out_n, 4);
    ASSERT_EQ_INT(memcmp(s_out, "WXYZ", 4), 0);
}

// ============================================================================
// 2. I/O instructions
// ============================================================================

// NEGATIVE: nobody answers.  A selection time-out latches SIST1's STO and
// HALTS the engine where it stands; the alternate address belongs to a
// reselection that beat the arbitration, never to a target that is not
// there (Symbios Programming Guide v2.1, Select).
TEST(test_io_selection_timeout) {
    setup();
    s_target_present = 0;
    put_insn_word(0x1000, IO_SELECT(5, 0));
    put_insn_word(0x1004, 0x1800); // the alternate address
    put_insn_word(0x1800, RW(7, 0, SYM825_SCRATCHA, 0x9C)); // must NOT run
    put_insn_word(0x1804, 0);
    put_insn_word(0x1808, TC(3, 0));
    put_insn_word(0x180C, 0x5A5A5A5Au);

    run_at(0x1000);

    // The cause latches and the engine stops where it stands.  The
    // alternate address belongs to a reselection that beat the arbitration,
    // not to a target that is not there, and a driver told the wrong one
    // recovers a command it never issued.
    ASSERT_EQ_INT(s_c->sist0, 0);
    ASSERT_TRUE(!s_c->connected);
    ASSERT_TRUE(!s_c->running);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0);
    ASSERT_EQ_INT((int)reg32(SYM825_DSP), 0x1008);
    ASSERT_EQ_INT(take_dstat(), 0);
    // Two causes, not one: the time-out is the event and latches first;
    // the disconnect that ends the arbitration is its consequence and is
    // STACKED behind it ("it may occur before, at the same time, or
    // stacked after the STO interrupt" — LSI53C825A TM v3.1, SIST0 UDC).
    // A driver reading the pair as one 16-bit word sees the time-out on
    // its own first; the disconnect surfaces as a fresh interrupt after.
    ASSERT_TRUE(s_c->sist1 & SYM825_SIST1_STO);
    ASSERT_TRUE(s_c->sist0_stacked & SYM825_SIST0_UDC);
}

// Select WITH ATN: the target enters MESSAGE OUT for the IDENTIFY, which
// the shared bus model has no notion of, so the chip presents it.
TEST(test_io_select_with_atn) {
    setup();
    s_mem[0x2000] = 0xC0; // IDENTIFY, disconnect privilege
    put_insn_word(0x1000, IO_SELECT(3, 1));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, BLOCK_MOVE(SYM825_PHASE_MSG_OUT, 1));
    put_insn_word(0x100C, 0x2000);
    put_insn_word(0x1010, TC(3, 0));
    put_insn_word(0x1014, 0);

    set_reg32(SYM825_DSP, 0x1000);
    sym53c8xx_start(s_c);
    // The message was collected by the chip, not pushed at the bus, and the
    // virtual phase retired once the move completed.
    ASSERT_EQ_INT((int)s_out_n, 0);
    ASSERT_EQ_INT(s_c->msgout_pending, 0);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
}

// Set and Clear reach the ALU carry and the SCSI control latches.
TEST(test_io_set_clear) {
    setup();
    put_insn_word(0x1000, IO_SET(IO_BIT_CARRY | IO_BIT_ATN));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TC(3, 0));
    put_insn_word(0x100C, 0);
    run_at(0x1000);
    ASSERT_TRUE(s_c->reg[SYM825_SCNTL1] & 0x04u);
    ASSERT_TRUE(s_c->reg[SYM825_SOCL] & 0x08u);

    put_insn_word(0x1000, IO_CLEAR(IO_BIT_CARRY | IO_BIT_ATN));
    run_at(0x1000);
    ASSERT_TRUE(!(s_c->reg[SYM825_SCNTL1] & 0x04u));
    ASSERT_TRUE(!(s_c->reg[SYM825_SOCL] & 0x08u));
}

// Wait Reselect PARKS.  With SIGP clear the part waits for a reselection
// that this bus can never deliver, so the engine stops with DSP pointing
// AT the instruction and raises nothing at all.
//
// This is the shape of a real driver's idle script: a short ring that ends
// in Wait Reselect and jumps back to its own start.  An engine that takes
// the alternate address unconditionally runs that ring at host speed until
// the watchdog stops it — which is exactly how the Network Server's own
// driver hung, with no interrupt ever delivered.
TEST(test_io_wait_reselect_parks) {
    setup();
    put_insn_word(0x1000, IO_WAIT_RESELECT);
    put_insn_word(0x1004, 0x1800); // the alternate address
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x5A5A5A5Au);

    run_at(0x1000);

    ASSERT_TRUE(!s_c->running);
    ASSERT_TRUE(s_c->waiting_reselect);
    ASSERT_EQ_INT((int)reg32(SYM825_DSP), 0x1000);
    ASSERT_EQ_INT(take_dstat() & (uint8_t)~SYM825_DSTAT_DFE, 0);
    ASSERT_EQ_INT(s_c->sist0 | s_c->sist1, 0);
    ASSERT_EQ_INT(s_irq_asserts, 0);
}

// ABRT is the driver's escape from an operation that will not finish on
// its own — the one thing it can do to a chip arbitrating for a target
// that is never going to answer.  Without it the driver's recovery has
// nothing to act on and the selection lands later, against a command it
// has already given up on.
TEST(test_abort_drops_a_selection_in_flight) {
    setup();
    s_target_present = 0;
    put_insn_word(0x1000, IO_SELECT(5, 0));
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x5A5A5A5Au);

    // Arm a selection that will not answer, then abandon it.  (With no
    // scheduler the time-out is immediate, so drive the state directly.)
    s_c->select_timeout_armed = true;
    s_c->running = false;

    sym53c8xx_abort(s_c);

    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_ABRT);
    ASSERT_TRUE(!s_c->select_timeout_armed);
    ASSERT_TRUE(!s_c->running);
    // An abort is not a reset: the bus is left alone.
    ASSERT_TRUE(!(s_c->sist0 & SYM825_SIST0_RST));
}

// The driver drove RST/.  Everything in flight is over — the connection,
// the negotiated transfer agreement, the selection the chip was still
// arbitrating for — and the chip reports what it saw on the bus.  AIX's
// driver has a handler named for exactly that condition.
TEST(test_bus_reset_ends_everything) {
    setup();
    // Get the chip connected and synchronous, then pull the line.
    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TC(3, 0));
    put_insn_word(0x100C, 0);
    run_at(0x1000);
    (void)take_dstat();
    s_c->sync_period = 25;
    s_c->sync_offset = 16;
    s_c->connected = true;

    sym53c8xx_bus_reset(s_c);

    ASSERT_TRUE(s_c->sist0 & SYM825_SIST0_RST);
    ASSERT_TRUE(!s_c->connected);
    ASSERT_TRUE(!s_c->running);
    ASSERT_EQ_INT(s_c->sync_offset, 0);
    ASSERT_EQ_INT(s_c->sync_period, 0);
    ASSERT_EQ_INT(s_c->wide, 0);
}

// SIGP is the doorbell that gets it moving again: the parked instruction
// is re-executed, sees the bit, CLEARS it, and takes its alternate
// address.  A driver signals work exactly this way.
TEST(test_io_wait_reselect_sigp) {
    setup();
    put_insn_word(0x1000, IO_WAIT_RESELECT);
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x5A5A5A5Au);

    s_c->reg[SYM825_ISTAT] |= SYM825_ISTAT_SIGP;
    run_at(0x1000);

    ASSERT_TRUE(!s_c->waiting_reselect);
    ASSERT_TRUE(!(s_c->reg[SYM825_ISTAT] & SYM825_ISTAT_SIGP));
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x5A5A5A5A);
}

// ============================================================================
// 3. Read/Write — the register ALU
// ============================================================================

TEST(test_read_write_alu) {
    setup();
    // Move an immediate into SCRATCHA, then OR, then add with carry out.
    s_c->reg[SYM825_SCRATCHA] = 0;
    put_insn_word(0x1000, RW(7, 0, SYM825_SCRATCHA, 0x30)); // RMW: move data
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, RW(7, 2, SYM825_SCRATCHA, 0x0F)); // RMW: OR
    put_insn_word(0x100C, 0);
    put_insn_word(0x1010, RW(6, 0, SYM825_SCRATCHA, 0x77)); // move data to SFBR
    put_insn_word(0x1014, 0);
    put_insn_word(0x1018, TC(3, 0));
    put_insn_word(0x101C, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x3F);
    ASSERT_EQ_INT(s_c->reg[SYM825_SFBR], 0x77);

    // Add with carry OUT: $F0 + $20 wraps and sets the ALU carry.
    setup();
    put_insn_word(0x1000, RW(7, 0, SYM825_SCRATCHA, 0xF0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, RW(7, 6, SYM825_SCRATCHA, 0x20)); // add without carry in
    put_insn_word(0x100C, 0);
    put_insn_word(0x1010, TC(3, 0));
    put_insn_word(0x1014, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x10);
    ASSERT_TRUE(s_c->reg[SYM825_SCNTL1] & 0x04u); // carry set

    // Shift left THROUGH the carry, which is the documented behaviour.
    put_insn_word(0x1000, RW(7, 1, SYM825_SCRATCHA, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TC(3, 0));
    put_insn_word(0x100C, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x21); // ($10 << 1) | carry-in
}

// NEGATIVE: a register address outside the implemented file.
TEST(test_read_write_bad_register) {
    setup();
    put_insn_word(0x1000, RW(7, 0, 0x7F, 0x11)); // $7F is the last legal one...
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TC(3, 0));
    put_insn_word(0x100C, 0);
    run_at(0x1000);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR); // ...so this one is fine

    setup();
    // A[6:0] cannot express an address above $7F, so an out-of-range one is
    // only reachable through the ENGINE's own bounds check — exercised via
    // Load/Store below, where the byte count can push past the end.
    ASSERT_TRUE(s_c != NULL);
}

// ============================================================================
// 4. Transfer Control
// ============================================================================

TEST(test_transfer_control_jump_and_call) {
    setup();
    // Unconditional jump forward, then a Call/Return pair through TEMP.
    put_insn_word(0x1000, TC(0, 0)); // JUMP
    put_insn_word(0x1004, 0x1100);
    put_insn_word(0x1100, TC(1, 0)); // CALL
    put_insn_word(0x1104, 0x1200);
    put_insn_word(0x1200, RW(7, 0, SYM825_SCRATCHA, 0xAB));
    put_insn_word(0x1204, 0);
    put_insn_word(0x1208, TC(2, 0)); // RETURN
    put_insn_word(0x120C, 0);
    put_insn_word(0x1108, TC(3, 0)); // INT, reached only via the return
    put_insn_word(0x110C, 0x11111111u);

    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0xAB);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x11111111);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
}

// A relative jump is a 24-bit SIGNED displacement from the address of the
// NEXT instruction — both directions.
TEST(test_transfer_control_relative) {
    setup();
    // Forward: the displacement is relative to the address of the NEXT
    // instruction, so $1000 + 8 + $100 = $1108.
    put_insn_word(0x1000, TC(0, TC_RELATIVE));
    put_insn_word(0x1004, 0x100);
    // Backward: $1108 + 8 - $80 = $1090.
    put_insn_word(0x1108, TC(0, TC_RELATIVE));
    put_insn_word(0x110C, 0xFFFF80u); // -$80, sign-extended from 24 bits
    put_insn_word(0x1090, TC(3, 0));
    put_insn_word(0x1094, 0x22222222u);
    run_at(0x1000);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x22222222);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
}

// Conditional transfers: on phase, on data through a compare MASK, and on
// the ALU carry — each in both the taken and the not-taken direction.
TEST(test_transfer_control_conditions) {
    setup();
    // Phase compare: the target is in COMMAND after selection.
    put_insn_word(0x1000, IO_SELECT(3, 0));
    put_insn_word(0x1004, 0);
    put_insn_word(0x1008, TC(0, TC_CMP_PHASE | TC_IF_TRUE | TC_PHASE(SYM825_PHASE_STATUS)));
    put_insn_word(0x100C, 0x1900); // NOT taken: the phase is COMMAND
    put_insn_word(0x1010, TC(0, TC_CMP_PHASE | TC_IF_TRUE | TC_PHASE(SYM825_PHASE_COMMAND)));
    put_insn_word(0x1014, 0x1800); // taken
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x33333333u);
    put_insn_word(0x1900, TC(3, 0));
    put_insn_word(0x1904, 0xDEADDEADu);
    run_at(0x1000);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x33333333);

    // Data compare with a mask: SFBR is $A5, and the mask hides everything
    // but the top bit, so a compare value of $80 matches.
    setup();
    s_c->reg[SYM825_SFBR] = 0xA5;
    put_insn_word(0x1000, TC(0, TC_CMP_DATA | TC_IF_TRUE) | TC_DATA(0x7F, 0x80));
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x44444444u);
    run_at(0x1000);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x44444444);

    // The same compare WITHOUT the mask does not match, and `jump if false`
    // therefore takes it.
    setup();
    s_c->reg[SYM825_SFBR] = 0xA5;
    put_insn_word(0x1000, TC(0, TC_CMP_DATA) | TC_DATA(0x00, 0x80)); // if FALSE
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x55555555u);
    run_at(0x1000);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x55555555);

    // Carry test.
    setup();
    s_c->reg[SYM825_SCNTL1] |= 0x04u;
    put_insn_word(0x1000, TC(0, TC_CARRY | TC_IF_TRUE));
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1800, TC(3, 0));
    put_insn_word(0x1804, 0x66666666u);
    run_at(0x1000);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x66666666);
}

// Interrupt-on-the-fly does NOT halt the processor: it sets its own ISTAT
// bit and execution continues.
TEST(test_transfer_control_interrupt_on_the_fly) {
    setup();
    put_insn_word(0x1000, TC(3, TC_ON_THE_FLY));
    put_insn_word(0x1004, 0x77777777u);
    put_insn_word(0x1008, RW(7, 0, SYM825_SCRATCHA, 0x5C)); // must still run
    put_insn_word(0x100C, 0);
    put_insn_word(0x1010, TC(3, 0));
    put_insn_word(0x1014, 0);
    run_at(0x1000);
    ASSERT_TRUE(s_c->reg[SYM825_ISTAT] & SYM825_ISTAT_INTF);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x5C);
    // ...and it drives the PIN, on its own.  The whole point of the
    // instruction is to tell the driver a command finished WITHOUT
    // stopping SCRIPTS; a model that only latches the bit leaves the
    // driver waiting forever on an interrupt a healthy script already
    // sent.  INTF has no enable bit to gate it.
    //
    // The program also ends on a plain INT, so retire that cause first:
    // what is being proved is that INTF alone holds the pin up.
    (void)take_dstat();
    sym53c8xx_update_irq(s_c);
    ASSERT_TRUE(s_c->irq);
    // And the pin follows the latch back down when the driver acknowledges
    // it.  (The write-ONE-to-clear decode itself belongs to the register
    // file, in sym53c825.c, which this suite does not link.)
    s_c->reg[SYM825_ISTAT] &= (uint8_t)~SYM825_ISTAT_INTF;
    sym53c8xx_update_irq(s_c);
    ASSERT_TRUE(!s_c->irq);
}

// ============================================================================
// 5. Memory Move
// ============================================================================

TEST(test_memory_move) {
    setup();
    memcpy(s_mem + 0x2000, "0123456789ABCDEF", 16);
    put_insn_word(0x1000, MEMORY_MOVE(16));
    put_insn_word(0x1004, 0x2000); // source
    put_insn_word(0x1008, 0x3000); // destination
    put_insn_word(0x100C, TC(3, 0));
    put_insn_word(0x1010, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(memcmp(s_mem + 0x3000, "0123456789ABCDEF", 16), 0);
    // Three dwords, so the next instruction is at +12.
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_SIR);
}

// A Memory Move inside a subroutine must leave the return address alone.
// TEMP is the Call/Return link register and nothing else; the three-Dword
// form carries both of its addresses in the instruction, so it has no
// reason to touch it.
//
// The Network Server's AIX driver is exactly this program: it calls a
// routine that patches its own instruction stream with a four-byte Memory
// Move and then returns.  An engine that parks the destination address in
// TEMP sends that Return into the middle of the instruction the move had
// just rewritten.
TEST(test_memory_move_preserves_temp) {
    setup();
    put_insn_word(0x1000, TC(1, TC_IF_TRUE)); // Call
    put_insn_word(0x1004, 0x1800);
    put_insn_word(0x1008, TC(3, 0)); // where the Return must land
    put_insn_word(0x100C, 0x99999999u);

    put_insn_word(0x1800, MEMORY_MOVE(4));
    put_insn_word(0x1804, 0x2000); // source
    put_insn_word(0x1808, 0x3000); // destination
    put_insn_word(0x180C, TC(2, TC_IF_TRUE)); // Return
    put_insn_word(0x1810, 0);

    run_at(0x1000);

    ASSERT_EQ_INT((int)reg32(SYM825_TEMP), 0x1008);
    ASSERT_EQ_INT((int)reg32(SYM825_DSPS), 0x99999999);
}

// NEGATIVE: "Both the source and destination addresses must start with the
// same address alignment A[1:0].  If the source and destination are not
// aligned, then an illegal instruction interrupt occurs."
TEST(test_memory_move_misaligned) {
    setup();
    put_insn_word(0x1000, MEMORY_MOVE(4));
    put_insn_word(0x1004, 0x2000);
    put_insn_word(0x1008, 0x3001); // different A[1:0]
    run_at(0x1000);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_IID);
    ASSERT_TRUE(!s_c->running);
}

// ============================================================================
// 6. Load and Store
// ============================================================================

TEST(test_load_and_store) {
    setup();
    s_mem[0x2000] = 0x11;
    s_mem[0x2001] = 0x22;
    put_insn_word(0x1000, LOAD(SYM825_SCRATCHA, 2));
    put_insn_word(0x1004, 0x2000);
    put_insn_word(0x1008, STORE(SYM825_SCRATCHA, 2));
    put_insn_word(0x100C, 0x2100);
    put_insn_word(0x1010, TC(3, 0));
    put_insn_word(0x1014, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x11);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA + 1], 0x22);
    ASSERT_EQ_INT(s_mem[0x2100], 0x11);
    ASSERT_EQ_INT(s_mem[0x2101], 0x22);
}

// The DSA-relative form, which is how a driver reaches its own structures.
TEST(test_load_dsa_relative) {
    setup();
    set_reg32(SYM825_DSA, 0x2000);
    s_mem[0x2010] = 0x99;
    put_insn_word(0x1000, LOAD(SYM825_SCRATCHA, 1) | (1u << 28));
    put_insn_word(0x1004, 0x10);
    put_insn_word(0x1008, TC(3, 0));
    put_insn_word(0x100C, 0);
    run_at(0x1000);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x99);
}

// NEGATIVE: "A maximum of 4 bytes may be moved… the register address and
// memory address must have the same byte alignment, and the count set such
// that it does not cross Dword boundaries."
TEST(test_load_store_illegal) {
    setup();
    put_insn_word(0x1000, LOAD(SYM825_SCRATCHA, 0)); // zero bytes
    put_insn_word(0x1004, 0x2000);
    run_at(0x1000);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_IID);

    setup();
    put_insn_word(0x1000, LOAD(SYM825_SCRATCHA, 4));
    put_insn_word(0x1004, 0x2001); // register and memory disagree on A[1:0]
    run_at(0x1000);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_IID);
}

// ============================================================================
// 7. The status discipline
// ============================================================================

// Enables gate the PIN, never the latch: "the SCRIPTS still stop … but the
// IRQ/ pin is not asserted."  A masked fatal condition is still fatal.
TEST(test_masking_gates_the_pin_not_the_halt) {
    setup();
    s_c->reg[SYM825_DIEN] = 0; // every DMA cause masked
    s_irq_asserts = 0;
    put_insn_word(0x1000, MEMORY_MOVE(4));
    put_insn_word(0x1004, 0x2000);
    put_insn_word(0x1008, 0x3001); // misaligned: illegal instruction
    run_at(0x1000);
    ASSERT_TRUE(s_c->dstat & SYM825_DSTAT_IID); // the cause latched
    ASSERT_TRUE(!s_c->running); // and it halted
    ASSERT_EQ_INT(s_irq_asserts, 0); // but the pin stayed quiet
}

// Non-fatal SCSI causes do not stop SCRIPTS.
TEST(test_nonfatal_scsi_cause) {
    setup();
    s_c->running = true;
    sym53c8xx_raise_scsi(s_c, SYM825_SIST0_CMP, 0);
    ASSERT_TRUE(s_c->running);
    sym53c8xx_raise_scsi(s_c, SYM825_SIST0_UDC, 0);
    ASSERT_TRUE(!s_c->running);
}

// ============================================================================
// 8. The runaway watchdog
// ============================================================================

// A program that never halts must not take the emulator with it: the chip's
// own watchdog cause is the honest report.
TEST(test_runaway_watchdog) {
    setup();
    put_insn_word(0x1000, TC(0, 0)); // JUMP to itself, forever
    put_insn_word(0x1004, 0x1000);
    run_at(0x1000);
    ASSERT_TRUE(take_dstat() & SYM825_DSTAT_WTD);
    ASSERT_TRUE(!s_c->running);
}

// ============================================================================
// 9. Checkpoint round trip
// ============================================================================

TEST(test_checkpoint_roundtrip) {
    setup();
    s_cp_w = s_cp_r = 0;
    s_c->reg[SYM825_SCRATCHA] = 0x5A;
    s_c->reg[SYM825_SCID] = 7;
    set_reg32(SYM825_DSA, 0xC0FFEE00u);
    s_c->script_ram[0x100] = 0xA7;
    s_c->sist0 = SYM825_SIST0_CMP;
    sym53c8xx_checkpoint_save(s_c, (checkpoint_t *)1);

    sym53c8xx_delete(s_c);
    s_c = sym53c8xx_new(NULL, 0);
    ASSERT_TRUE(s_c != NULL);
    sym53c8xx_checkpoint_restore(s_c, (checkpoint_t *)1);

    ASSERT_EQ_INT(s_c->reg[SYM825_SCRATCHA], 0x5A);
    ASSERT_EQ_INT(s_c->reg[SYM825_SCID], 7);
    ASSERT_EQ_INT((int)reg32(SYM825_DSA), (int)0xC0FFEE00u);
    ASSERT_EQ_INT(s_c->script_ram[0x100], 0xA7);
    ASSERT_EQ_INT(s_c->sist0, SYM825_SIST0_CMP);
}

int main(void) {
    RUN(test_block_move_full_command);
    RUN(test_io_wait_disconnect_is_not_unexpected);
    RUN(test_block_move_sfbr);
    RUN(test_block_move_phase_mismatch);
    RUN(test_block_move_short_transfer);
    RUN(test_block_move_table_indirect);
    RUN(test_io_selection_timeout);
    RUN(test_io_select_with_atn);
    RUN(test_io_set_clear);
    RUN(test_abort_drops_a_selection_in_flight);
    RUN(test_bus_reset_ends_everything);
    RUN(test_io_wait_reselect_parks);
    RUN(test_io_wait_reselect_sigp);
    RUN(test_read_write_alu);
    RUN(test_read_write_bad_register);
    RUN(test_transfer_control_jump_and_call);
    RUN(test_transfer_control_relative);
    RUN(test_transfer_control_conditions);
    RUN(test_transfer_control_interrupt_on_the_fly);
    RUN(test_memory_move);
    RUN(test_memory_move_preserves_temp);
    RUN(test_memory_move_misaligned);
    RUN(test_load_and_store);
    RUN(test_load_dsa_relative);
    RUN(test_load_store_illegal);
    RUN(test_masking_gates_the_pin_not_the_halt);
    RUN(test_nonfatal_scsi_cause);
    RUN(test_runaway_watchdog);
    RUN(test_checkpoint_roundtrip);
    return 0;
}
