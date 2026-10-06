// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// New Age floppy controller unit test.
//
// The real core/peripherals/new_age.c against a fake SuperDrive (the floppy.h
// accessors it calls), a fake 1.44 MB image and a fake scheduler that fires
// events in time order.  Each test replays a register sequence the shipped
// AV driver (NewAgeDrvr.a) issues and checks what the chip answers.

#include "floppy.h"
#include "floppy_geometry.h"
#include "image.h"
#include "new_age.h"
#include "scheduler.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Fake drive and medium
// ============================================================================

static image_t s_img; // only `writable` is read through the struct
static uint8_t s_disk[80 * 2 * 18 * 512]; // a 1.44 MB MFM medium
static bool s_inserted = true;
static bool s_motor = false;
static int s_track = 5;

int floppy_drive_count(const floppy_t *f) {
    (void)f;
    return 1;
}
image_t *floppy_drive_image(const floppy_t *f, unsigned d) {
    (void)f;
    return (d == 0 && s_inserted) ? &s_img : NULL;
}
bool floppy_drive_motor_on(const floppy_t *f, unsigned d) {
    (void)f;
    return d == 0 && s_motor;
}
int floppy_drive_track(const floppy_t *f, unsigned d) {
    (void)f;
    (void)d;
    return s_track;
}
void floppy_mech_step(floppy_t *f, unsigned d, bool outward, int count) {
    (void)f;
    (void)d;
    s_track += outward ? -count : count;
    if (s_track < 0)
        s_track = 0;
}
void floppy_mech_set_motor(floppy_t *f, unsigned d, bool on) {
    (void)f;
    (void)d;
    s_motor = on;
}
bool floppy_drive_eject(floppy_t *f, unsigned d) {
    (void)f;
    (void)d;
    bool was = s_inserted;
    s_inserted = false;
    return was;
}
bool floppy_media_current(struct floppy *f, unsigned d, floppy_media_t *m) {
    (void)f;
    memset(m, 0, sizeof(*m));
    if (d != 0 || !s_inserted)
        return false;
    m->img = &s_img;
    m->valid = true;
    m->format = FLOPPY_FMT_MFM_1440K;
    m->mfm = true;
    m->hd = true;
    m->sides = 2;
    m->mfm_spt = 18;
    m->fmt_byte = 0x02;
    return true;
}
void floppy_media_set_format(struct floppy *f, unsigned d, floppy_format_t fmt) {
    (void)f;
    (void)d;
    (void)fmt;
}
int floppy_media_spt(const floppy_media_t *m, int track) {
    (void)track;
    return m && m->valid ? m->mfm_spt : 0;
}
size_t floppy_media_sector_offset(const floppy_media_t *m, int track, int side, int sector) {
    return ((size_t)(track * m->sides + side) * (size_t)m->mfm_spt + (size_t)sector) * 512;
}
int floppy_zone_rpm(int track) {
    (void)track;
    return 394;
}
size_t disk_size(image_t *disk) {
    (void)disk;
    return sizeof s_disk;
}
size_t disk_read_data(image_t *disk, size_t off, uint8_t *buf, size_t size) {
    (void)disk;
    memcpy(buf, s_disk + off, size);
    return size;
}
size_t disk_write_data(image_t *disk, size_t off, uint8_t *buf, size_t size) {
    (void)disk;
    memcpy(s_disk + off, buf, size);
    return size;
}

// ============================================================================
// Fake scheduler: a small queue fired in time order
// ============================================================================

typedef struct {
    bool live;
    double when, interval;
    event_callback_t cb;
    void *src;
    uint64_t data;
} fake_event_t;

static fake_event_t s_ev[16];
static double s_now;

double scheduler_time_ns(struct scheduler *s) {
    (void)s;
    return s_now;
}
void scheduler_new_event_type(struct scheduler *s, const char *source_name, void *source, const char *name,
                              event_callback_t cb) {
    (void)s, (void)source_name, (void)source, (void)name, (void)cb;
}
event_t *scheduler_new_cpu_event_ex(struct scheduler *s, event_callback_t cb, void *src, uint64_t data, uint64_t cycles,
                                    uint64_t ns, bool periodic) {
    (void)s;
    (void)cycles;
    for (int i = 0; i < 16; i++) {
        if (!s_ev[i].live) {
            s_ev[i] = (fake_event_t){true, s_now + (double)ns, periodic ? (double)ns : 0, cb, src, data};
            return NULL;
        }
    }
    ASSERT_TRUE(!"fake scheduler full");
    return NULL;
}
void remove_event_by_data(struct scheduler *s, event_callback_t cb, void *src, uint64_t data) {
    (void)s;
    for (int i = 0; i < 16; i++)
        if (s_ev[i].live && s_ev[i].cb == cb && s_ev[i].src == src && s_ev[i].data == data)
            s_ev[i].live = false;
}

// Fire everything due up to `ns` from now, in time order.
static void advance(double ns) {
    double end = s_now + ns;
    for (;;) {
        int next = -1;
        for (int i = 0; i < 16; i++)
            if (s_ev[i].live && s_ev[i].when <= end && (next < 0 || s_ev[i].when < s_ev[next].when))
                next = i;
        if (next < 0)
            break;
        fake_event_t e = s_ev[next];
        s_now = e.when;
        if (e.interval > 0)
            s_ev[next].when += e.interval;
        else
            s_ev[next].live = false;
        e.cb(e.src, e.data);
    }
    s_now = end;
}

// ============================================================================
// Fake board: an INT line and a DMA channel with a terminal count
// ============================================================================

static bool s_int;
static uint8_t s_dma[16384];
static int s_dma_pos, s_dma_count; // bytes moved; the armed count

static void set_irq(void *ctx, bool level) {
    (void)ctx;
    s_int = level;
}
static int dma_put(void *ctx, uint8_t v) {
    (void)ctx;
    if (s_dma_pos >= s_dma_count)
        return NEW_AGE_DMA_NONE;
    s_dma[s_dma_pos++] = v;
    return s_dma_pos == s_dma_count ? NEW_AGE_DMA_TC : NEW_AGE_DMA_OK;
}
static int dma_get(void *ctx, uint8_t *out) {
    (void)ctx;
    if (s_dma_pos >= s_dma_count)
        return NEW_AGE_DMA_NONE;
    *out = s_dma[s_dma_pos++];
    return s_dma_pos == s_dma_count ? NEW_AGE_DMA_TC : NEW_AGE_DMA_OK;
}

static new_age_t s_na;

static uint8_t msr(void) {
    return new_age_read(&s_na, NEW_AGE_REG_STATUS);
}
static void cmd(const uint8_t *bytes, int n) {
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(msr() & NEW_AGE_MSR_RQM);
        ASSERT_TRUE(!(msr() & NEW_AGE_MSR_DIO));
        new_age_write(&s_na, NEW_AGE_REG_DATA, bytes[i]);
    }
}
static uint8_t result(void) {
    ASSERT_TRUE((msr() & (NEW_AGE_MSR_RQM | NEW_AGE_MSR_DIO | NEW_AGE_MSR_CB)) ==
                (NEW_AGE_MSR_RQM | NEW_AGE_MSR_DIO | NEW_AGE_MSR_CB));
    return new_age_read(&s_na, NEW_AGE_REG_DATA);
}

// The driver's ResetFDC.
static void reset_fdc(void) {
    new_age_write(&s_na, NEW_AGE_REG_STATUS, 0x9C);
    cmd((const uint8_t[]){0x32, 0x03}, 2); // Select Drive Type: Apple
    cmd((const uint8_t[]){0x13, 0x00, 0x0F, 0x00}, 4); // Configure
    cmd((const uint8_t[]){0x03, 0x00, 0x00}, 3); // Specify: DMA
}

static void fresh(void) {
    memset(s_ev, 0, sizeof s_ev);
    memset(&s_na, 0, sizeof s_na);
    s_now = 0;
    s_int = false;
    s_inserted = true;
    s_motor = false;
    s_track = 5;
    s_img.writable = true;
    const new_age_backend_t be = {.dma_put = dma_put, .dma_get = dma_get, .set_irq = set_irq};
    new_age_bind(&s_na, (floppy_t *)&s_img /* any non-NULL handle */, (struct scheduler *)1, &be);
    new_age_reset(&s_na);
    new_age_register_events(&s_na, false);
    reset_fdc();
}

// Collect the pending cause; returns ST0 and fills *pcn when two bytes come.
static uint8_t sense_interrupt(int *len, uint8_t *pcn) {
    cmd((const uint8_t[]){0x08}, 1);
    uint8_t st0 = result();
    *len = 1;
    if (msr() & NEW_AGE_MSR_CB) {
        *pcn = result();
        *len = 2;
    }
    ASSERT_TRUE(!(msr() & NEW_AGE_MSR_CB)); // back to idle
    return st0;
}

// ============================================================================
// Tests
// ============================================================================

// Idle after reset: RQM only, drive 1 "not installed" (one drive cabled).
TEST(test_idle_msr) {
    fresh();
    ASSERT_EQ_INT(msr(), NEW_AGE_MSR_RQM | NEW_AGE_MSR_D1I);
}

// The driver's Open probe: Set Enable Control interrupts at once, Sense
// Drive Status answers without one; a missing drive reads $FF.
TEST(test_open_probe) {
    fresh();
    cmd((const uint8_t[]){0x9B, 0x00}, 2);
    ASSERT_TRUE(s_int);
    cmd((const uint8_t[]){0x04, 0x00}, 2);
    ASSERT_TRUE(!s_int); // command arrival drops INT; ST3 raises none
    uint8_t st3 = result();
    ASSERT_TRUE(st3 != 0xFF);
    ASSERT_EQ_INT(st3 & 0x0C, 0x0C); // SuperDrive: 2 MB class, not a Typhoon
    ASSERT_EQ_INT(st3 & 0x80, 0); // HD medium
    ASSERT_EQ_INT(st3 & 0x40, 0x40); // writable
    cmd((const uint8_t[]){0x1B, 0x00}, 2);
    int len;
    uint8_t pcn;
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0x00);
    ASSERT_EQ_INT(len, 1);
    cmd((const uint8_t[]){0x04, 0x01}, 2);
    ASSERT_EQ_INT(result(), 0xFF); // no drive 1
}

// An Apple-mode-illegal opcode (Dumpreg) and Sense Interrupt Status with
// nothing pending both answer ST0 = $80, one byte.
TEST(test_invalid) {
    fresh();
    advance(200e6); // let the poller report the medium...
    int len;
    uint8_t pcn;
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0xC0); // ...as an insertion
    new_age_write(&s_na, NEW_AGE_REG_DATA, 0x0E);
    ASSERT_EQ_INT(result(), 0x80);
    ASSERT_TRUE(!(msr() & NEW_AGE_MSR_CB));
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0x80);
    ASSERT_EQ_INT(len, 1);
}

// Seek runs in the background with D0B up, then interrupts; Sense
// Interrupt Status returns ST0 with SE and the PCN.  Not ready = EC.
TEST(test_seek) {
    fresh();
    cmd((const uint8_t[]){0x9B, 0x00}, 2);
    int len;
    uint8_t pcn;
    sense_interrupt(&len, &pcn);
    cmd((const uint8_t[]){0x0F, 0x00, 0x10}, 3); // motor off: not ready
    advance(100e6);
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn) & 0xF8, 0x40 | 0x20 | 0x10 | 0x08);
    cmd((const uint8_t[]){0x9A, 0x00}, 2); // motor on: /Ready after spin-up
    ASSERT_TRUE(!s_int);
    advance(1e9);
    ASSERT_TRUE(s_int);
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0x00);
    cmd((const uint8_t[]){0x0F, 0x00, 0x10}, 3);
    ASSERT_TRUE(msr() & NEW_AGE_MSR_D0B);
    ASSERT_TRUE(msr() & NEW_AGE_MSR_RQM); // the chip is free while the drive steps
    advance(1e9);
    ASSERT_TRUE(s_int);
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0x20);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(pcn, 0x10);
    ASSERT_EQ_INT(s_track, 0x10);
    ASSERT_TRUE(!(msr() & NEW_AGE_MSR_D0B));
}

// The /CSTIN poller reports a change only while no drive is enabled.
TEST(test_cstin_poll) {
    fresh();
    cmd((const uint8_t[]){0x9B, 0x00}, 2);
    int len;
    uint8_t pcn;
    sense_interrupt(&len, &pcn);
    advance(500e6);
    ASSERT_TRUE(!s_int); // enabled: no polling
    cmd((const uint8_t[]){0x1B, 0x00}, 2);
    sense_interrupt(&len, &pcn);
    advance(500e6);
    ASSERT_TRUE(s_int);
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0xC0); // inserted, FIN clear
    s_inserted = false;
    advance(500e6);
    ASSERT_EQ_INT(sense_interrupt(&len, &pcn), 0xC2); // removed, FIN set
    advance(500e6);
    ASSERT_TRUE(!s_int); // reported once
}

// Bring the drive up and read sectors 3..5 of cylinder 5, head 1.  The
// launch byte leaves RQM low until the result phase; with the channel's
// count ending on sector 5 the command terminates normally, C unchanged.
static void read_track(int count_bytes, uint8_t *st0, uint8_t *st1, uint8_t *c, uint8_t *r) {
    cmd((const uint8_t[]){0x9B, 0x00}, 2);
    int len;
    uint8_t pcn;
    sense_interrupt(&len, &pcn);
    cmd((const uint8_t[]){0x9A, 0x00}, 2);
    advance(1e9);
    sense_interrupt(&len, &pcn);
    cmd((const uint8_t[]){0x5C, 0x00}, 2); // drive mode MFM
    advance(1e9);
    sense_interrupt(&len, &pcn);
    new_age_write(&s_na, NEW_AGE_REG_STATUS, 0x0C); // 500 kbps, Apple precomp
    s_dma_pos = 0;
    s_dma_count = count_bytes;
    cmd((const uint8_t[]){0x46, 0x04, 5, 1, 3, 0x02, 5, 0x1B, 0x00}, 9);
    ASSERT_TRUE(!(msr() & NEW_AGE_MSR_RQM)); // execution phase
    ASSERT_TRUE(msr() & NEW_AGE_MSR_CB);
    advance(1e9);
    ASSERT_TRUE(s_int);
    *st0 = result();
    ASSERT_TRUE(!s_int); // the first result read drops INT
    *st1 = result();
    result(); // ST2
    *c = result();
    result(); // H
    *r = result();
    ASSERT_EQ_INT(result(), 0x02); // N
    ASSERT_TRUE(!(msr() & NEW_AGE_MSR_CB));
}

TEST(test_read_tc) {
    fresh();
    for (size_t i = 0; i < sizeof s_disk; i++)
        s_disk[i] = (uint8_t)(i / 512);
    uint8_t st0, st1, c, r;
    read_track(3 * 512, &st0, &st1, &c, &r);
    ASSERT_EQ_INT(st0, 0x04); // normal, head 1
    ASSERT_EQ_INT(st1, 0);
    ASSERT_EQ_INT(c, 5); // the cylinder the head is on (see na_update_id)
    ASSERT_EQ_INT(r, 1); // wrapped at EOT
    // Cylinder 5 head 1 sector 3 is image sector (5*2+1)*18 + 2.
    ASSERT_EQ_INT(s_dma[0], (uint8_t)((5 * 2 + 1) * 18 + 2));
    ASSERT_EQ_INT(s_dma[3 * 512 - 1], (uint8_t)((5 * 2 + 1) * 18 + 4));
}

// Without terminal count the chip runs to EOT and ends abnormally with EN.
TEST(test_read_no_tc) {
    fresh();
    uint8_t st0, st1, c, r;
    read_track(8 * 512, &st0, &st1, &c, &r);
    ASSERT_EQ_INT(st0, 0x44);
    ASSERT_EQ_INT(st1, 0x80);
}

// The wrong recording: a GCR Read ID on an MFM medium finds no mark.
TEST(test_wrong_encoding) {
    fresh();
    cmd((const uint8_t[]){0x9A, 0x00}, 2);
    advance(1e9);
    int len;
    uint8_t pcn;
    sense_interrupt(&len, &pcn);
    cmd((const uint8_t[]){0x0A, 0x00}, 2); // drive still in GCR mode
    advance(1e9);
    ASSERT_EQ_INT(result(), 0x40);
    ASSERT_EQ_INT(result(), 0x01); // MA
}

int main(void) {
    RUN(test_idle_msr);
    RUN(test_open_probe);
    RUN(test_invalid);
    RUN(test_seek);
    RUN(test_cstin_poll);
    RUN(test_read_tc);
    RUN(test_read_no_tc);
    RUN(test_wrong_encoding);
    fprintf(stderr, "new_age: all tests passed\n");
    return 0;
}
