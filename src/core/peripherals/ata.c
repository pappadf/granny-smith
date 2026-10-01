// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ata.c
// An ATA channel with up to two devices: an ATA hard disk over the image
// layer, or an ATAPI CD-ROM whose PACKET commands run on the SCSI CD-ROM
// model.  See ata.h for the boundary.
//
// Sources: ATA/ATAPI-4 (T13 1153D rev 18) for the task file, the protocols
// (PIO data-in §9.7, PIO data-out §9.8, DMA §9.10, PACKET §9.11, the
// signatures §9.1) and the commands; SFF-8020i for the ATAPI interrupt
// reason and byte-count registers.
//
// Every command completes at once: the device never shows BSY to the host,
// and a data block is ready (DRQ, INTRQ) by the time the command register
// write returns.  INTRQ is a level, held until the host reads Status or
// writes a new command, so a host that enables the interrupt late still
// sees it.

#include "ata.h"

#include "image.h"
#include "log.h"
#include "scsi.h"
#include "system.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ata");

#define SECTOR 512u

// scsi_device_type() of a CD-ROM (the media_slot_t numbering: 1 hd, 2 cdrom).
#define SCSI_TYPE_CDROM 2

// The ATAPI interrupt reason, read through Sector Count (SFF-8020i §5.12):
// CoD (bit 0) and IO (bit 1).
#define IR_COD 0x01u
#define IR_IO  0x02u

// Largest PACKET response held at once (a READ of 4 MB).
#define ATAPI_RESP_MAX (4u * 1024u * 1024u)

static int sel_unit(const ata_channel_t *ch) {
    return (ch->select >> 4) & 1;
}

static ata_dev_t *sel_dev(ata_channel_t *ch) {
    return &ch->dev[sel_unit(ch)];
}

static bool any_device(const ata_channel_t *ch) {
    return ch->dev[0].kind != ATA_DEV_NONE || ch->dev[1].kind != ATA_DEV_NONE;
}

bool ata_intrq(const ata_channel_t *ch) {
    const ata_dev_t *d = &ch->dev[sel_unit(ch)];
    return ch->enabled && d->kind != ATA_DEV_NONE && d->intrq && !(ch->devctl & ATA_DC_NIEN);
}

static void update_line(ata_channel_t *ch) {
    bool level = ata_intrq(ch);
    if (level == (ch->line != 0))
        return;
    ch->line = level ? 1 : 0;
    if (ch->irq)
        ch->irq(ch->irq_ctx, level);
}

static void post_intrq(ata_channel_t *ch, ata_dev_t *d) {
    d->intrq = 1;
    update_line(ch);
}

// The signature a device leaves in its task file after any reset (§9.1).
static void set_signature(ata_dev_t *d) {
    d->error = 0x01; // diagnostic passed
    d->nsect = 0x01;
    d->sector = 0x01;
    d->intrq = 0;
    if (d->kind == ATA_DEV_ATAPI) {
        d->lcyl = 0x14;
        d->hcyl = 0xEB;
        d->status = 0;
    } else {
        d->lcyl = 0;
        d->hcyl = 0;
        d->status = d->kind == ATA_DEV_HD ? (ATA_ST_DRDY | ATA_ST_DSC) : 0;
    }
}

static void free_resp(ata_channel_t *ch) {
    free(ch->atapi_resp);
    ch->atapi_resp = NULL;
    ch->atapi_resp_len = ch->atapi_resp_pos = 0;
}

static void end_transfer(ata_channel_t *ch) {
    ch->xfer = ATA_XFER_NONE;
    ch->buf_len = ch->buf_pos = 0;
    ch->sectors_left = 0;
    free_resp(ch);
}

void ata_hard_reset(ata_channel_t *ch) {
    end_transfer(ch);
    ch->select = 0;
    ch->feature = 0;
    for (int u = 0; u < 2; u++) {
        ch->dev[u].mult = 0;
        set_signature(&ch->dev[u]);
    }
    update_line(ch);
}

void ata_channel_init(ata_channel_t *ch, int index) {
    memset(ch, 0, sizeof *ch);
    ch->index = index;
    ch->buf = (uint8_t *)calloc(1, ATA_BUF_SIZE);
    ch->dev[0].scsi_id = ch->dev[1].scsi_id = -1;
    ata_hard_reset(ch);
}

void ata_channel_free(ata_channel_t *ch) {
    free(ch->buf);
    ch->buf = NULL;
    free_resp(ch);
}

void ata_set_irq(ata_channel_t *ch, ata_irq_fn fn, void *ctx) {
    ch->irq = fn;
    ch->irq_ctx = ctx;
}

void ata_set_dma_kick(ata_channel_t *ch, ata_dma_kick_fn fn, void *ctx) {
    ch->dma_kick = fn;
    ch->dma_ctx = ctx;
}

void ata_set_atapi_bus(ata_channel_t *ch, struct scsi *bus) {
    ch->atapi_bus = bus;
}

void ata_set_enabled(ata_channel_t *ch, bool enabled) {
    ch->enabled = enabled ? 1 : 0;
    update_line(ch);
}

ata_dev_kind_t ata_device_kind(const ata_channel_t *ch, int unit) {
    return (unit == 0 || unit == 1) ? (ata_dev_kind_t)ch->dev[unit].kind : ATA_DEV_NONE;
}

// The default translation for a disk of `sectors` (ATA-4 Annex: 16 heads,
// 63 sectors per track, cylinders capped at 16,383).
static void default_geometry(ata_dev_t *d) {
    d->heads = 16;
    d->spt = 63;
    uint32_t cyls = d->sectors / (16u * 63u);
    d->cyls = (uint16_t)(cyls > 16383u ? 16383u : cyls);
}

bool ata_attach_hd(ata_channel_t *ch, int unit, struct image *img) {
    if ((unit != 0 && unit != 1) || !img)
        return false;
    ata_dev_t *d = &ch->dev[unit];
    memset(d, 0, sizeof *d);
    d->kind = ATA_DEV_HD;
    d->scsi_id = -1;
    d->sectors = (uint32_t)(disk_size(img) / SECTOR);
    default_geometry(d);
    strcpy(d->model, "GS ATA HARD DISK");
    snprintf(d->serial, sizeof d->serial, "GS%04d%d", ch->index, unit);
    ch->img[unit] = img;
    set_signature(d);
    return true;
}

bool ata_attach_atapi(ata_channel_t *ch, int unit, int scsi_id) {
    if (unit != 0 && unit != 1)
        return false;
    ata_dev_t *d = &ch->dev[unit];
    memset(d, 0, sizeof *d);
    d->kind = ATA_DEV_ATAPI;
    d->scsi_id = scsi_id;
    strcpy(d->model, "GS ATAPI CD-ROM");
    snprintf(d->serial, sizeof d->serial, "GSCD%02d%d", ch->index, unit);
    ch->img[unit] = NULL;
    set_signature(d);
    return true;
}

void ata_detach(ata_channel_t *ch, int unit) {
    if (unit != 0 && unit != 1)
        return;
    memset(&ch->dev[unit], 0, sizeof ch->dev[unit]);
    ch->dev[unit].scsi_id = -1;
    ch->img[unit] = NULL;
    update_line(ch);
}

// ---- completion helpers ----------------------------------------------------

static void finish_ok(ata_channel_t *ch, ata_dev_t *d) {
    end_transfer(ch);
    d->status = ATA_ST_DRDY | ATA_ST_DSC;
    d->error = 0;
    post_intrq(ch, d);
}

static void finish_abort(ata_channel_t *ch, ata_dev_t *d, uint8_t error) {
    end_transfer(ch);
    d->status = ATA_ST_DRDY | ATA_ST_ERR;
    d->error = error;
    post_intrq(ch, d);
}

// ---- IDENTIFY --------------------------------------------------------------

static void put_word(uint8_t *b, int w, uint16_t v) {
    b[2 * w] = (uint8_t)v;
    b[2 * w + 1] = (uint8_t)(v >> 8);
}

// An ATA string: two characters per word, the first in the high byte,
// padded with spaces (§7.16).
static void put_string(uint8_t *b, int w, int words, const char *s) {
    size_t n = strlen(s);
    for (int i = 0; i < 2 * words; i++) {
        char c = (size_t)i < n ? s[i] : ' ';
        b[2 * w + (i ^ 1)] = (uint8_t)c;
    }
}

static void identify_common(uint8_t *b, const ata_dev_t *d) {
    put_string(b, 10, 10, d->serial);
    put_string(b, 23, 4, "1.0");
    put_string(b, 27, 20, d->model);
    put_word(b, 49, 0x0300); // LBA and DMA supported
    put_word(b, 51, 0x0200); // PIO timing mode 2
    put_word(b, 52, 0x0200);
    put_word(b, 53, 0x0006); // words 64-70 and 88 valid
    // Multiword DMA modes 0-2 supported; the mode SET FEATURES chose.
    uint16_t mw = 0x0007;
    if ((d->xfer_mode & 0xF8u) == 0x20u)
        mw |= (uint16_t)(0x0100u << (d->xfer_mode & 7u));
    put_word(b, 63, mw);
    put_word(b, 64, 0x0003); // PIO modes 3 and 4
    put_word(b, 65, 120);
    put_word(b, 66, 120);
    put_word(b, 67, 120);
    put_word(b, 68, 120);
    put_word(b, 80, 0x001E); // ATA-1 through ATA-4
}

static void identify_hd(uint8_t *b, const ata_dev_t *d) {
    memset(b, 0, SECTOR);
    put_word(b, 0, 0x0040); // fixed disk
    uint32_t cyls = d->sectors / (16u * 63u);
    put_word(b, 1, (uint16_t)(cyls > 16383u ? 16383u : cyls));
    put_word(b, 3, 16);
    put_word(b, 6, 63);
    identify_common(b, d);
    put_word(b, 47, 0x8010); // READ/WRITE MULTIPLE up to 16 sectors
    put_word(b, 53, 0x0007); // and words 54-58 valid
    put_word(b, 54, d->cyls);
    put_word(b, 55, d->heads);
    put_word(b, 56, d->spt);
    uint32_t chs = (uint32_t)d->cyls * d->heads * d->spt;
    put_word(b, 57, (uint16_t)chs);
    put_word(b, 58, (uint16_t)(chs >> 16));
    put_word(b, 59, d->mult ? (uint16_t)(0x0100u | d->mult) : 0);
    put_word(b, 60, (uint16_t)d->sectors);
    put_word(b, 61, (uint16_t)(d->sectors >> 16));
    // Integrity word (§8.12.255): signature $A5 and a checksum making the
    // 512 bytes sum to zero.
    b[510] = 0xA5;
    uint8_t sum = 0;
    for (int i = 0; i < 511; i++)
        sum = (uint8_t)(sum + b[i]);
    b[511] = (uint8_t)-sum;
}

static void identify_atapi(uint8_t *b, const ata_dev_t *d) {
    memset(b, 0, SECTOR);
    // ATAPI, CD-ROM (type 5), removable, accelerated DRQ, 12-byte packets.
    put_word(b, 0, 0x85C0);
    identify_common(b, d);
}

// ---- HD data path ----------------------------------------------------------

// The LBA the task file of `d` addresses, or false when it addresses no
// sector (a CHS sector number of 0, or no translation).
static bool tf_lba(const ata_channel_t *ch, const ata_dev_t *d, uint64_t *out) {
    if (ch->select & 0x40u) {
        *out = ((uint64_t)(ch->select & 0xFu) << 24) | ((uint64_t)d->hcyl << 16) | ((uint64_t)d->lcyl << 8) | d->sector;
        return true;
    }
    uint32_t cyl = (uint32_t)d->lcyl | ((uint32_t)d->hcyl << 8);
    uint32_t head = ch->select & 0xFu;
    if (d->sector == 0 || d->heads == 0 || d->spt == 0 || head >= d->heads || d->sector > d->spt)
        return false;
    *out = ((uint64_t)cyl * d->heads + head) * d->spt + (d->sector - 1u);
    return true;
}

// Leave the task file pointing at the last sector moved (§8: an LBA
// command's registers hold the address on completion).
static void tf_store_lba(ata_channel_t *ch, ata_dev_t *d, uint64_t lba) {
    if (!(ch->select & 0x40u))
        return;
    d->sector = (uint8_t)lba;
    d->lcyl = (uint8_t)(lba >> 8);
    d->hcyl = (uint8_t)(lba >> 16);
    ch->select = (uint8_t)((ch->select & 0xF0u) | ((lba >> 24) & 0xFu));
}

static bool hd_io(ata_channel_t *ch, int unit, bool write, uint64_t lba, uint32_t n) {
    image_t *img = ch->img[unit];
    if (!img)
        return false;
    size_t bytes = (size_t)n * SECTOR;
    size_t got = write ? disk_write_data(img, (size_t)lba * SECTOR, ch->buf, bytes)
                       : disk_read_data(img, (size_t)lba * SECTOR, ch->buf, bytes);
    return got == bytes;
}

// Stage the next block of a read into the buffer; false on a media error.
static bool stage_read(ata_channel_t *ch, uint32_t max_sectors) {
    uint32_t n = ch->sectors_left < max_sectors ? ch->sectors_left : max_sectors;
    if (!hd_io(ch, ch->cur, false, ch->lba, n))
        return false;
    ch->lba += n;
    ch->sectors_left -= n;
    ch->buf_len = n * SECTOR;
    ch->buf_pos = 0;
    return true;
}

static void pio_in_block(ata_channel_t *ch, ata_dev_t *d) {
    if (!stage_read(ch, ch->block_sectors)) {
        finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
        return;
    }
    ch->xfer = ATA_XFER_PIO_IN;
    d->status = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
    post_intrq(ch, d);
}

static void pio_out_block(ata_channel_t *ch, ata_dev_t *d, bool interrupt) {
    uint32_t n = ch->sectors_left < ch->block_sectors ? ch->sectors_left : ch->block_sectors;
    ch->buf_len = n * SECTOR;
    ch->buf_pos = 0;
    ch->xfer = ATA_XFER_PIO_OUT;
    d->status = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
    if (interrupt)
        post_intrq(ch, d);
}

// Start a read or write: check the range, then the first block.
static void start_rw(ata_channel_t *ch, ata_dev_t *d, bool write, bool dma, uint32_t block) {
    uint64_t lba;
    uint32_t count = d->nsect ? d->nsect : 256u;
    if (!tf_lba(ch, d, &lba) || lba + count > d->sectors) {
        finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
        return;
    }
    ch->lba = lba;
    ch->sectors_left = count;
    ch->block_sectors = block;
    if (dma) {
        ch->buf_len = ch->buf_pos = 0;
        ch->xfer = write ? ATA_XFER_DMA_OUT : ATA_XFER_DMA_IN;
        if (write) {
            uint32_t n = count < ATA_BUF_SIZE / SECTOR ? count : ATA_BUF_SIZE / SECTOR;
            ch->buf_len = n * SECTOR;
        } else if (!stage_read(ch, ATA_BUF_SIZE / SECTOR)) {
            finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
            return;
        }
        d->status = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
        if (ch->dma_kick)
            ch->dma_kick(ch->dma_ctx);
        return;
    }
    if (write)
        pio_out_block(ch, d, false); // the first block needs no interrupt
    else
        pio_in_block(ch, d);
}

// ---- ATAPI -------------------------------------------------------------------

static void atapi_complete(ata_channel_t *ch, ata_dev_t *d, int scsi_status) {
    uint8_t sense = 0;
    if (scsi_status == 2 && ch->atapi_bus) // CHECK CONDITION
        sense = scsi_device_sense_key(ch->atapi_bus, d->scsi_id);
    LOG(4, "ch%d: PACKET done: status $%02X sense %u, %u bytes", ch->index, scsi_status, sense, ch->atapi_resp_len);
    end_transfer(ch);
    d->nsect = IR_IO | IR_COD;
    if (scsi_status == 0) {
        d->status = ATA_ST_DRDY | ATA_ST_DSC;
        d->error = 0;
    } else {
        d->status = ATA_ST_DRDY | ATA_ST_ERR;
        d->error = (uint8_t)((sense & 0xFu) << 4) | (sense ? 0 : ATA_ER_ABRT);
    }
    post_intrq(ch, d);
}

// Run the SCSI status / message-in tail and release the bus.
static int atapi_tail(scsi_t *bus) {
    if (scsi_get_bus_phase(bus) == scsi_data_in)
        scsi_external_data_in_complete(bus);
    int st = scsi_external_status_byte(bus);
    (void)scsi_external_message_byte(bus);
    scsi_external_release(bus);
    return st < 0 ? 2 : (st & 0x3E);
}

// Hand the host the next PIO chunk of the response (SFF-8020i: byte count
// in the cylinder registers, IO set, CoD clear).
static void atapi_pio_chunk(ata_channel_t *ch, ata_dev_t *d) {
    uint32_t left = ch->atapi_resp_len - ch->atapi_resp_pos;
    uint32_t limit = ch->atapi_limit ? ch->atapi_limit : 0xFFFEu;
    uint32_t n = left < limit ? left : limit;
    if (n < left)
        n &= ~1u; // a chunk that is not the last is even
    ch->buf_pos = ch->atapi_resp_pos;
    ch->buf_len = ch->atapi_resp_pos + n;
    ch->xfer = ATA_XFER_PIO_IN;
    d->lcyl = (uint8_t)n;
    d->hcyl = (uint8_t)(n >> 8);
    d->nsect = IR_IO;
    d->status = ATA_ST_DRDY | ATA_ST_DRQ;
    post_intrq(ch, d);
}

static void atapi_exec(ata_channel_t *ch, ata_dev_t *d) {
    scsi_t *bus = ch->atapi_bus;
    if (!bus || d->scsi_id < 0 || !scsi_external_select(bus, d->scsi_id)) {
        LOG(2, "ch%d: PACKET $%02X: no device on the back end", ch->index, ch->buf[0]);
        atapi_complete(ch, d, 2);
        return;
    }
    LOG(4, "ch%d: PACKET $%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", ch->index, ch->buf[0], ch->buf[1],
        ch->buf[2], ch->buf[3], ch->buf[4], ch->buf[5], ch->buf[6], ch->buf[7], ch->buf[8], ch->buf[9]);
    for (int i = 0; i < 12 && scsi_get_bus_phase(bus) == scsi_command; i++)
        scsi_push_data_out_byte(bus, ch->buf[i]);

    int phase = scsi_get_bus_phase(bus);
    if (phase == scsi_data_out) {
        // The host supplies the data: one chunk of the byte-count limit.
        uint32_t n = ch->atapi_limit ? ch->atapi_limit : 0xFFFEu;
        if (n > ATA_BUF_SIZE)
            n = ATA_BUF_SIZE;
        ch->atapi_out_len = n;
        ch->buf_len = n;
        ch->buf_pos = 0;
        d->lcyl = (uint8_t)n;
        d->hcyl = (uint8_t)(n >> 8);
        d->nsect = 0;
        d->status = ATA_ST_DRDY | ATA_ST_DRQ;
        if (ch->packet_dma) {
            ch->xfer = ATA_XFER_DMA_OUT;
            if (ch->dma_kick)
                ch->dma_kick(ch->dma_ctx);
        } else {
            ch->xfer = ATA_XFER_PIO_OUT;
            post_intrq(ch, d);
        }
        return;
    }

    uint32_t cap = 0, len = 0;
    uint8_t *resp = NULL;
    if (phase == scsi_data_in) {
        uint8_t b;
        while (len < ATAPI_RESP_MAX && scsi_pop_data_in_byte(bus, &b)) {
            if (len == cap) {
                uint32_t ncap = cap ? cap * 2u : 4096u;
                uint8_t *nr = (uint8_t *)realloc(resp, ncap);
                if (!nr)
                    break;
                resp = nr;
                cap = ncap;
            }
            resp[len++] = b;
        }
    }
    int st = atapi_tail(bus);
    if (st != 0 || len == 0) {
        free(resp);
        atapi_complete(ch, d, st);
        return;
    }
    ch->atapi_resp = resp;
    ch->atapi_resp_len = len;
    ch->atapi_resp_pos = 0;
    if (ch->packet_dma) {
        ch->xfer = ATA_XFER_DMA_IN;
        d->nsect = IR_IO;
        d->status = ATA_ST_DRDY | ATA_ST_DRQ;
        if (ch->dma_kick)
            ch->dma_kick(ch->dma_ctx);
    } else {
        atapi_pio_chunk(ch, d);
    }
}

// The host's DATA OUT bytes are in: hand them to the target and finish.
static void atapi_data_out_done(ata_channel_t *ch, ata_dev_t *d) {
    scsi_t *bus = ch->atapi_bus;
    for (uint32_t i = 0; i < ch->buf_len && bus && scsi_get_bus_phase(bus) == scsi_data_out; i++)
        scsi_push_data_out_byte(bus, ch->buf[i]);
    atapi_complete(ch, d, bus ? atapi_tail(bus) : 2);
}

// ---- commands ----------------------------------------------------------------

static void exec_command(ata_channel_t *ch, uint8_t cmd) {
    int unit = sel_unit(ch);
    ata_dev_t *d = &ch->dev[unit];
    if (d->kind == ATA_DEV_NONE)
        return; // nobody to take it
    end_transfer(ch);
    ch->cmd = cmd;
    ch->cur = (uint8_t)unit;
    d->intrq = 0;
    LOG(3, "ch%d dev%d: command $%02X (nsect %u lba %02X/%02X/%02X/%X feat %02X)", ch->index, unit, cmd, d->nsect,
        d->hcyl, d->lcyl, d->sector, ch->select & 0xF, ch->feature);

    // Commands both kinds of device take.
    switch (cmd) {
    case 0x90: // EXECUTE DEVICE DIAGNOSTIC: both devices, device 0 reports
        for (int u = 0; u < 2; u++)
            set_signature(&ch->dev[u]);
        ch->select &= ~0x10u;
        post_intrq(ch, &ch->dev[0]);
        return;
    case 0xEF: // SET FEATURES
        if (ch->feature == 0x03)
            d->xfer_mode = d->nsect;
        finish_ok(ch, d);
        return;
    case 0xE0: // STANDBY IMMEDIATE
    case 0xE1: // IDLE IMMEDIATE
    case 0xE2: // STANDBY
    case 0xE3: // IDLE
    case 0xE6: // SLEEP
    case 0xE7: // FLUSH CACHE
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x99:
        finish_ok(ch, d);
        return;
    case 0xE5: // CHECK POWER MODE: active
    case 0x98:
        finish_ok(ch, d);
        d->nsect = 0xFF;
        return;
    default:
        break;
    }

    if (d->kind == ATA_DEV_ATAPI) {
        switch (cmd) {
        case 0xA0: // PACKET
            ch->packet_dma = ch->feature & 1u;
            ch->atapi_limit = (uint32_t)d->lcyl | ((uint32_t)d->hcyl << 8);
            ch->xfer = ATA_XFER_PACKET;
            ch->buf_len = 12;
            ch->buf_pos = 0;
            d->nsect = IR_COD;
            d->status = ATA_ST_DRDY | ATA_ST_DRQ; // accelerated DRQ: no interrupt
            return;
        case 0xA1: // IDENTIFY PACKET DEVICE
            identify_atapi(ch->buf, d);
            ch->buf_len = SECTOR;
            ch->buf_pos = 0;
            ch->sectors_left = 0;
            ch->xfer = ATA_XFER_PIO_IN;
            d->status = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
            post_intrq(ch, d);
            return;
        case 0x08: // DEVICE RESET: the signature, no interrupt
            set_signature(d);
            return;
        case 0xEC: // IDENTIFY DEVICE: aborted, with the signature (§8.12)
            set_signature(d);
            d->status = ATA_ST_DRDY | ATA_ST_ERR;
            d->error = ATA_ER_ABRT;
            post_intrq(ch, d);
            return;
        default:
            finish_abort(ch, d, ATA_ER_ABRT);
            return;
        }
    }

    switch (cmd) {
    case 0xEC: // IDENTIFY DEVICE
        identify_hd(ch->buf, d);
        ch->buf_len = SECTOR;
        ch->buf_pos = 0;
        ch->sectors_left = 0;
        ch->xfer = ATA_XFER_PIO_IN;
        d->status = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
        post_intrq(ch, d);
        return;
    case 0x20: // READ SECTORS (with and without retry)
    case 0x21:
        start_rw(ch, d, false, false, 1);
        return;
    case 0x30: // WRITE SECTORS
    case 0x31:
        start_rw(ch, d, true, false, 1);
        return;
    case 0xC4: // READ MULTIPLE
    case 0xC5: // WRITE MULTIPLE
        if (!d->mult) {
            finish_abort(ch, d, ATA_ER_ABRT);
            return;
        }
        start_rw(ch, d, cmd == 0xC5, false, d->mult);
        return;
    case 0xC8: // READ DMA
    case 0xC9:
        start_rw(ch, d, false, true, 0);
        return;
    case 0xCA: // WRITE DMA
    case 0xCB:
        start_rw(ch, d, true, true, 0);
        return;
    case 0x40: // READ VERIFY SECTORS
    case 0x41: {
        uint64_t lba;
        uint32_t count = d->nsect ? d->nsect : 256u;
        if (!tf_lba(ch, d, &lba) || lba + count > d->sectors)
            finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
        else
            finish_ok(ch, d);
        return;
    }
    case 0x70: // SEEK
        finish_ok(ch, d);
        return;
    case 0x91: { // INITIALIZE DEVICE PARAMETERS
        uint16_t heads = (uint16_t)((ch->select & 0xFu) + 1u);
        uint16_t spt = d->nsect;
        if (!spt) {
            finish_abort(ch, d, ATA_ER_ABRT);
            return;
        }
        d->heads = heads;
        d->spt = spt;
        uint32_t cyls = d->sectors / ((uint32_t)heads * spt);
        d->cyls = (uint16_t)(cyls > 65535u ? 65535u : cyls);
        finish_ok(ch, d);
        return;
    }
    case 0xC6: { // SET MULTIPLE MODE: 0 disables; 2, 4, 8 or 16 sectors
        uint8_t n = d->nsect;
        if (n > 16 || (n & (n - 1))) {
            finish_abort(ch, d, ATA_ER_ABRT);
            return;
        }
        d->mult = n;
        finish_ok(ch, d);
        return;
    }
    default:
        if ((cmd & 0xF0u) == 0x10u) { // RECALIBRATE
            finish_ok(ch, d);
            return;
        }
        LOG(2, "ch%d dev%d: command $%02X not supported — aborted", ch->index, unit, cmd);
        finish_abort(ch, d, ATA_ER_ABRT);
        return;
    }
}

// ---- register access ---------------------------------------------------------

// The ATAPI devices are whatever CD-ROMs sit on the back end at SCSI id
// cell * 2 + device: a disc attached there (machine.atapi.attach_cdrom)
// appears on the channel, and one taken away leaves it.  Checked on every
// register access, which costs two lookups and needs no attach hook.
static void sync_atapi(ata_channel_t *ch) {
    if (!ch->atapi_bus)
        return;
    for (int u = 0; u < 2; u++) {
        ata_dev_t *d = &ch->dev[u];
        if (d->kind == ATA_DEV_HD)
            continue;
        int id = ch->index * 2 + u;
        bool present = scsi_device_present(ch->atapi_bus, (unsigned)id) &&
                       scsi_device_type(ch->atapi_bus, (unsigned)id) == SCSI_TYPE_CDROM;
        if (present && d->kind == ATA_DEV_NONE)
            ata_attach_atapi(ch, u, id);
        else if (!present && d->kind == ATA_DEV_ATAPI)
            ata_detach(ch, u);
    }
}

uint8_t ata_read(ata_channel_t *ch, int reg) {
    if (!ch->enabled)
        return 0xFF;
    sync_atapi(ch);
    if (!any_device(ch))
        return 0x7F; // nothing drives the bus; DD7 is pulled down
    ata_dev_t *d = sel_dev(ch);
    if (d->kind == ATA_DEV_NONE)
        return reg == ATA_REG_SELECT ? ch->select : 0; // device 0 answers for an absent 1
    switch (reg) {
    case ATA_REG_DATA:
        return (uint8_t)(ata_read_data16(ch) >> 8);
    case ATA_REG_ERROR:
        return d->error;
    case ATA_REG_NSECT:
        return d->nsect;
    case ATA_REG_SECTOR:
        return d->sector;
    case ATA_REG_LCYL:
        return d->lcyl;
    case ATA_REG_HCYL:
        return d->hcyl;
    case ATA_REG_SELECT:
        return ch->select;
    default: // Status: reading it acknowledges INTRQ
        d->intrq = 0;
        update_line(ch);
        return d->status;
    }
}

uint8_t ata_read_altstatus(ata_channel_t *ch) {
    if (!ch->enabled)
        return 0xFF;
    sync_atapi(ch);
    if (!any_device(ch))
        return 0x7F;
    ata_dev_t *d = sel_dev(ch);
    return d->kind == ATA_DEV_NONE ? 0 : d->status;
}

void ata_write(ata_channel_t *ch, int reg, uint8_t value) {
    if (!ch->enabled)
        return;
    sync_atapi(ch);
    switch (reg) {
    case ATA_REG_DATA:
        ata_write_data16(ch, (uint16_t)(value << 8 | value));
        return;
    case ATA_REG_ERROR:
        ch->feature = value;
        return;
    case ATA_REG_NSECT:
    case ATA_REG_SECTOR:
    case ATA_REG_LCYL:
    case ATA_REG_HCYL:
        // Both devices latch every task-file write.
        for (int u = 0; u < 2; u++) {
            ata_dev_t *d = &ch->dev[u];
            if (reg == ATA_REG_NSECT)
                d->nsect = value;
            else if (reg == ATA_REG_SECTOR)
                d->sector = value;
            else if (reg == ATA_REG_LCYL)
                d->lcyl = value;
            else
                d->hcyl = value;
        }
        return;
    case ATA_REG_SELECT:
        ch->select = value;
        update_line(ch);
        return;
    default:
        exec_command(ch, value);
        return;
    }
}

void ata_write_devctl(ata_channel_t *ch, uint8_t value) {
    if (!ch->enabled)
        return;
    bool was_reset = (ch->devctl & ATA_DC_SRST) != 0;
    ch->devctl = value;
    if (value & ATA_DC_SRST) {
        end_transfer(ch);
        for (int u = 0; u < 2; u++) {
            ch->dev[u].intrq = 0;
            if (ch->dev[u].kind != ATA_DEV_NONE)
                ch->dev[u].status = ATA_ST_BSY;
        }
    } else if (was_reset) {
        // The end of a software reset: both devices post their signatures
        // and device 0 is selected (§9.1).
        for (int u = 0; u < 2; u++) {
            ch->dev[u].mult = 0;
            set_signature(&ch->dev[u]);
        }
        ch->select = 0;
    }
    update_line(ch);
}

// A host-side block is drained or filled: move the protocol on.
static void block_done(ata_channel_t *ch) {
    ata_dev_t *d = &ch->dev[ch->cur];
    if (d->kind == ATA_DEV_ATAPI) {
        if (ch->xfer == ATA_XFER_PACKET) {
            ch->xfer = ATA_XFER_NONE;
            atapi_exec(ch, d);
        } else if (ch->xfer == ATA_XFER_PIO_IN) {
            if (ch->atapi_resp) {
                ch->atapi_resp_pos = ch->buf_len;
                if (ch->atapi_resp_pos < ch->atapi_resp_len)
                    atapi_pio_chunk(ch, d);
                else
                    atapi_complete(ch, d, 0);
            } else { // IDENTIFY PACKET DEVICE
                end_transfer(ch);
                d->status = ATA_ST_DRDY | ATA_ST_DSC;
            }
        } else if (ch->xfer == ATA_XFER_PIO_OUT) {
            atapi_data_out_done(ch, d);
        }
        return;
    }
    if (ch->xfer == ATA_XFER_PIO_IN) {
        if (ch->sectors_left) {
            pio_in_block(ch, d);
        } else {
            tf_store_lba(ch, d, ch->lba ? ch->lba - 1 : 0);
            end_transfer(ch);
            d->status = ATA_ST_DRDY | ATA_ST_DSC; // the last block: no interrupt
        }
    } else if (ch->xfer == ATA_XFER_PIO_OUT) {
        uint32_t n = ch->buf_len / SECTOR;
        if (!hd_io(ch, ch->cur, true, ch->lba, n)) {
            finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
            return;
        }
        ch->lba += n;
        ch->sectors_left -= n;
        if (ch->sectors_left) {
            pio_out_block(ch, d, true);
        } else {
            tf_store_lba(ch, d, ch->lba - 1);
            finish_ok(ch, d);
        }
    }
}

uint16_t ata_read_data16(ata_channel_t *ch) {
    if (!ch->enabled || ch->xfer != ATA_XFER_PIO_IN || ch->cur != sel_unit(ch))
        return 0xFFFF;
    const uint8_t *src = ch->atapi_resp ? ch->atapi_resp : ch->buf;
    uint32_t p = ch->buf_pos;
    uint16_t v = (uint16_t)(src[p] << 8);
    if (p + 1 < ch->buf_len)
        v |= src[p + 1];
    ch->buf_pos = p + 2;
    if (ch->buf_pos >= ch->buf_len)
        block_done(ch);
    return v;
}

void ata_write_data16(ata_channel_t *ch, uint16_t value) {
    if (!ch->enabled || ch->cur != sel_unit(ch))
        return;
    if (ch->xfer != ATA_XFER_PIO_OUT && ch->xfer != ATA_XFER_PACKET)
        return;
    uint32_t p = ch->buf_pos;
    ch->buf[p] = (uint8_t)(value >> 8);
    if (p + 1 < ch->buf_len)
        ch->buf[p + 1] = (uint8_t)value;
    ch->buf_pos = p + 2;
    if (ch->buf_pos >= ch->buf_len)
        block_done(ch);
}

// ---- DMA -------------------------------------------------------------------------

bool ata_dma_pending(const ata_channel_t *ch) {
    return ch->xfer == ATA_XFER_DMA_IN || ch->xfer == ATA_XFER_DMA_OUT;
}

int ata_dma_in(void *ctx, uint8_t *buf, int len) {
    ata_channel_t *ch = (ata_channel_t *)ctx;
    if (ch->xfer != ATA_XFER_DMA_IN || len <= 0)
        return 0;
    ata_dev_t *d = &ch->dev[ch->cur];
    int moved = 0;
    if (ch->atapi_resp) {
        uint32_t left = ch->atapi_resp_len - ch->atapi_resp_pos;
        uint32_t n = left < (uint32_t)len ? left : (uint32_t)len;
        memcpy(buf, ch->atapi_resp + ch->atapi_resp_pos, n);
        ch->atapi_resp_pos += n;
        if (ch->atapi_resp_pos >= ch->atapi_resp_len)
            atapi_complete(ch, d, 0);
        return (int)n;
    }
    while (moved < len && ch->xfer == ATA_XFER_DMA_IN) {
        uint32_t left = ch->buf_len - ch->buf_pos;
        uint32_t n = left < (uint32_t)(len - moved) ? left : (uint32_t)(len - moved);
        memcpy(buf + moved, ch->buf + ch->buf_pos, n);
        ch->buf_pos += n;
        moved += (int)n;
        if (ch->buf_pos < ch->buf_len)
            break;
        if (!ch->sectors_left) {
            tf_store_lba(ch, d, ch->lba - 1);
            finish_ok(ch, d);
        } else if (!stage_read(ch, ATA_BUF_SIZE / SECTOR)) {
            finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
        }
    }
    return moved;
}

int ata_dma_out(void *ctx, const uint8_t *buf, int len) {
    ata_channel_t *ch = (ata_channel_t *)ctx;
    if (ch->xfer != ATA_XFER_DMA_OUT || len <= 0)
        return 0;
    ata_dev_t *d = &ch->dev[ch->cur];
    int moved = 0;
    while (moved < len && ch->xfer == ATA_XFER_DMA_OUT) {
        uint32_t room = ch->buf_len - ch->buf_pos;
        uint32_t n = room < (uint32_t)(len - moved) ? room : (uint32_t)(len - moved);
        memcpy(ch->buf + ch->buf_pos, buf + moved, n);
        ch->buf_pos += n;
        moved += (int)n;
        if (ch->buf_pos < ch->buf_len)
            break;
        if (d->kind == ATA_DEV_ATAPI) {
            atapi_data_out_done(ch, d);
            break;
        }
        uint32_t sectors = ch->buf_len / SECTOR;
        if (!hd_io(ch, ch->cur, true, ch->lba, sectors)) {
            finish_abort(ch, d, ATA_ER_ABRT | ATA_ER_IDNF);
            break;
        }
        ch->lba += sectors;
        ch->sectors_left -= sectors;
        if (!ch->sectors_left) {
            tf_store_lba(ch, d, ch->lba - 1);
            finish_ok(ch, d);
        } else {
            uint32_t next = ch->sectors_left < ATA_BUF_SIZE / SECTOR ? ch->sectors_left : ATA_BUF_SIZE / SECTOR;
            ch->buf_len = next * SECTOR;
            ch->buf_pos = 0;
        }
    }
    return moved;
}

// ---- checkpoint ------------------------------------------------------------------

void ata_checkpoint_save(ata_channel_t *ch, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, ch, offsetof(ata_channel_t, buf));
    system_write_checkpoint_data(cp, ch->buf, ATA_BUF_SIZE);
    if (ch->atapi_resp_len)
        system_write_checkpoint_data(cp, ch->atapi_resp, ch->atapi_resp_len);
    for (int u = 0; u < 2; u++) {
        const char *name = ch->img[u] ? image_get_filename(ch->img[u]) : NULL;
        uint32_t len = (name && *name) ? (uint32_t)strlen(name) + 1u : 0u;
        system_write_checkpoint_data(cp, &len, sizeof len);
        if (len)
            system_write_checkpoint_data(cp, name, len);
    }
}

void ata_checkpoint_restore(ata_channel_t *ch, checkpoint_t *cp) {
    free_resp(ch);
    system_read_checkpoint_data(cp, ch, offsetof(ata_channel_t, buf));
    system_read_checkpoint_data(cp, ch->buf, ATA_BUF_SIZE);
    if (ch->atapi_resp_len) {
        ch->atapi_resp = (uint8_t *)malloc(ch->atapi_resp_len);
        if (ch->atapi_resp) {
            system_read_checkpoint_data(cp, ch->atapi_resp, ch->atapi_resp_len);
        } else {
            uint8_t tmp;
            for (uint32_t i = 0; i < ch->atapi_resp_len; i++)
                system_read_checkpoint_data(cp, &tmp, 1);
            ch->atapi_resp_len = 0;
        }
    }
    for (int u = 0; u < 2; u++) {
        uint32_t len = 0;
        system_read_checkpoint_data(cp, &len, sizeof len);
        ch->img[u] = NULL;
        if (!len)
            continue;
        char *name = (char *)malloc(len);
        if (!name) {
            uint8_t tmp;
            for (uint32_t i = 0; i < len; i++)
                system_read_checkpoint_data(cp, &tmp, 1);
            continue;
        }
        system_read_checkpoint_data(cp, name, len);
        name[len - 1] = '\0';
        ch->img[u] = setup_get_image_by_filename(name);
        if (!ch->img[u])
            LOG(1, "ch%d dev%d: image '%s' is not in the machine's image table — device absent", ch->index, u, name);
        free(name);
    }
    for (int u = 0; u < 2; u++)
        if (ch->dev[u].kind == ATA_DEV_HD && !ch->img[u])
            ch->dev[u].kind = ATA_DEV_NONE;
    // The line is whatever the restored state drives; the owner's restored
    // interrupt controller already holds the matching source level.
    ch->line = ata_intrq(ch) ? 1 : 0;
}
