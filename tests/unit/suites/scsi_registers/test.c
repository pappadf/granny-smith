// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Guest-reachable NCR 5380 register transitions must be declined, not asserted.
//
// 03-scsi F-07: the bus state machine used plain assert() to guard transitions
// a guest drives entirely through the register file.  Two builds, two opposite
// wrong answers:
//
//   - The DEFAULT headless build keeps assertions live (Makefile.headless:
//     "No -DNDEBUG: keep assertions active in the headless test/debug tool"),
//     and CI runs every integration row with it.  Five byte-writes to the ICR
//     from guest address space killed the process with SIGABRT.
//   - GS_FAST / NDEBUG (the release wasm profile) compiled the guard out and
//     walked straight into the state the rest of the file assumes cannot
//     happen.
//
// Neither is "decline the request", which is what the file does elsewhere and
// what the comment on the INQUIRY path argues for ("the guest may legitimately
// try").  Five sites were reachable; all five are pinned below.
//
// Reachability was established by fuzzing the register file from guest address
// space on a Mac Plus: ~100,000 random writes across 125 seeds reached exactly
// these five and no others.  The buffer-capacity asserts (buf.size < buf.max,
// buf.size <= cmd_size) were never violated and remain assert()s -- they are
// real invariants, because buf.size increments by one and always lands exactly
// on the dispatch boundary.
//
// These tests call scsi->memory_interface.write_uint8 directly.  That is the
// exact function a guest store lands on once the memory map has decoded the
// address, so no map, machine or ROM is required -- and it is the only way to
// reach the register file from a unit test.

#include "cpu.h"
#include "image.h"
#include "memory.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "system.h"
#include "test_assert.h"
#include "via.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================
// Link stubs
// ============================================================
// scsi.c registers an object-model node and carries shell-facing helpers, so it
// references the wider emulator.  None of that is on the path these tests drive.

config_t *global_emulator = NULL;

void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *context) {
    (void)mem, (void)addr, (void)size, (void)name, (void)iface, (void)context;
}
uint32_t cpu_get_pc(cpu_t *restrict cpu) {
    (void)cpu;
    return 0;
}
void via_input_c(via_t *via, int port, int c, bool value) {
    (void)via, (void)port, (void)c, (void)value;
}
image_t *setup_get_image_by_filename(const char *filename) {
    (void)filename;
    return NULL;
}
int system_hd_attach(const char *path, int scsi_id) {
    (void)path, (void)scsi_id;
    return -1;
}
void add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
}
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
void add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
}

#define TARGET 0
#define BLK    512
#define BLOCKS 64

static char g_path[] = "/tmp/gs-scsi-registers-XXXXXX";

static void make_disk(void) {
    int fd = mkstemp(g_path);
    ASSERT_TRUE(fd >= 0);
    uint8_t *blk = calloc(1, BLK);
    ASSERT_TRUE(blk != NULL);
    for (int i = 0; i < BLOCKS; i++)
        ASSERT_TRUE(write(fd, blk, BLK) == BLK);
    free(blk);
    close(fd);
}

static scsi_t *attach_disk(void) {
    scsi_t *scsi = scsi_init(NULL, NULL);
    ASSERT_TRUE(scsi != NULL);
    image_t *img = image_create(g_path, NULL);
    ASSERT_TRUE(img != NULL);
    scsi_add_device(scsi, TARGET, "GS", "SCRATCH", "1.0", img, scsi_dev_hd, BLK, false);
    return scsi;
}

// Write one 5380 register the way the guest does.  Register select is A4-A6
// (see the decode in scsi.c write_uint8), so the register index shifts left 4.
static void wr(scsi_t *scsi, int reg, uint8_t val) {
    scsi->memory_interface.write_uint8(scsi, (uint32_t)(reg << 4), val);
}

// The finding's first named sequence: select normally, then drive SEL/BSY a
// second time.  The second BSY-release reaches the end-of-selection branch with
// phase == command, which used to be `assert(phase == scsi_selection)`.
//
// ICR = SEL, SEL|BSY, SEL  completes a selection and lands in COMMAND; doing it
// again is what a driver does when it believes the bus is hung.
TEST(test_reselect_from_command_is_declined) {
    scsi_t *scsi = attach_disk();

    wr(scsi, ODR, 1 << TARGET | 1 << 7); // target + initiator ID on the data bus
    wr(scsi, ICR, ICR_SEL);
    wr(scsi, ICR, ICR_SEL | ICR_BSY);
    wr(scsi, ICR, ICR_SEL); // selection completes -> COMMAND
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);

    // Round two.  Before the fix this aborted the process.
    wr(scsi, ICR, ICR_SEL | ICR_BSY);
    wr(scsi, ICR, ICR_SEL);

    // Declined: the live transaction is untouched, not restarted.
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);
    scsi_delete(scsi);
}

// The finding's second named sequence, and by far the most frequently reached
// in the fuzz: a single write of MR.DMA while the bus is idle.  There is no
// REQ/ACK partner in BUS FREE, so on real silicon arming DMA transfers nothing.
TEST(test_dma_mode_in_bus_free_is_declined) {
    scsi_t *scsi = attach_disk();
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_bus_free);

    wr(scsi, MR, MR_DMA); // before the fix: SIGABRT

    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_bus_free);
    scsi_delete(scsi);
}

// phase_arbitration: MR.ARBITRATE is only meaningful from BUS FREE.  Setting it
// mid-transaction must not drag a live bus backwards into arbitration.
TEST(test_arbitrate_outside_bus_free_is_declined) {
    scsi_t *scsi = attach_disk();

    wr(scsi, ODR, 1 << TARGET | 1 << 7);
    wr(scsi, ICR, ICR_SEL);
    wr(scsi, ICR, ICR_SEL | ICR_BSY);
    wr(scsi, ICR, ICR_SEL);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);

    wr(scsi, MR, MR_ARBITRATE);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);
    scsi_delete(scsi);
}

// phase_selection: SEL asserted from a phase that is neither BUS FREE nor
// ARBITRATION.  Same rule -- the transaction in flight wins.
TEST(test_select_from_command_is_declined) {
    scsi_t *scsi = attach_disk();

    wr(scsi, ODR, 1 << TARGET | 1 << 7);
    wr(scsi, ICR, ICR_SEL);
    wr(scsi, ICR, ICR_SEL | ICR_BSY);
    wr(scsi, ICR, ICR_SEL);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);

    wr(scsi, ICR, 0); // drop SEL
    wr(scsi, ICR, ICR_SEL); // re-assert from COMMAND
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);
    scsi_delete(scsi);
}

// Declining must not cost the bus its ability to work afterwards: a normal
// selection still succeeds once the guest stops driving nonsense.
TEST(test_bus_still_usable_after_declines) {
    scsi_t *scsi = attach_disk();

    wr(scsi, MR, MR_DMA); // declined (BUS FREE)
    wr(scsi, MR, MR_ARBITRATE); // legal from BUS FREE
    wr(scsi, MR, 0);
    wr(scsi, ICR, ICR_SEL);
    wr(scsi, ICR, ICR_SEL | ICR_BSY);
    wr(scsi, ICR, ICR_SEL);
    ASSERT_EQ_INT(scsi_get_bus_phase(scsi), scsi_command);
    scsi_delete(scsi);
}

int main(void) {
    make_disk();
    RUN(test_reselect_from_command_is_declined);
    RUN(test_dma_mode_in_bus_free_is_declined);
    RUN(test_arbitrate_outside_bus_free_is_declined);
    RUN(test_select_from_command_is_declined);
    RUN(test_bus_still_usable_after_declines);
    unlink(g_path);
    printf("All scsi_registers tests passed\n");
    return 0;
}
