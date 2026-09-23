// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SCSI checkpoint round-trip.
//
// 03-scsi F-20.  The save was a plain-data prefix that stopped at `devices`,
// plus a hand-written per-field loop over the eight device records -- and the
// loop forgot fields.  sense, prevent_removal and default_block_size were all
// silently dropped.
//
// That is not theoretical: instrumenting the save path showed suite-quadra
// checkpointing its CD-ROM twice with non-zero sense, once as
// NOT READY / 0xB0 ("caddy not inserted") and once as
// UNIT ATTENTION / 0x29 ("power on, reset or BUS DEVICE RESET occurred").
// unit_attention IS saved and the sense explaining it is not, so after a
// restore the driver asks REQUEST SENSE and is told 0x28 -- "a disc was
// inserted" -- when the truth was "the bus was reset".
//
// The fix moves the image pointers out of the device records into
// device_images[], so the whole plain-data region saves in ONE write, the way
// via_t, scc_t and rtc_t do.  This suite exists so the next field added to
// that region is caught if it does not survive.

#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "scsi_internal.h"
#include "test_assert.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// A recording checkpoint stream
// ============================================================

static uint8_t s_cp_buf[262144];
static size_t s_cp_w, s_cp_r;

static void cp_rewind(void) {
    s_cp_r = 0;
}
static void cp_reset(void) {
    s_cp_w = s_cp_r = 0;
}

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag;
    (void)cp, (void)file, (void)line;
    ASSERT_TRUE(s_cp_w + size <= sizeof(s_cp_buf));
    memcpy(s_cp_buf + s_cp_w, data, size);
    s_cp_w += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag;
    (void)cp, (void)file, (void)line;
    ASSERT_TRUE(s_cp_r + size <= s_cp_w);
    memcpy(data, s_cp_buf + s_cp_r, size);
    s_cp_r += size;
}

// ============================================================
// Link stubs
// ============================================================

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
// No scheduler here: these tests never let time pass.  A NULL scheduler means
// a DATA OUT settle completes immediately (there is nothing to wait for), which
// is what the bus does when it cannot read a clock.
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
// Tests
// ============================================================

// Stage state across the whole plain-data region, round-trip it, compare.
//
// Every field here was chosen because losing it changes what a driver is told:
// sense is what REQUEST SENSE reports, prevent_removal decides whether an
// eject is refused, default_block_size is what a bus reset restores to, and
// loopback is whether the diagnostic card is fitted.
TEST(test_device_state_survives_a_round_trip) {
    scsi_t *a = scsi_init(NULL);
    ASSERT_TRUE(a != NULL);
    scsi_add_device(a, 3, "SONY", "CD-ROM CDU-8002", "1.8g", NULL, scsi_dev_cdrom, 2048, true);

    // The exact case measured in suite-quadra: a pending attention whose cause
    // is the bus reset, not a media change.
    a->devices[3].unit_attention = true;
    a->devices[3].sense.key = SENSE_UNIT_ATTENTION;
    a->devices[3].sense.asc = ASC_POWER_ON_OR_RESET;
    a->devices[3].sense.ascq = 0x00;
    a->devices[3].prevent_removal = true;
    a->devices[3].block_size = 512; // A/UX switches the CD to 512-byte blocks
    a->loopback = true;
    a->bus.phase = scsi_data_in;
    a->cmd.opcode = 0x28;
    a->cmd.lba = 0x1234;

    cp_reset();
    scsi_checkpoint(a, (checkpoint_t *)1);
    cp_rewind();

    scsi_t *b = scsi_init((checkpoint_t *)1);
    ASSERT_TRUE(b != NULL);

    ASSERT_EQ_INT(b->devices[3].sense.key, SENSE_UNIT_ATTENTION);
    ASSERT_EQ_INT(b->devices[3].sense.asc, ASC_POWER_ON_OR_RESET);
    ASSERT_EQ_INT(b->devices[3].sense.ascq, 0x00);
    ASSERT_TRUE(b->devices[3].unit_attention);
    ASSERT_TRUE(b->devices[3].prevent_removal);
    ASSERT_EQ_INT(b->devices[3].block_size, 512);
    ASSERT_EQ_INT(b->devices[3].default_block_size, 2048); // what a reset restores to
    ASSERT_TRUE(b->loopback);
    ASSERT_EQ_INT(b->bus.phase, scsi_data_in);
    ASSERT_EQ_INT(b->cmd.opcode, 0x28);
    ASSERT_EQ_INT(b->cmd.lba, 0x1234);

    scsi_delete(b);
    scsi_delete(a);
}

// A DATA OUT that has been commanded but not yet settled.  This used to be
// three fields recording a held "primer" byte and the CPU program counter that
// wrote it; the chip no longer looks at the program counter, and the state that
// matters now belongs to the BUS: the target has taken the command and is
// preparing, with REQ low until it is ready.  A checkpoint taken in that window
// has to come back in it, or the restored machine starts accepting payload the
// target never asked for.
TEST(test_a_pending_data_out_settle_survives_a_round_trip) {
    scsi_t *a = scsi_init(NULL);
    ASSERT_TRUE(scsi_5380_attach(a, NULL) != NULL);
    a->bus.phase = scsi_data_out; // phase lines valid...
    a->bus.data_out_pending = true;
    a->bus.data_out_ready_cy = 0x1234ABCDu;
    a->bus.req = false;

    cp_reset();
    scsi_checkpoint(a, (checkpoint_t *)1);
    cp_rewind();

    scsi_t *b = scsi_init((checkpoint_t *)1);
    ASSERT_TRUE(scsi_5380_attach(b, (checkpoint_t *)1) != NULL);

    ASSERT_EQ_INT((int)b->bus.phase, (int)scsi_data_out);
    ASSERT_TRUE(b->bus.data_out_pending);
    ASSERT_TRUE(b->bus.data_out_ready_cy == 0x1234ABCDu);
    ASSERT_TRUE(!b->bus.req); // still not asking for data

    scsi_delete(a);
    scsi_delete(b);
}

// The 5380's own block: pin levels and the pseudo-DMA gates.  Losing these
// restores a chip that says it is driving no interrupt and has no transfer in
// flight, whatever it was actually doing.
TEST(test_5380_state_survives_a_round_trip) {
    scsi_t *a = scsi_init(NULL);
    ASSERT_TRUE(scsi_5380_attach(a, NULL) != NULL);
    a->chip5380->reg.mr = MR_DMA;
    a->chip5380->reg.icr = ICR_ACK;
    a->chip5380->end_of_dma = true;
    a->chip5380->dma_write_armed = true;
    a->chip5380->cdr_idx = 2;
    a->chip5380->drq_evt_registered = true; // must NOT come back

    cp_reset();
    scsi_checkpoint(a, (checkpoint_t *)1);
    cp_rewind();

    scsi_t *b = scsi_init((checkpoint_t *)1);
    ASSERT_TRUE(scsi_5380_attach(b, (checkpoint_t *)1) != NULL);

    ASSERT_EQ_INT(b->chip5380->reg.mr, MR_DMA);
    ASSERT_EQ_INT(b->chip5380->reg.icr, ICR_ACK);
    // irq_active/drq_active are deliberately NOT asserted here.  They are
    // tracked PIN levels, derived from the register file, the bus phase and the
    // buffer -- all of which the block above restores -- so the restore
    // recomputes them through scsi_update_irq/_drq and re-drives the machine's
    // wiring with the answer.  Restoring a cached output over a recomputed one
    // would be the worse of the two.
    ASSERT_TRUE(b->chip5380->end_of_dma);
    ASSERT_TRUE(b->chip5380->dma_write_armed);
    ASSERT_EQ_INT(b->chip5380->cdr_idx, 2);

    // Below the line in struct scsi_5380, and for a reason: it records that
    // THIS process registered the DRQ event type.  Coming back true would make
    // a fresh process skip the registration and lose the event.
    ASSERT_TRUE(!b->chip5380->drq_evt_registered);

    scsi_delete(b);
    scsi_delete(a);
}

// A bus with no 5380 -- a Quadra, a PowerMac -- writes no chip block, and the
// restore must not go looking for one.
TEST(test_busless_round_trip_is_symmetric) {
    scsi_t *a = scsi_init(NULL);
    scsi_add_device(a, 0, "GS", "SCRATCH", "1.0", NULL, scsi_dev_hd, 512, false);
    a->devices[0].sense.key = SENSE_NOT_READY;
    a->devices[0].sense.asc = ASC_SONY_CADDY_NOT_INSERTED;

    cp_reset();
    scsi_checkpoint(a, (checkpoint_t *)1);
    size_t written = s_cp_w;
    cp_rewind();

    scsi_t *b = scsi_init((checkpoint_t *)1);
    ASSERT_EQ_INT(b->devices[0].sense.key, SENSE_NOT_READY);
    ASSERT_EQ_INT(b->devices[0].sense.asc, ASC_SONY_CADDY_NOT_INSERTED);
    ASSERT_TRUE(b->chip5380 == NULL);
    // The whole stream was consumed: no chip block was written, and none read.
    ASSERT_TRUE(s_cp_r == written);

    scsi_delete(b);
    scsi_delete(a);
}

// ============================================================================
// F-21: no host pointers in the stream
// ============================================================================
//
// scsi_53c96_checkpoint wrote sizeof(*c) -- the whole struct, pointers and all.
// The restore overwrote them coming back in, so it was never unsafe, but it put
// ASLR-dependent host addresses in the file: measured at 32 of 88 bytes on a
// Quadra.  The same machine saved twice produced different files, so "save,
// save again, diff" could not verify anything.
//
// The struct was already ordered plain-data-first; only the bound was wrong.

// Does the recorded stream contain this pointer's bytes anywhere?
static bool stream_contains_pointer(const void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!v)
        return false; // a NULL is not a leak
    for (size_t i = 0; i + sizeof(v) <= s_cp_w; i++)
        if (memcmp(s_cp_buf + i, &v, sizeof(v)) == 0)
            return true;
    return false;
}

static void dummy_irq(void *ctx, bool level) {
    (void)ctx, (void)level;
}

TEST(test_53c96_writes_no_host_pointers) {
    scsi_t *bus = scsi_init(NULL);
    scsi_53c96_t *c = scsi_53c96_init(NULL, 25000000, NULL);
    ASSERT_TRUE(c != NULL);
    scsi_53c96_attach_bus(c, bus);
    scsi_53c96_set_irq_callback(c, dummy_irq, bus);

    cp_reset();
    scsi_53c96_checkpoint(c, (checkpoint_t *)1);

    // Every pointer the chip is holding, and the chip's own address.
    ASSERT_TRUE(!stream_contains_pointer(bus));
    ASSERT_TRUE(!stream_contains_pointer((void *)(uintptr_t)dummy_irq));
    ASSERT_TRUE(!stream_contains_pointer(c));

    scsi_53c96_delete(c);
    scsi_delete(bus);
}

// ...and the programmer-visible state still survives, so the narrower bound did
// not cost anything.  Driven through the register file, because struct
// scsi_53c96 is private to its translation unit -- which is the right way to
// test it anyway: these are the bytes a driver can actually observe.
TEST(test_53c96_state_survives_a_round_trip) {
    scsi_53c96_t *a = scsi_53c96_init(NULL, 25000000, NULL);
    ASSERT_TRUE(a != NULL);
    scsi_53c96_write(a, 0x8, 0x47); // Config 1: bus ID + parity enables
    scsi_53c96_write(a, 0xB, 0x08); // Config 2: SCSI-2 features
    scsi_53c96_write(a, 0xC, 0x04); // Config 3
    scsi_53c96_write(a, 0x0, 0x34); // transfer count low
    scsi_53c96_write(a, 0x1, 0x12); // transfer count high

    cp_reset();
    scsi_53c96_checkpoint(a, (checkpoint_t *)1);
    cp_rewind();

    scsi_53c96_t *b = scsi_53c96_init(NULL, 25000000, (checkpoint_t *)1);
    ASSERT_TRUE(b != NULL);
    ASSERT_EQ_INT(scsi_53c96_read(b, 0x8), 0x47);
    ASSERT_EQ_INT(scsi_53c96_read(b, 0xB), 0x08);
    ASSERT_EQ_INT(scsi_53c96_read(b, 0xC), 0x04);

    scsi_53c96_delete(b);
    scsi_53c96_delete(a);
}

// The shared selection time-out does not cross a checkpoint, and must not try.
//
// F-18 moved the wait into the bus so every controller uses one implementation.
// That makes its restore behaviour shared too: the armed callback is a host
// function pointer and its context a host address, so neither may be written
// (F-21), and a restore lands with nothing armed.  Stated once on
// scsi_bus_arm_select_timeout() in scsi.h; pinned here so it stays true.
static void dummy_seltmo(void *ctx) {
    (void)ctx;
}

TEST(test_armed_select_timeout_does_not_cross_a_checkpoint) {
    scsi_t *a = scsi_init(NULL);
    ASSERT_TRUE(a != NULL);
    // Set the fields directly: with no scheduler under the suite the arming
    // helper reports immediately rather than leaving anything armed, which is
    // itself the documented no-scheduler behaviour.
    a->seltmo_fn = dummy_seltmo;
    a->seltmo_ctx = a;
    a->seltmo_registered = true;

    cp_reset();
    scsi_checkpoint(a, (checkpoint_t *)1);

    // Neither the callback nor its context reached the stream.
    ASSERT_TRUE(!stream_contains_pointer((void *)(uintptr_t)dummy_seltmo));
    ASSERT_TRUE(!stream_contains_pointer(a));

    cp_rewind();
    scsi_t *b = scsi_init((checkpoint_t *)1);
    ASSERT_TRUE(b->seltmo_fn == NULL);
    ASSERT_TRUE(b->seltmo_ctx == NULL);
    ASSERT_TRUE(!b->seltmo_registered); // a scheduler registration is per-process

    scsi_delete(b);
    scsi_delete(a);
}

int main(void) {
    RUN(test_device_state_survives_a_round_trip);
    RUN(test_a_pending_data_out_settle_survives_a_round_trip);
    RUN(test_5380_state_survives_a_round_trip);
    RUN(test_busless_round_trip_is_symmetric);
    RUN(test_53c96_writes_no_host_pointers);
    RUN(test_53c96_state_survives_a_round_trip);
    RUN(test_armed_select_timeout_does_not_cross_a_checkpoint);
    printf("All scsi_checkpoint tests passed\n");
    return 0;
}
