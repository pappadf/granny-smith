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

#include <stdio.h>
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
#define FDC_DISKIN                                                                                                     \
    32 // $00FCC041: NON-ZERO = disk in drive.  LisaOS's Sony driver
       // (SOURCE-SONYASM `ISDISKIN`/`DISKIN .EQU $41`) polls this byte at
       // drive init; a zero here makes FS_Mount abort with nodiskpres (614).
#define FDC_DRVSTAT 47 // drive status byte ($00C05F: present/eject/complete)

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

    uint8_t ram[FDC_RAM_BYTES];
};

// Set FDIR (VIA1 PB4) and notify the machine.
static void fdc_set_fdir(lisa_fdc_t *fdc, bool asserted) {
    fdc->fdir = asserted;
    if (fdc->fdir_cb)
        fdc->fdir_cb(fdc->fdir_ctx, asserted);
}

// Deferred RWTS completion.  Real floppy reads take milliseconds; the OS Sony
// driver (SOURCE-SONYASM) issues an interrupt-generating command and *blocks*
// (WAIT_INT) waiting for the completion interrupt.  Signalling completion
// synchronously (in the same instruction as the command-register write) raced
// that block: the IPL-1 interrupt fired before the driver had blocked, so the
// wakeup was lost and the reader process hung forever (the 4th-boot hang).  We
// therefore latch the data immediately but raise COMPLETE + FDIR on a scheduler
// event a sector-read time later.  (The boot ROM polls PB4, so it is unaffected
// by the added latency.)
#define FDC_COMPLETE_CYCLES 24000 // ~4.7 ms — a Sony 400K sector-read latency

static void fdc_complete(void *source, uint64_t data) {
    (void)data;
    lisa_fdc_t *fdc = (lisa_fdc_t *)source;
    // The Sony controller's interrupt-source byte ($C05F = DISKSTAT) reports WHICH
    // drive completed: the controller ORs in (drive_id & 0x88) >> 1, then sets the
    // summary bit 7 if any drive-done bit (0x70) is present (the real Lisa FDC
    // interrupt-status encoding).  The Lisa drive
    // id is $80 (lower) ⇒ bit 6 + bit 7 = $C0; the OS Sony driver's DISK_INT
    // reads bit 6 as int_stat.bot_done (MSB-first packing) and only unblocks the
    // waiting reader process when it is set — a fixed bits-2/3 value left bot_done
    // clear, so the interrupt-driven OS reader hung after its first read.  The
    // Mac ROM (MacWorks) drains ($C05F & $77), which includes bit 6, so it is
    // unaffected.
    uint8_t ist = (uint8_t)((fdc->ram[FDC_DRIVE] & 0x88) >> 1); // $80→bit6, $08→bit2
    if (ist & 0x70)
        ist |= 0x80; // summary/OR bit
    if (ist == 0)
        ist = DRVSTAT_COMPLETE1 | DRVSTAT_OR1; // drive 0: fall back to legacy bits 2/3
    fdc->ram[FDC_DRVSTAT] |= ist; // RWTS complete (drive-encoded interrupt source)
    fdc_set_fdir(fdc, true); // raise FDIR / IPL1 now that the driver has blocked
    if (getenv("GSTRACE")) {
        extern uint64_t cpu_instr_count(void);
        extern int g_lisa_trace;
        uint64_t ic = cpu_instr_count();
        fprintf(stderr, "GSCOMPLETE i=%llu\n", (unsigned long long)ic);
        if (getenv("GSTRACE_AT") == 0 && ic > 19886100 && !g_lisa_trace) {
            g_lisa_trace = 1;
            fprintf(stderr, "GSTRACE ON i=%llu\n", (unsigned long long)ic);
        }
    }
}

// Report the Sony disk geometry the boot loader's block->(track,sector)
// converter reads (controller byte 10).
static void fdc_update_disktype(lisa_fdc_t *fdc) {
    fdc->ram[FDC_DISKTYPE] = fdc->image ? (fdc->num_sides == 1 ? DISKTYPE_SONY_400K : DISKTYPE_SONY_800K) : 0;
    fdc->ram[FDC_DISKIN] = fdc->image ? 0x01 : 0x00; // "disk in drive" the Sony driver's ISDISKIN polls
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
        if (getenv("GSTRACE")) {
            extern uint64_t cpu_instr_count(void);
            fprintf(stderr, "GSRWTS read lba=%zu trk=%d sec=%d drive=%02x i=%llu\n", (size_t)offset / 512, track,
                    sector, fdc->ram[FDC_DRIVE], (unsigned long long)cpu_instr_count());
        }
    } else if (rwts == RWTS_WRITE) {
        size_t put = disk_write_data(fdc->image, offset, &fdc->ram[FDC_DATA], 512);
        // The Sony controller writes the data sector AND its tag (pagelabel)
        // together; persist the 12-byte tag the OS staged in DSKBUFF so later
        // reads see the FS's updated pagelabels.
        disk_write_tag(fdc->image, (size_t)offset / 512, &fdc->ram[FDC_HDR], 12);
        fdc->ram[FDC_STATUS] = (put == 512) ? 0x00 : 0x18; // unwritable on short write
        if (getenv("GSTRACE")) {
            extern uint64_t cpu_instr_count(void);
            fprintf(stderr, "GSRWTS WRITE lba=%zu trk=%d sec=%d drive=%02x i=%llu\n", (size_t)offset / 512, track,
                    sector, fdc->ram[FDC_DRIVE], (unsigned long long)cpu_instr_count());
        }
    } else if (rwts == RWTS_UNCLAMP) {
        // Unclamp releases/ejects the disk (the Lisa drive is software-eject).
        // The drive goes empty until the host inserts new media; that is how the
        // Mac ROM's "no system, blink ?-disk" loop drains a non-system disk so a
        // system disk can be inserted in its place.
        fdc->image = NULL;
        fdc_update_disktype(fdc);
        fdc->ram[FDC_DRVSTAT] = 0;
        fdc->ram[FDC_STATUS] = 0x00;
        LOG(2, "fdc unclamp -> eject (drive now empty)");
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
        fdc_execute_rwts(fdc); // latch the sector data into the buffer now
        fdc->ram[FDC_CMDREG] = 0; // command taken
        // Signal COMPLETE + FDIR on a scheduler event, not synchronously, so
        // the OS driver's WAIT_INT block is in place before the interrupt fires.
        remove_event(fdc->sched, &fdc_complete, fdc); // coalesce any prior pending
        scheduler_new_cpu_event(fdc->sched, &fdc_complete, fdc, 0, FDC_COMPLETE_CYCLES, 0);
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
    if (image) {
        // Disk insertion raises FDIR (IPL 1) so the host re-examines the drive
        // after a swap (docs/lisa.md §13).  We surface it as a completion event
        // (the same status the host's drive-event handler drains after any
        // operation); a raw disk-in bit cannot be acknowledged by that handler.
        fdc->ram[FDC_DRVSTAT] |= DRVSTAT_COMPLETE1 | DRVSTAT_OR1;
        fdc_set_fdir(fdc, true);
    }
}
void lisa_fdc_eject(lisa_fdc_t *fdc) {
    fdc->image = NULL;
    fdc_update_disktype(fdc);
    fdc->ram[FDC_DRVSTAT] = 0; // no media → no pending drive events
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
    if (scheduler)
        scheduler_new_event_type(scheduler, "lisa_fdc", fdc, "complete", &fdc_complete);
    fdc->fdir_cb = fdir_cb;
    fdc->fdir_ctx = fdir_ctx;
    fdc->num_sides = 1;
    {
        fdc->ram[196] = 0x10;
        fdc->ram[254] = 0xFE;
        fdc->ram[255] = 0x00;
    } // GSDIAG auto-boot
    if (cp)
        lisa_fdc_checkpoint(fdc, cp);
    return fdc;
}

// Set the disk-controller ROM id byte at $FCC031 (= ram[24]).  The boot ROM's
// SETTYPE reads it to detect the machine: bit7 clear ⇒ Lisa 1 (twin Twiggy
// drives), bit7 set ⇒ Lisa 2 with bit6 (FASTMR)=fast timers / bit5 (SLOTMR)=
// slow timers.  A calloc-zeroed byte mis-identifies us as a Lisa 1 (two-floppy
// STARTUP menu); the machine sets the value matching the board it models.
void lisa_fdc_set_diskrom(lisa_fdc_t *fdc, uint8_t id) {
    if (fdc)
        fdc->ram[24] = id;
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
