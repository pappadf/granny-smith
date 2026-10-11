// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ata.h
// An ATA (IDE) channel: the task file, up to two devices on it (an ATA hard
// disk over the image layer, or an ATAPI CD-ROM whose PACKET commands are
// the SCSI CD-ROM model's, re-hosted), PIO and DMA data transfer, and the
// INTRQ line.  Family-clean: the I/O controller that carries the channel
// (Heathrow's two cells on the beige G3) decodes the registers, wires INTRQ
// to its interrupt controller and attaches a DMA channel to the data port.
//
// Register truth: ATA/ATAPI-4 (T13 1153D) — the task file, the command set
// a Mac OS 8/9-era host uses, the device-0/1 protocol, the ATAPI signature
// and PACKET protocol; SFF-8020i for ATAPI.  The Heathrow wiring (16-byte
// register stride, the timing latch at +$200) is the I/O controller's.

#ifndef GS_CORE_PERIPHERALS_ATA_H
#define GS_CORE_PERIPHERALS_ATA_H

#include <stdbool.h>
#include <stdint.h>

struct image;
struct scsi;
struct checkpoint;
struct image_list;

// Task-file register indices (the register's offset / 16 on Heathrow).
enum {
    ATA_REG_DATA = 0,
    ATA_REG_ERROR = 1, // read: error; write: features
    ATA_REG_NSECT = 2,
    ATA_REG_SECTOR = 3,
    ATA_REG_LCYL = 4,
    ATA_REG_HCYL = 5,
    ATA_REG_SELECT = 6,
    ATA_REG_STATUS = 7, // read: status; write: command
};

// Status register bits (ATA-4).
#define ATA_ST_BSY  0x80u
#define ATA_ST_DRDY 0x40u
#define ATA_ST_DF   0x20u
#define ATA_ST_DSC  0x10u
#define ATA_ST_DRQ  0x08u
#define ATA_ST_ERR  0x01u

// Error register bits.
#define ATA_ER_ABRT 0x04u
#define ATA_ER_IDNF 0x10u
#define ATA_ER_UNC  0x40u // uncorrectable data error

// Device control register bits.
#define ATA_DC_NIEN 0x02u
#define ATA_DC_SRST 0x04u

typedef enum ata_dev_kind {
    ATA_DEV_NONE = 0,
    ATA_DEV_HD, // ATA hard disk (image layer, 512-byte sectors)
    ATA_DEV_ATAPI, // ATAPI CD-ROM (a device on the channel's SCSI back end)
} ata_dev_kind_t;

typedef void (*ata_irq_fn)(void *ctx, bool level);
typedef void (*ata_dma_kick_fn)(void *ctx);

// One device on the channel.  Each device holds its own copy of the task
// file: a host write reaches both, but status, error and the signature
// registers read back from the selected device alone.
typedef struct ata_dev {
    uint8_t kind; // ata_dev_kind_t
    uint8_t mult; // READ/WRITE MULTIPLE block size in sectors (0 = disabled)
    uint8_t xfer_mode; // last SET FEATURES transfer mode
    uint8_t status, error, nsect, sector, lcyl, hcyl;
    uint8_t intrq; // the device's INTRQ (before nIEN)
    uint8_t pad[2];
    uint16_t cyls, heads, spt; // current CHS translation
    uint32_t sectors; // capacity (HD)
    int32_t scsi_id; // ATAPI: target id on the channel's SCSI back end
    char model[41];
    char serial[21];
    char pad2[2];
} ata_dev_t;

// Transfer the channel is in the middle of.
enum {
    ATA_XFER_NONE = 0,
    ATA_XFER_PIO_IN, // device -> host through the data register
    ATA_XFER_PIO_OUT, // host -> device through the data register
    ATA_XFER_DMA_IN,
    ATA_XFER_DMA_OUT,
    ATA_XFER_PACKET, // ATAPI: collecting the 12-byte command packet
};

#define ATA_BUF_SIZE (128u * 1024u) // one transfer block (up to 256 sectors)

typedef struct ata_channel {
    // ---- plain data, checkpointed as one block ----------------------------
    uint8_t feature, select, devctl;
    uint8_t xfer; // ATA_XFER_*
    uint8_t cmd; // the command in progress
    uint8_t cur; // the device the command in progress belongs to
    uint8_t enabled; // the I/O controller has the cell enabled
    uint8_t packet_dma; // the pending PACKET command moves its data by DMA
    uint8_t line; // INTRQ as last driven out
    uint8_t pad[3];
    uint32_t timing; // the I/O controller's timing latch (store and read back)
    uint64_t lba; // next sector of a multi-sector transfer
    uint32_t sectors_left; // sectors of the command not yet staged
    uint32_t block_sectors; // sectors per DRQ block (1, or the MULTIPLE size)
    uint32_t buf_len, buf_pos; // the staged block and the host's cursor in it
    uint32_t atapi_limit; // ATAPI: the host's byte-count limit
    uint32_t atapi_resp_len, atapi_resp_pos; // ATAPI response and its cursor
    uint32_t atapi_out_len; // ATAPI: DATA OUT bytes the command wants
    ata_dev_t dev[2];

    // ---- runtime (re-bound after a restore) --------------------------------
    uint8_t *buf; // ATA_BUF_SIZE staging buffer
    uint8_t *atapi_resp; // the whole response of the current PACKET command
    struct image *img[2]; // HD media
    struct scsi *atapi_bus; // the ATAPI devices' back end (shared, may be NULL)
    ata_irq_fn irq;
    void *irq_ctx;
    ata_dma_kick_fn dma_kick;
    void *dma_ctx;
    int index; // channel number, for logs
} ata_channel_t;

// Build / destroy a channel (registers at their power-on values).
void ata_channel_init(ata_channel_t *ch, int index);
void ata_channel_free(ata_channel_t *ch);

void ata_set_irq(ata_channel_t *ch, ata_irq_fn fn, void *ctx);
void ata_set_dma_kick(ata_channel_t *ch, ata_dma_kick_fn fn, void *ctx);
// The ATAPI back end: a CD-ROM at SCSI id index * 2 + device on `bus` is
// that device of this channel, picked up on the next register access.
void ata_set_atapi_bus(ata_channel_t *ch, struct scsi *bus);

// Devices.  An HD takes an open image (the caller owns its lifetime and its
// place in the machine's image table); an ATAPI device is the CD-ROM at
// `scsi_id` on the channel's SCSI back end.
bool ata_attach_hd(ata_channel_t *ch, int unit, struct image *img);
bool ata_attach_atapi(ata_channel_t *ch, int unit, int scsi_id);
void ata_detach(ata_channel_t *ch, int unit);
ata_dev_kind_t ata_device_kind(const ata_channel_t *ch, int unit);
// Pick up (or drop) the ATAPI devices on the back end now; register
// accesses do this on their own.
void ata_refresh_devices(ata_channel_t *ch);

// The cell's enable and RESET- line (driven by the I/O controller's feature
// bits).  A reset returns both devices to their power-on signatures.
void ata_set_enabled(ata_channel_t *ch, bool enabled);
void ata_hard_reset(ata_channel_t *ch);

// Register access.  `reg` is ATA_REG_*; the data register moves 16 bits with
// the first byte of the stream in bits 15:8 (the order a big-endian host's
// halfword load presents).  The control block is the alternate status /
// device control register.
uint8_t ata_read(ata_channel_t *ch, int reg);
void ata_write(ata_channel_t *ch, int reg, uint8_t value);
uint16_t ata_read_data16(ata_channel_t *ch);
void ata_write_data16(ata_channel_t *ch, uint16_t value);
uint8_t ata_read_altstatus(ata_channel_t *ch);
void ata_write_devctl(ata_channel_t *ch, uint8_t value);
// The same reads without their side effects (an inspection): no INTRQ
// acknowledge, no PIO data advance, no ATAPI attach/detach refresh.
uint8_t ata_peek(ata_channel_t *ch, int reg);
uint16_t ata_peek_data16(ata_channel_t *ch);
uint32_t ata_peek_data32(ata_channel_t *ch); // two data16 reads in a row
uint8_t ata_peek_altstatus(ata_channel_t *ch);

// The INTRQ line as driven (INTRQ of the selected device, gated by nIEN).
bool ata_intrq(const ata_channel_t *ch);

// The DMA data port (dbdma_port_t shape): bytes move only while a DMA
// command is in its data phase; a short return stalls the channel.
int ata_dma_in(void *ctx, uint8_t *buf, int len);
int ata_dma_out(void *ctx, const uint8_t *buf, int len);
bool ata_dma_pending(const ata_channel_t *ch);

// Checkpoint: the register block, the staged data and the device table; HD
// media are re-bound by filename through the machine's image table.
void ata_checkpoint_save(ata_channel_t *ch, struct checkpoint *cp);
void ata_checkpoint_restore(ata_channel_t *ch, struct checkpoint *cp, const struct image_list *images);

#endif // GS_CORE_PERIPHERALS_ATA_H
