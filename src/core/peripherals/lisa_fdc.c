// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_fdc.c
// Apple Lisa intelligent floppy controller. See lisa_fdc.h and docs/lisa.md §13.
//
// Behavioural model: the 6504A coprocessor is represented by its 1 KB shared
// RAM plus a synchronous command engine.  When the 68000 writes the command-
// issue register (byte 0), the engine reads the command block, services it with
// disk_read_data / disk_write_data against the image_t, fills the data buffer,
// sets the status byte, and raises FDIR.  No GCR cell modelling — the
// controller returns logical 512-byte sectors (docs/lisa.md §13).

#include "lisa_fdc.h"

#include "floppy_internal.h" // iwm_sectors_per_track / iwm_disk_image_offset (Sony geometry)
#include "image.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("floppy");

#define FDC_RAM_BYTES 1024 // controller RAM addressable by the 68000 (odd bytes)

// Command-block byte indices within the shared RAM (docs/lisa.md §13.2; offsets
// in the source are address-space, halved here to RAM byte indices).
#define FDC_CMDREG   0 // command-issue register
#define FDC_RWTS     1 // RWTS sub-command (CMD)
#define FDC_DRIVE    2 // drive ($00 = drive 2, $80 = drive 1)
#define FDC_SIDE     3 // side
#define FDC_SECTOR   4 // sector
#define FDC_TRACK    5 // track
#define FDC_SPEED    6 // motor speed
#define FDC_CONFIRM  7 // format confirm
#define FDC_STATUS   8 // error status (0 = OK)
#define FDC_DISKTYPE 10 // disk geometry/type the controller reports for the media in the drive
#define FDC_DRVSTAT  47 // drive status byte ($00C05F: present/eject/complete)

// FDC_DISKTYPE encoding the boot loader's block->(track,sector) converter reads
// ($21028 in MacWorks PREBOOT: reads byte 10, then picks the disk's total block
// count from it).  0 = Twiggy/FileWare (1702 blocks, the original Lisa default);
// non-zero with bit0=1 = Sony 400 KB single-sided (800 blocks); bit0=0 = Sony
// 800 KB double-sided (1600 blocks).  Without this the converter defaults to the
// 1702-block Twiggy geometry and computes out-of-range (track, sector) pairs.
#define DISKTYPE_SONY_400K 0x01 // odd  -> bit0=1 -> 800 blocks
#define DISKTYPE_SONY_800K 0x02 // even -> bit0=0 -> 1600 blocks
#define FDC_HDR            500 // 12-byte sector tag/header (DSKBUFF)
#define FDC_DATA           512 // 512-byte data sector (DSKDATA)

// Command-issue values (docs/lisa.md §13.1).
#define CMD_EXEC     0x81 // execute the RWTS command
#define CMD_SEEK     0x83 // seek
#define CMD_JSR      0x84 // JSR to a host-downloaded routine in controller RAM ($00C003)
#define CMD_CLRSTAT  0x85 // clear interrupt status
#define CMD_SETMASK  0x86 // set interrupt mask
#define CMD_CLRMASK  0x87 // clear interrupt mask
#define CMD_COLDWAIT 0x88 // wait in ROM for cold start
#define CMD_LOOP     0x89 // loop in ROM

// RWTS sub-commands (docs/lisa.md §13.2): 0/7 read, 1 write, 2 unclamp, …
#define RWTS_READ     0x00
#define RWTS_WRITE    0x01
#define RWTS_UNCLAMP  0x02
#define RWTS_READNOCK 0x07

// Drive-status / interrupt-event bits ($00C05F).  These are LATCHED events, not
// live state: the controller raises FDIR and sets the matching bit, the host's
// interrupt/drain handler reads them, and CLRSTAT ($85) clears them.  (The disk
// stays physically attached via fdc->image regardless — reads don't depend on
// these bits.)  MacWorks' startup drains events until ($C05F & $77) == 0, so a
// persistently-set "present" bit here would loop forever (docs/lisa.md §13.3).
#define DRVSTAT_DISKIN1   0x01 // drive 1 disk-inserted event
#define DRVSTAT_COMPLETE1 0x04 // drive 1 RWTS complete
#define DRVSTAT_OR1       0x08 // OR of bits 0-2

struct lisa_fdc {
    struct scheduler *sched;
    lisa_fdc_fdir_fn fdir_cb;
    void *fdir_ctx;
    bool fdir; // current FDIR (VIA1 PB4) state

    image_t *image; // attached floppy (NULL if none)
    int num_sides; // 1 (400 KB) or 2 (800 KB)
    image_t *next_disk; // queued disk auto-fed on the next eject (NULL = none)

    uint8_t ram[FDC_RAM_BYTES];
};

// Set FDIR (VIA1 PB4) and notify the machine.
static void fdc_set_fdir(lisa_fdc_t *fdc, bool asserted) {
    fdc->fdir = asserted;
    if (fdc->fdir_cb)
        fdc->fdir_cb(fdc->fdir_ctx, asserted);
}

// Report the Sony disk geometry the boot loader's block->(track,sector)
// converter reads (controller byte 10).
static void fdc_update_disktype(lisa_fdc_t *fdc) {
    fdc->ram[FDC_DISKTYPE] = fdc->image ? (fdc->num_sides == 1 ? DISKTYPE_SONY_400K : DISKTYPE_SONY_800K) : 0;
}

// Map (track, side, sector) to a byte offset in the image (Sony 5-zone layout).
static size_t fdc_block_offset(const lisa_fdc_t *fdc, int track, int side, int sector) {
    return iwm_disk_image_offset(track, side, fdc->num_sides) + (size_t)sector * 512u;
}

// Execute the RWTS command currently in the command block.
static void fdc_execute_rwts(lisa_fdc_t *fdc) {
    uint8_t rwts = fdc->ram[FDC_RWTS];
    int side = fdc->ram[FDC_SIDE] & 1;
    int sector = fdc->ram[FDC_SECTOR];
    int track = fdc->ram[FDC_TRACK];

    if (!fdc->image) {
        fdc->ram[FDC_STATUS] = 0x16; // no disk / not ready
        return;
    }
    if (track < 0 || track > 79 || sector < 0 || sector >= iwm_sectors_per_track(track)) {
        fdc->ram[FDC_STATUS] = 0x17; // unreadable
        LOG(2, "fdc unreadable: rwts=%02x trk=%d sec=%d side=%d (out of Sony geometry)", rwts, track, sector, side);
        return;
    }

    size_t offset = fdc_block_offset(fdc, track, side, sector);

    if (rwts == RWTS_READ || rwts == RWTS_READNOCK) {
        // Serve the sector's DiskCopy tag as the 12-byte header the boot ROM
        // validates (FILEID at offset 4 must be $AAAA for the boot block).
        memset(&fdc->ram[FDC_HDR], 0, 12);
        disk_read_tag(fdc->image, (size_t)offset / 512, &fdc->ram[FDC_HDR], 12);
        size_t got = disk_read_data(fdc->image, offset, &fdc->ram[FDC_DATA], 512);
        fdc->ram[FDC_STATUS] = (got == 512) ? 0x00 : 0x17;
        LOG(2, "fdc read trk=%d side=%d sec=%d off=%zu -> status=%02x", track, side, sector, offset,
            fdc->ram[FDC_STATUS]);
    } else if (rwts == RWTS_WRITE) {
        size_t put = disk_write_data(fdc->image, offset, &fdc->ram[FDC_DATA], 512);
        fdc->ram[FDC_STATUS] = (put == 512) ? 0x00 : 0x18; // unwritable on short write
    } else if (rwts == RWTS_UNCLAMP) {
        // Unclamp releases/ejects the disk (the Lisa drive is software-eject).
        // The Mac ROM's "no system, blink ?-disk" loop ejects the bad disk to
        // get a system disk; if one is queued, auto-feed it (the 2-disk boot),
        // otherwise the drive goes empty.
        fdc->ram[FDC_STATUS] = 0x00;
        if (fdc->next_disk) {
            image_t *next = fdc->next_disk;
            fdc->next_disk = NULL;
            lisa_fdc_insert(fdc, next); // sets image/num_sides/disktype + latches disk-in
            LOG(2, "fdc unclamp -> auto-fed queued disk");
        } else {
            fdc->image = NULL;
            fdc_update_disktype(fdc);
            fdc->ram[FDC_DRVSTAT] = 0;
            LOG(2, "fdc unclamp -> eject (drive now empty)");
        }
    } else {
        // format / verify: acknowledge with OK for now
        fdc->ram[FDC_STATUS] = 0x00;
        LOG(2, "fdc rwts=%02x (format/verify) trk=%d sec=%d", rwts, track, sector);
    }
}

// Service a command-issue register write (byte 0).
static void fdc_command(lisa_fdc_t *fdc, uint8_t cmd) {
    switch (cmd) {
    case CMD_EXEC:
        fdc_execute_rwts(fdc);
        fdc->ram[FDC_DRVSTAT] |= DRVSTAT_COMPLETE1 | DRVSTAT_OR1; // RWTS complete
        fdc->ram[FDC_CMDREG] = 0; // command taken
        fdc_set_fdir(fdc, true); // signal completion
        break;
    case CMD_SEEK:
        fdc->ram[FDC_STATUS] = 0;
        fdc->ram[FDC_CMDREG] = 0;
        fdc_set_fdir(fdc, true);
        break;
    case CMD_JSR:
        // $84: the coprocessor JSRs to a routine the host downloaded into the
        // shared RAM at $00C003.  We can't run 6504A code, so we emulate its
        // observable effect: MacWorks uses byte 6 as a busy flag (sets it $FF,
        // issues $84, polls it to 0) and reads the status byte afterwards.
        fdc->ram[6] = 0; // clear the busy/completion flag the host polls
        fdc->ram[FDC_STATUS] = 0; // report OK
        fdc->ram[FDC_CMDREG] = 0;
        LOG(2, "fdc $84 JSR-to-controller-RAM (acknowledged)");
        break;
    case CMD_CLRSTAT:
        fdc->ram[FDC_DRVSTAT] = 0; // CLRSTAT acknowledges all latched drive events
        fdc->ram[FDC_CMDREG] = 0;
        fdc_set_fdir(fdc, false); // clear interrupt
        break;
    case CMD_SETMASK:
    case CMD_CLRMASK:
    case CMD_LOOP:
        fdc->ram[FDC_CMDREG] = 0; // accepted, no interrupt
        break;
    case CMD_COLDWAIT:
        fdc->ram[FDC_CMDREG] = 0; // controller is "warm": cold-start satisfied
        fdc_set_fdir(fdc, true);
        break;
    default:
        fdc->ram[FDC_CMDREG] = 0; // accept unknown command issues
        break;
    }
}

// === Shared-RAM access ======================================================

uint8_t lisa_fdc_read8(lisa_fdc_t *fdc, uint32_t offset) {
    uint32_t idx = offset >> 1; // controller RAM lives on the odd bus bytes
    return idx < FDC_RAM_BYTES ? fdc->ram[idx] : 0xFF;
}
uint16_t lisa_fdc_read16(lisa_fdc_t *fdc, uint32_t offset) {
    return lisa_fdc_read8(fdc, offset);
}
uint32_t lisa_fdc_read32(lisa_fdc_t *fdc, uint32_t offset) {
    return lisa_fdc_read8(fdc, offset);
}

void lisa_fdc_write8(lisa_fdc_t *fdc, uint32_t offset, uint8_t value) {
    uint32_t idx = offset >> 1;
    if (idx >= FDC_RAM_BYTES)
        return;
    fdc->ram[idx] = value;
    if (idx == FDC_CMDREG && value != 0)
        fdc_command(fdc, value); // writing byte 0 issues a command
}
void lisa_fdc_write16(lisa_fdc_t *fdc, uint32_t offset, uint16_t value) {
    lisa_fdc_write8(fdc, offset, (uint8_t)value);
}
void lisa_fdc_write32(lisa_fdc_t *fdc, uint32_t offset, uint32_t value) {
    lisa_fdc_write8(fdc, offset, (uint8_t)value);
}

// === Media ==================================================================

void lisa_fdc_insert(lisa_fdc_t *fdc, image_t *image) {
    fdc->image = image;
    fdc->num_sides = (image && disk_size(image) > 500000) ? 2 : 1; // 400 KB = 1 side
    fdc_update_disktype(fdc);
    // Note: we deliberately do NOT latch a disk-in status bit here.  The host's
    // drive-event drain (Mac ROM) only knows how to clear RWTS-complete events;
    // a lingering disk-in bit it can't acknowledge would wedge that loop.  The
    // disk is readable via fdc->image regardless of the status byte.
}
void lisa_fdc_eject(lisa_fdc_t *fdc) {
    fdc->image = NULL;
    fdc_update_disktype(fdc);
    fdc->ram[FDC_DRVSTAT] = 0; // no media → no pending drive events
}
void lisa_fdc_queue_disk(lisa_fdc_t *fdc, image_t *image) {
    fdc->next_disk = image;
}
bool lisa_fdc_disk_present(const lisa_fdc_t *fdc) {
    return fdc && fdc->image != NULL;
}

// === Lifecycle =============================================================

lisa_fdc_t *lisa_fdc_init(struct scheduler *scheduler, lisa_fdc_fdir_fn fdir_cb, void *fdir_ctx, checkpoint_t *cp) {
    lisa_fdc_t *fdc = (lisa_fdc_t *)calloc(1, sizeof(*fdc));
    if (!fdc)
        return NULL;
    fdc->sched = scheduler;
    fdc->fdir_cb = fdir_cb;
    fdc->fdir_ctx = fdir_ctx;
    fdc->num_sides = 1;
    if (cp)
        lisa_fdc_checkpoint(fdc, cp);
    return fdc;
}

void lisa_fdc_delete(lisa_fdc_t *fdc) {
    free(fdc);
}

void lisa_fdc_checkpoint(lisa_fdc_t *fdc, checkpoint_t *cp) {
    // Symmetric no-op for now (same discipline as the MMU/COPS): the shared RAM
    // is re-derived by the next command sequence.  Full save/restore: Step 9.
    (void)fdc;
    (void)cp;
}
