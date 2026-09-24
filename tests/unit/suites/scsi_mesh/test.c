// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Apple's MESH controller, driven through its register file.
//
// 03-scsi F-23.  MESH was the only controller in this subsystem with no unit
// tests, because it had no handle to construct: it lived under machines/tnt/,
// took config_t* everywhere and reached its state through tnt_st(cfg)->mesh.
// Every bug in it had to be found through a full Power Macintosh boot.
//
// That cost was paid during the move itself.  A mechanical rewrite turned
//
//     if (dma)
//         tnt_dbdma_kick(tnt_st(cfg)->dbdma, 10);
//     else
//         pump_in(m);
//
// into an unbraced `if (dma) if (m->dbdma_kick) ... else pump_in(m);` -- a
// dangling else that silently removed the non-DMA DATA IN path.  The only
// symptom was that a PowerMac stopped finding its boot drive, and it took a
// 100,000-access register trace diffed against the pre-move build to find it.
// A test at this level would have said so in a second.

#include "scheduler.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "scsi_mesh.h"
#include "test_assert.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MESH registers are selected by address bits 4-7 (offset >> 4).
#define R_COUNT_LO    0x0
#define R_COUNT_HI    0x1
#define R_FIFO        0x2
#define R_SEQUENCE    0x3
#define R_BUS_STATUS0 0x4
#define R_BUS_STATUS1 0x5
#define R_FIFO_COUNT  0x6
#define R_EXCEPTION   0x7
#define R_ERROR       0x8
#define R_INTR_MASK   0x9
#define R_INTERRUPT   0xA
#define R_DEST_ID     0xC
#define R_SEL_TIMEOUT 0xE

#define SEQ_ARBITRATE 0x1
#define SEQ_SELECT    0x2
#define SEQ_COMMAND   0x3
#define SEQ_DATAIN    0x6
#define SEQ_DMA_MODE  0x80

#define INT_CMDDONE   0x01
#define INT_EXCEPTION 0x02
#define EXC_SELTO     0x01

#define TARGET 3
#define BLK    512

// ============================================================
// Link stubs
// ============================================================

// This suite never checkpoints -- it exercises the register file.
void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *d, size_t n, const char *tag, const char *f,
                                      int l) {
    (void)tag;
    (void)cp, (void)d, (void)n, (void)f, (void)l;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *d, size_t n, const char *tag, const char *f, int l) {
    (void)tag;
    (void)cp, (void)d, (void)n, (void)f, (void)l;
}

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
// No image is re-opened here: these tests care about the register and device
// state around the medium, not the medium itself.
image_t *setup_get_image_by_filename(const char *filename) {
    (void)filename;
    return NULL;
}
int system_hd_attach(const char *path, int scsi_id) {
    (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
}
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
}
// No scheduler here: these tests never let time pass.
struct scheduler *system_scheduler(void) {
    return NULL;
}
uint64_t scheduler_cpu_cycles(struct scheduler *restrict s) {
    (void)s;
    return 0;
}
void scheduler_new_event_type(struct scheduler *s, const char *sn, void *src, const char *en, event_callback_t cb) {
    (void)s, (void)sn, (void)src, (void)en, (void)cb;
}
event_t *scheduler_new_cpu_event_ex(struct scheduler *restrict s, event_callback_t cb, void *src, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)periodic;
    (void)s, (void)cb, (void)src, (void)data, (void)cycles, (void)ns;
    return NULL;
}
void remove_event(struct scheduler *restrict s, event_callback_t cb, void *src) {
    (void)s, (void)cb, (void)src;
}
// The DMA pump (F-15) asks whether it is already queued.  These tests drive
// the port directly rather than through the scheduler, so "never queued" is
// the right answer: every mesh_pump_arm re-arms into the no-op above.
bool has_event(struct scheduler *restrict s, event_callback_t cb) {
    (void)s, (void)cb;
    return false;
}
void scheduler_forget_source(struct scheduler *sch, void *source) {
    (void)sch;
    (void)source;
}
// The medium itself is out of scope here: these tests stage the state AROUND a
// device, not its contents, and setup_get_image_by_filename() returns NULL so
// no image is ever opened.
size_t disk_read_data(image_t *img, size_t off, uint8_t *buf, size_t len) {
    (void)img, (void)off, (void)buf, (void)len;
    return 0;
}
size_t disk_write_data(image_t *img, size_t off, uint8_t *buf, size_t len) {
    (void)img, (void)off, (void)buf, (void)len;
    return 0;
}
size_t disk_size(image_t *img) {
    (void)img;
    return 0;
}
const char *image_get_filename(const image_t *img) {
    (void)img;
    return NULL;
}
const char *image_path(const image_t *img) {
    (void)img;
    return NULL;
}
image_t *image_open_readonly(const char *path) {
    (void)path;
    return NULL;
}
void image_close(image_t *img) {
    (void)img;
}
int image_export_to(image_t *img, const char *path) {
    (void)img, (void)path;
    return -1;
}
int drive_catalog_count(void) {
    return 0;
}
const struct drive_model *drive_catalog_get(int i) {
    (void)i;
    return NULL;
}
const struct drive_model *drive_catalog_find_closest(size_t bytes) {
    (void)bytes;
    return NULL;
}

struct cpu *system_cpu(void) {
    return NULL;
}
struct object *machine_object(void) {
    return NULL;
}
int platform_ntz32(uint32_t v) {
    unsigned n = 0;
    if (!v)
        return 32;
    while (!(v & 1u)) {
        v >>= 1;
        n++;
    }
    return n;
}

// ============================================================
// Harness
// ============================================================

static mesh_t *s_m;
static scsi_t *s_bus;
static int s_irq_level;
static int s_dbdma_kicks;

static void on_irq(void *ctx, bool level) {
    (void)ctx;
    s_irq_level = level ? 1 : 0;
}
static void on_kick(void *ctx) {
    (void)ctx;
    s_dbdma_kicks++;
}

// Build a bus with one HD on it and a MESH driving it -- the whole point of
// the move: this is three calls, not a Power Macintosh.
static void setup(void) {
    s_irq_level = 0;
    s_dbdma_kicks = 0;
    s_bus = scsi_init(NULL);
    ASSERT_TRUE(s_bus != NULL);
    scsi_add_device(s_bus, TARGET, "GS", "SCRATCH", "1.0", NULL, scsi_dev_hd, BLK, false);
    s_m = mesh_init(NULL, NULL);
    ASSERT_TRUE(s_m != NULL);
    mesh_attach_bus(s_m, s_bus);
    mesh_set_irq_callback(s_m, on_irq, NULL);
    mesh_set_dbdma_kick(s_m, on_kick, NULL);
    mesh_reset(s_m);
}

static void teardown(void) {
    mesh_delete(s_m);
    scsi_delete(s_bus);
    s_m = NULL;
    s_bus = NULL;
}

static uint8_t rd(int reg) {
    return mesh_read(s_m, (uint32_t)reg << 4);
}
static void wr(int reg, uint8_t v) {
    mesh_write(s_m, (uint32_t)reg << 4, v);
}

// ============================================================
// Tests
// ============================================================

// The move's own regression, pinned -- in BOTH directions, because one
// direction alone does not distinguish the bug.
//
// A DATA IN sequence WITHOUT the DMA-mode bit must pump the FIFO itself; only
// the DMA form defers to the channel-10 port.  A mechanical rewrite turned the
// braced pair into `if (dma) if (m->dbdma_kick) ... else pump_in(m);` -- a
// dangling else that bound pump_in to the INNER if.
//
// The discriminator is what pump_in does when the target has no data: it
// reports a short transfer as a phase mismatch and clears the active command.
// So "did pump_in run?" is directly observable.
TEST(datain_without_dma_pumps_the_fifo) {
    setup();
    wr(R_DEST_ID, TARGET);
    wr(R_SEQUENCE, SEQ_SELECT);
    ASSERT_TRUE(s_m->connected);

    wr(R_COUNT_HI, 0x02); // ask for bytes, so pump_in has work to attempt
    wr(R_COUNT_LO, 0x00);
    wr(R_SEQUENCE, SEQ_DATAIN); // no SEQ_DMA_MODE: pump here and now

    // pump_in ran, found the target not in DATA IN, and said so.
    ASSERT_EQ_INT(s_dbdma_kicks, 0); // the DMA port was NOT asked
    ASSERT_EQ_INT(s_m->active, 0); // the command was retired
    ASSERT_TRUE((rd(R_EXCEPTION) & 0x02) != 0); // EXC_PHASEMM
    teardown();
}

// The other direction, and the one that catches a dangling else on its own:
// with the DMA bit set but NO channel wired, the correct code does nothing at
// all.  The buggy form falls through to pump_in and retires the command.
TEST(datain_with_dma_never_pumps_the_fifo) {
    setup();
    mesh_set_dbdma_kick(s_m, NULL, NULL); // no channel attached
    wr(R_DEST_ID, TARGET);
    wr(R_SEQUENCE, SEQ_SELECT);
    ASSERT_TRUE(s_m->connected);

    wr(R_COUNT_HI, 0x02);
    wr(R_COUNT_LO, 0x00);
    wr(R_SEQUENCE, SEQ_DATAIN | SEQ_DMA_MODE);

    // The transfer is armed and waiting for the channel, not retired.
    ASSERT_EQ_INT(s_m->active, SEQ_DATAIN);
    ASSERT_TRUE(s_m->active_dma);
    ASSERT_TRUE((rd(R_EXCEPTION) & 0x02) == 0); // no phase mismatch
    teardown();
}

// ...and with a channel wired, the DMA form asks it to run.
TEST(datain_with_dma_asks_the_channel) {
    setup();
    wr(R_DEST_ID, TARGET);
    wr(R_SEQUENCE, SEQ_SELECT);
    ASSERT_TRUE(s_m->connected);

    wr(R_COUNT_HI, 0x02);
    wr(R_COUNT_LO, 0x00);
    wr(R_SEQUENCE, SEQ_DATAIN | SEQ_DMA_MODE);
    ASSERT_EQ_INT(s_dbdma_kicks, 1);
    teardown();
}

// Selecting a target that is not there reports a time-out -- and does so
// through the SHARED bus helper (F-18), which with no scheduler under the
// suite reports immediately.
TEST(select_absent_target_times_out) {
    setup();
    wr(R_SEL_TIMEOUT, 25); // 250 ms, what Mac OS programs
    wr(R_DEST_ID, 5); // nothing on id 5
    wr(R_SEQUENCE, SEQ_SELECT);

    ASSERT_TRUE(!s_m->connected);
    ASSERT_TRUE((rd(R_EXCEPTION) & EXC_SELTO) != 0);
    ASSERT_TRUE((rd(R_INTERRUPT) & INT_EXCEPTION) != 0);
    teardown();
}

// The interrupt line is a level gated by the mask, and the machine sees it
// through the callback rather than the model reaching into Grand Central.
TEST(interrupt_line_follows_the_mask) {
    setup();
    wr(R_INTR_MASK, 0x00); // everything masked
    wr(R_DEST_ID, 5);
    wr(R_SEQUENCE, SEQ_SELECT); // raises EXC_SELTO
    ASSERT_EQ_INT(s_irq_level, 0); // latched, but the line stays down

    wr(R_INTR_MASK, INT_EXCEPTION); // unmask
    ASSERT_EQ_INT(s_irq_level, 1);

    wr(R_INTERRUPT, INT_EXCEPTION); // W1C
    ASSERT_EQ_INT(s_irq_level, 0);
    teardown();
}

// A reset returns the plain-data region to power-on and leaves the WIRING
// alone.  This is the other bug the move introduced: mesh_reset() was a
// memset over sizeof(*m), which was safe while the struct had no pointers and
// erased the bus and both callbacks once it did.
TEST(reset_clears_state_but_keeps_the_wiring) {
    setup();
    wr(R_DEST_ID, TARGET);
    wr(R_SEQUENCE, SEQ_SELECT);
    ASSERT_TRUE(s_m->connected);

    mesh_reset(s_m);
    ASSERT_TRUE(!s_m->connected);
    ASSERT_EQ_INT(s_m->sync_params, 2); // ASYNC_PARAMS power-on default

    // Still wired: the bus, the interrupt line and the DMA channel.
    ASSERT_TRUE(s_m->bus == s_bus);
    ASSERT_TRUE(s_m->irq_cb == on_irq);
    ASSERT_TRUE(s_m->dbdma_kick == on_kick);

    // ...and it still works afterwards.
    wr(R_DEST_ID, TARGET);
    wr(R_SEQUENCE, SEQ_SELECT);
    ASSERT_TRUE(s_m->connected);
    teardown();
}

int main(void) {
    RUN(datain_without_dma_pumps_the_fifo);
    RUN(datain_with_dma_never_pumps_the_fifo);
    RUN(datain_with_dma_asks_the_channel);
    RUN(select_absent_target_times_out);
    RUN(interrupt_line_follows_the_mask);
    RUN(reset_clears_state_but_keeps_the_wiring);
    printf("All scsi_mesh tests passed\n");
    return 0;
    return true;
}
