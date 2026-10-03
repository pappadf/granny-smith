// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// The ATA channel, driven through its task file the way a host driver does:
// signatures, IDENTIFY, PIO and DMA reads and writes of an ATA hard disk over
// an in-memory medium, the interrupt line, software reset, and an ATAPI
// CD-ROM whose PACKET commands run on the real SCSI CD-ROM model.

#include "ata.h"
#include "image.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_internal.h"
#include "test_assert.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HD_SECTORS 2048u
#define CD_BLOCKS  64u
#define CD_ID      1 // cell 0, device 1

// ============================================================
// The media: two in-memory images
// ============================================================

static image_t s_hd_img, s_cd_img;
static uint8_t s_hd[HD_SECTORS * 512u];
static uint8_t s_cd[CD_BLOCKS * 2048u];

static uint8_t *medium(image_t *img, size_t *size) {
    if (img == &s_hd_img) {
        *size = sizeof s_hd;
        return s_hd;
    }
    if (img == &s_cd_img) {
        *size = sizeof s_cd;
        return s_cd;
    }
    *size = 0;
    return NULL;
}

size_t disk_read_data(image_t *img, size_t off, uint8_t *buf, size_t len) {
    size_t size;
    uint8_t *m = medium(img, &size);
    if (!m || off > size || len > size - off)
        return 0;
    memcpy(buf, m + off, len);
    return len;
}
size_t disk_write_data(image_t *img, size_t off, uint8_t *buf, size_t len) {
    size_t size;
    uint8_t *m = medium(img, &size);
    if (!m || off > size || len > size - off)
        return 0;
    memcpy(m + off, buf, len);
    return len;
}
size_t disk_size(image_t *img) {
    size_t size;
    (void)medium(img, &size);
    return size;
}
uint32_t disk_block_size(image_t *img) {
    (void)img;
    return 512;
}
const char *image_get_filename(const image_t *img) {
    return img == &s_hd_img ? "hd.img" : img == &s_cd_img ? "cd.iso" : NULL;
}
image_t *setup_get_image_by_filename(const char *filename) {
    if (filename && strcmp(filename, "hd.img") == 0)
        return &s_hd_img;
    if (filename && strcmp(filename, "cd.iso") == 0)
        return &s_cd_img;
    return NULL;
}

// scsi_init flags a checkpoint whose bus lacks the CD bay's drive.
void checkpoint_set_error(checkpoint_t *checkpoint) {
    (void)checkpoint;
}

// ============================================================
// A checkpoint stream in memory
// ============================================================

static uint8_t *s_cp;
static size_t s_cp_len, s_cp_cap, s_cp_pos;

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *d, size_t n, const char *tag, const char *f,
                                      int l) {
    (void)cp, (void)tag, (void)f, (void)l;
    if (s_cp_len + n > s_cp_cap) {
        s_cp_cap = (s_cp_len + n) * 2;
        s_cp = realloc(s_cp, s_cp_cap);
    }
    memcpy(s_cp + s_cp_len, d, n);
    s_cp_len += n;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *d, size_t n, const char *tag, const char *f, int l) {
    (void)cp, (void)tag, (void)f, (void)l;
    if (s_cp_pos + n > s_cp_len) {
        memset(d, 0, n);
        return;
    }
    memcpy(d, s_cp + s_cp_pos, n);
    s_cp_pos += n;
}

// ============================================================
// Link stubs for the SCSI bus model
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
int system_hd_attach(const char *path, int scsi_id) {
    (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom(struct config *restrict config, const char *filename, int scsi_id) {
    (void)config, (void)filename, (void)scsi_id;
    return false;
}
int system_hd_attach_on(struct scsi *bus, const char *path, int scsi_id) {
    (void)bus, (void)path, (void)scsi_id;
    return -1;
}
bool add_scsi_cdrom_on(struct config *restrict config, struct scsi *bus, const char *filename, int scsi_id) {
    (void)config, (void)bus, (void)filename, (void)scsi_id;
    return false;
}
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
bool has_event(struct scheduler *restrict s, event_callback_t cb) {
    (void)s, (void)cb;
    return false;
}
void scheduler_forget_source(struct scheduler *sch, void *source) {
    (void)sch;
    (void)source;
}
const char *image_path(const image_t *img) {
    return image_get_filename(img);
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

// ============================================================
// Harness
// ============================================================

static ata_channel_t s_ch;
static scsi_t *s_bus;
static int s_line, s_kicks;

static void on_irq(void *ctx, bool level) {
    (void)ctx;
    s_line = level ? 1 : 0;
}
static void on_kick(void *ctx) {
    (void)ctx;
    s_kicks++;
}

// Cell 0: an ATA disk as device 0, an ATAPI CD-ROM as device 1.
static void setup(void) {
    s_line = s_kicks = 0;
    for (size_t i = 0; i < sizeof s_hd; i++)
        s_hd[i] = (uint8_t)(i * 7u + (i >> 9));
    for (size_t i = 0; i < sizeof s_cd; i++)
        s_cd[i] = (uint8_t)(i * 13u + (i >> 11));
    s_cd_img.type = image_cdrom;
    s_cd_img.raw_size = sizeof s_cd; // what the bus model's range check reads
    s_hd_img.raw_size = sizeof s_hd;
    s_bus = scsi_init_named(NULL, "atapi");
    ASSERT_TRUE(s_bus != NULL);
    scsi_add_device(s_bus, CD_ID, "GS", "CD-ROM", "1.0", &s_cd_img, scsi_dev_cdrom, 2048, true);
    ata_channel_init(&s_ch, 0);
    ata_set_irq(&s_ch, on_irq, NULL);
    ata_set_dma_kick(&s_ch, on_kick, NULL);
    ata_set_atapi_bus(&s_ch, s_bus);
    ASSERT_TRUE(ata_attach_hd(&s_ch, 0, &s_hd_img));
    ASSERT_TRUE(ata_attach_atapi(&s_ch, 1, CD_ID));
    ata_set_enabled(&s_ch, true);
    ata_hard_reset(&s_ch);
}

static void teardown(void) {
    ata_channel_free(&s_ch);
    scsi_delete(s_bus);
    s_bus = NULL;
}

static uint8_t rd(int reg) {
    return ata_read(&s_ch, reg);
}
static void wr(int reg, uint8_t v) {
    ata_write(&s_ch, reg, v);
}

// A host's LBA28 task-file setup on the current device.
static void lba(uint32_t sector, uint8_t count, int dev) {
    wr(ATA_REG_SELECT, (uint8_t)(0xE0u | (dev << 4) | ((sector >> 24) & 0xFu)));
    wr(ATA_REG_NSECT, count);
    wr(ATA_REG_SECTOR, (uint8_t)sector);
    wr(ATA_REG_LCYL, (uint8_t)(sector >> 8));
    wr(ATA_REG_HCYL, (uint8_t)(sector >> 16));
}

// Read `n` bytes through the data port into `out` (the stream's order).
static void pio_in(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i += 2) {
        uint16_t w = ata_read_data16(&s_ch);
        out[i] = (uint8_t)(w >> 8);
        out[i + 1] = (uint8_t)w;
    }
}

static void pio_out(const uint8_t *in, size_t n) {
    for (size_t i = 0; i < n; i += 2)
        ata_write_data16(&s_ch, (uint16_t)(in[i] << 8 | in[i + 1]));
}

// The ATA word `w` of a 512-byte IDENTIFY block in stream order.
static uint16_t id_word(const uint8_t *b, int w) {
    return (uint16_t)(b[2 * w] | b[2 * w + 1] << 8);
}

static void packet(const uint8_t cdb[12], uint16_t limit, bool dma) {
    wr(ATA_REG_SELECT, 0xA0u | 0x10u);
    wr(ATA_REG_ERROR, dma ? 1 : 0);
    wr(ATA_REG_LCYL, (uint8_t)limit);
    wr(ATA_REG_HCYL, (uint8_t)(limit >> 8));
    wr(ATA_REG_STATUS, 0xA0);
    ASSERT_EQ_INT(ata_read_altstatus(&s_ch) & (ATA_ST_DRQ | ATA_ST_BSY), ATA_ST_DRQ);
    ASSERT_EQ_INT(rd(ATA_REG_NSECT) & 3, 1); // CoD: the packet goes next
    pio_out(cdb, 12);
}

// The first command after power-on draws UNIT ATTENTION from the CD-ROM;
// a host driver clears it with TEST UNIT READY before it reads.
static void clear_unit_attention(void) {
    const uint8_t tur[12] = {0x00};
    packet(tur, 0, false);
    (void)rd(ATA_REG_STATUS);
}

// ============================================================
// Tests
// ============================================================

// After a reset each device leaves its own signature (ATA-4): the
// disk 1/1/00/00 with DRDY, the ATAPI device 1/1/14/EB with status 0.
TEST(each_device_reads_back_its_own_signature) {
    setup();
    wr(ATA_REG_SELECT, 0xA0);
    uint8_t st = rd(ATA_REG_STATUS);
    uint8_t nsect = rd(ATA_REG_NSECT), lcyl = rd(ATA_REG_LCYL), hcyl = rd(ATA_REG_HCYL);
    ASSERT_EQ_INT(st, ATA_ST_DRDY | ATA_ST_DSC);
    ASSERT_EQ_INT(nsect, 1);
    ASSERT_EQ_INT(lcyl, 0);
    ASSERT_EQ_INT(hcyl, 0);
    wr(ATA_REG_SELECT, 0xB0);
    st = rd(ATA_REG_STATUS);
    lcyl = rd(ATA_REG_LCYL);
    hcyl = rd(ATA_REG_HCYL);
    ASSERT_EQ_INT(st, 0);
    ASSERT_EQ_INT(lcyl, 0x14);
    ASSERT_EQ_INT(hcyl, 0xEB);
    teardown();
}

TEST(identify_device_describes_the_disk) {
    setup();
    wr(ATA_REG_SELECT, 0xA0);
    wr(ATA_REG_STATUS, 0xEC);
    ASSERT_EQ_INT(s_line, 1);
    uint8_t st = rd(ATA_REG_STATUS);
    ASSERT_TRUE(st & ATA_ST_DRQ);
    ASSERT_EQ_INT(s_line, 0); // reading Status acknowledges INTRQ
    uint8_t b[512];
    pio_in(b, sizeof b);
    ASSERT_EQ_INT(id_word(b, 0), 0x0040);
    ASSERT_EQ_INT(id_word(b, 60) | (uint32_t)id_word(b, 61) << 16, HD_SECTORS);
    ASSERT_TRUE(id_word(b, 49) & 0x0200); // LBA
    ASSERT_EQ_INT(b[27 * 2 + 1], 'G'); // ATA string: first character high
    ASSERT_EQ_INT(b[27 * 2], 'S');
    uint8_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum = (uint8_t)(sum + b[i]);
    ASSERT_EQ_INT(sum, 0); // the integrity word balances the block
    st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st, ATA_ST_DRDY | ATA_ST_DSC);
    teardown();
}

TEST(pio_read_streams_the_sectors_and_interrupts_per_block) {
    setup();
    lba(5, 2, 0);
    wr(ATA_REG_STATUS, 0x20);
    uint8_t b[1024];
    ASSERT_EQ_INT(s_line, 1);
    (void)rd(ATA_REG_STATUS);
    pio_in(b, 512);
    ASSERT_EQ_INT(s_line, 1); // the second block's DRQ
    (void)rd(ATA_REG_STATUS);
    pio_in(b + 512, 512);
    ASSERT_TRUE(memcmp(b, s_hd + 5 * 512, 1024) == 0);
    uint8_t st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st, ATA_ST_DRDY | ATA_ST_DSC);
    ASSERT_EQ_INT(s_line, 0); // no interrupt after the last read block
    uint8_t sector = rd(ATA_REG_SECTOR);
    ASSERT_EQ_INT(sector, 6); // the task file holds the last sector moved
    teardown();
}

TEST(pio_write_lands_on_the_medium) {
    setup();
    uint8_t b[512];
    for (int i = 0; i < 512; i++)
        b[i] = (uint8_t)(0xA5 ^ i);
    lba(100, 1, 0);
    wr(ATA_REG_STATUS, 0x30);
    ASSERT_EQ_INT(s_line, 0); // the first write block needs no interrupt
    ASSERT_TRUE(ata_read_altstatus(&s_ch) & ATA_ST_DRQ);
    pio_out(b, 512);
    ASSERT_EQ_INT(s_line, 1);
    ASSERT_TRUE(memcmp(s_hd + 100 * 512, b, 512) == 0);
    teardown();
}

TEST(read_multiple_needs_set_multiple_first) {
    setup();
    lba(0, 4, 0);
    wr(ATA_REG_STATUS, 0xC4);
    uint8_t st = rd(ATA_REG_STATUS), er = rd(ATA_REG_ERROR);
    ASSERT_TRUE(st & ATA_ST_ERR);
    ASSERT_EQ_INT(er, ATA_ER_ABRT);
    wr(ATA_REG_NSECT, 4);
    wr(ATA_REG_STATUS, 0xC6);
    st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st & ATA_ST_ERR, 0);
    lba(8, 4, 0);
    wr(ATA_REG_STATUS, 0xC4);
    (void)rd(ATA_REG_STATUS);
    uint8_t b[2048];
    pio_in(b, sizeof b); // one 4-sector DRQ block
    ASSERT_TRUE(memcmp(b, s_hd + 8 * 512, sizeof b) == 0);
    st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st & ATA_ST_DRQ, 0);
    teardown();
}

TEST(dma_read_moves_through_the_port_then_interrupts) {
    setup();
    lba(20, 3, 0);
    wr(ATA_REG_STATUS, 0xC8);
    ASSERT_EQ_INT(s_kicks, 1);
    ASSERT_EQ_INT(s_line, 0);
    uint8_t b[1536];
    int got = ata_dma_in(&s_ch, b, 1000);
    ASSERT_EQ_INT(got, 1000);
    got += ata_dma_in(&s_ch, b + 1000, 1000); // asks for more than is left
    ASSERT_EQ_INT(got, 1536);
    ASSERT_TRUE(memcmp(b, s_hd + 20 * 512, sizeof b) == 0);
    ASSERT_EQ_INT(s_line, 1);
    ASSERT_EQ_INT(ata_dma_in(&s_ch, b, 16), 0); // nothing more to move
    teardown();
}

TEST(dma_write_lands_on_the_medium) {
    setup();
    uint8_t b[1024];
    for (int i = 0; i < 1024; i++)
        b[i] = (uint8_t)(i * 3);
    lba(40, 2, 0);
    wr(ATA_REG_STATUS, 0xCA);
    ASSERT_EQ_INT(ata_dma_out(&s_ch, b, 1024), 1024);
    ASSERT_EQ_INT(s_line, 1);
    ASSERT_TRUE(memcmp(s_hd + 40 * 512, b, 1024) == 0);
    teardown();
}

TEST(a_sector_past_the_end_is_an_id_not_found) {
    setup();
    lba(HD_SECTORS - 1, 2, 0);
    wr(ATA_REG_STATUS, 0x20);
    uint8_t st = rd(ATA_REG_STATUS), er = rd(ATA_REG_ERROR);
    ASSERT_TRUE(st & ATA_ST_ERR);
    ASSERT_TRUE(er & ATA_ER_IDNF);
    teardown();
}

TEST(nien_holds_the_line_low) {
    setup();
    ata_write_devctl(&s_ch, ATA_DC_NIEN);
    wr(ATA_REG_SELECT, 0xA0);
    wr(ATA_REG_STATUS, 0xE7); // FLUSH CACHE: completes with an interrupt
    ASSERT_EQ_INT(s_line, 0);
    ata_write_devctl(&s_ch, 0);
    ASSERT_EQ_INT(s_line, 1); // still pending until Status is read
    (void)rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(s_line, 0);
    teardown();
}

TEST(software_reset_restores_the_signatures) {
    setup();
    wr(ATA_REG_SELECT, 0xB0);
    wr(ATA_REG_LCYL, 0x55);
    ata_write_devctl(&s_ch, ATA_DC_SRST);
    ASSERT_TRUE(ata_read_altstatus(&s_ch) & ATA_ST_BSY);
    ata_write_devctl(&s_ch, 0);
    uint8_t sel = rd(ATA_REG_SELECT);
    ASSERT_EQ_INT(sel & 0x10, 0); // device 0 selected again
    wr(ATA_REG_SELECT, 0xB0);
    uint8_t lcyl = rd(ATA_REG_LCYL);
    ASSERT_EQ_INT(lcyl, 0x14);
    teardown();
}

TEST(an_empty_or_disabled_cell_floats) {
    setup();
    ata_set_atapi_bus(&s_ch, NULL); // no back end: the CD drive is gone too
    ata_detach(&s_ch, 0);
    ata_detach(&s_ch, 1);
    uint8_t st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st, 0x7F);
    ata_set_enabled(&s_ch, false);
    st = rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(st, 0xFF);
    teardown();
}

// A CD-ROM attached to the back end after the channel was built shows up on
// the next register access; the drive stays when its disc is ejected.
TEST(a_cd_drive_on_the_back_end_appears_on_the_channel) {
    setup();
    ata_detach(&s_ch, 1);
    ASSERT_EQ_INT(ata_device_kind(&s_ch, 1), ATA_DEV_NONE);
    wr(ATA_REG_SELECT, 0xB0);
    uint8_t hcyl = rd(ATA_REG_HCYL);
    ASSERT_EQ_INT(ata_device_kind(&s_ch, 1), ATA_DEV_ATAPI);
    ASSERT_EQ_INT(hcyl, 0xEB);
    scsi_eject_device(s_bus, CD_ID);
    (void)rd(ATA_REG_STATUS);
    ASSERT_EQ_INT(ata_device_kind(&s_ch, 1), ATA_DEV_ATAPI);
    teardown();
}

TEST(atapi_device_aborts_identify_device_and_answers_identify_packet) {
    setup();
    wr(ATA_REG_SELECT, 0xB0);
    wr(ATA_REG_STATUS, 0xEC);
    uint8_t st = rd(ATA_REG_STATUS), er = rd(ATA_REG_ERROR), hcyl = rd(ATA_REG_HCYL);
    ASSERT_TRUE(st & ATA_ST_ERR);
    ASSERT_EQ_INT(er, ATA_ER_ABRT);
    ASSERT_EQ_INT(hcyl, 0xEB);
    wr(ATA_REG_STATUS, 0xA1);
    (void)rd(ATA_REG_STATUS);
    uint8_t b[512];
    pio_in(b, sizeof b);
    ASSERT_EQ_INT(id_word(b, 0), 0x85C0);
    teardown();
}

TEST(atapi_inquiry_comes_back_in_one_chunk) {
    setup();
    const uint8_t cdb[12] = {0x12, 0, 0, 0, 36, 0};
    packet(cdb, 0xFFFE, false);
    ASSERT_EQ_INT(s_line, 1);
    uint8_t st = rd(ATA_REG_STATUS), ir = rd(ATA_REG_NSECT);
    uint8_t lo = rd(ATA_REG_LCYL), hi = rd(ATA_REG_HCYL);
    ASSERT_TRUE(st & ATA_ST_DRQ);
    ASSERT_EQ_INT(ir & 3, 2); // IO, data to the host
    ASSERT_EQ_INT(lo | hi << 8, 36);
    uint8_t b[36];
    pio_in(b, sizeof b);
    ASSERT_EQ_INT(b[0] & 0x1F, 5); // CD-ROM
    ASSERT_EQ_INT(s_line, 1); // completion
    st = rd(ATA_REG_STATUS);
    ir = rd(ATA_REG_NSECT);
    ASSERT_EQ_INT(st & (ATA_ST_DRQ | ATA_ST_ERR), 0);
    ASSERT_EQ_INT(ir & 3, 3);
    teardown();
}

TEST(atapi_read_splits_at_the_byte_count_limit) {
    setup();
    clear_unit_attention();
    const uint8_t cdb[12] = {0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0}; // READ(10) LBA 2, 2 blocks
    packet(cdb, 2048, false);
    uint8_t b[4096];
    for (int chunk = 0; chunk < 2; chunk++) {
        uint8_t st = rd(ATA_REG_STATUS);
        uint8_t lo = rd(ATA_REG_LCYL), hi = rd(ATA_REG_HCYL);
        ASSERT_EQ_INT(st & (ATA_ST_DRQ | ATA_ST_ERR), ATA_ST_DRQ);
        ASSERT_EQ_INT(lo | hi << 8, 2048);
        pio_in(b + chunk * 2048, 2048);
    }
    ASSERT_TRUE(memcmp(b, s_cd + 2 * 2048, sizeof b) == 0);
    uint8_t ir = rd(ATA_REG_NSECT);
    ASSERT_EQ_INT(ir & 3, 3);
    teardown();
}

TEST(atapi_read_by_dma) {
    setup();
    clear_unit_attention();
    const uint8_t cdb[12] = {0x28, 0, 0, 0, 0, 7, 0, 0, 1, 0};
    packet(cdb, 0, true);
    ASSERT_EQ_INT(s_kicks, 1);
    uint8_t b[2048];
    ASSERT_EQ_INT(ata_dma_in(&s_ch, b, 2048), 2048);
    ASSERT_TRUE(memcmp(b, s_cd + 7 * 2048, sizeof b) == 0);
    ASSERT_EQ_INT(s_line, 1);
    teardown();
}

TEST(atapi_check_condition_reports_the_sense_key) {
    setup();
    scsi_eject_device(s_bus, CD_ID);
    const uint8_t cdb[12] = {0x00}; // TEST UNIT READY with no disc
    packet(cdb, 0, false);
    uint8_t st = rd(ATA_REG_STATUS), er = rd(ATA_REG_ERROR);
    ASSERT_TRUE(st & ATA_ST_ERR);
    ASSERT_EQ_INT(er >> 4, 6); // UNIT ATTENTION: the medium changed
    packet(cdb, 0, false);
    st = rd(ATA_REG_STATUS);
    er = rd(ATA_REG_ERROR);
    ASSERT_TRUE(st & ATA_ST_ERR);
    ASSERT_EQ_INT(er >> 4, 2); // then NOT READY
    teardown();
}

TEST(a_checkpoint_mid_read_resumes_the_stream) {
    setup();
    lba(30, 2, 0);
    wr(ATA_REG_STATUS, 0x20);
    uint8_t b[1024];
    pio_in(b, 256); // part way into the first block
    s_cp_len = s_cp_pos = 0;
    ata_checkpoint_save(&s_ch, NULL);
    ata_channel_free(&s_ch);
    ata_channel_init(&s_ch, 0);
    ata_set_irq(&s_ch, on_irq, NULL);
    ata_set_atapi_bus(&s_ch, s_bus);
    ata_checkpoint_restore(&s_ch, NULL);
    pio_in(b + 256, 1024 - 256);
    ASSERT_TRUE(memcmp(b, s_hd + 30 * 512, sizeof b) == 0);
    teardown();
}

int main(void) {
    RUN(each_device_reads_back_its_own_signature);
    RUN(identify_device_describes_the_disk);
    RUN(pio_read_streams_the_sectors_and_interrupts_per_block);
    RUN(pio_write_lands_on_the_medium);
    RUN(read_multiple_needs_set_multiple_first);
    RUN(dma_read_moves_through_the_port_then_interrupts);
    RUN(dma_write_lands_on_the_medium);
    RUN(a_sector_past_the_end_is_an_id_not_found);
    RUN(nien_holds_the_line_low);
    RUN(software_reset_restores_the_signatures);
    RUN(an_empty_or_disabled_cell_floats);
    RUN(a_cd_drive_on_the_back_end_appears_on_the_channel);
    RUN(atapi_device_aborts_identify_device_and_answers_identify_packet);
    RUN(atapi_inquiry_comes_back_in_one_chunk);
    RUN(atapi_read_splits_at_the_byte_count_limit);
    RUN(atapi_read_by_dma);
    RUN(atapi_check_condition_reports_the_sense_key);
    RUN(a_checkpoint_mid_read_resumes_the_stream);
    free(s_cp);
    printf("All ata tests passed\n");
    return 0;
}
