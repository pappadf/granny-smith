// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_fdc.c
// Apple Lisa intelligent floppy controller. See lisa_fdc.h and docs/machines/lisa/lisa.md §13.
//
// Behavioural model: the 6504A coprocessor is represented by its 1 KB shared
// RAM plus a synchronous command engine.  When the 68000 writes the command-
// issue register (byte 0), the engine reads the command block, services it with
// disk_read_data / disk_write_data against the image_t, fills the data buffer,
// sets the status byte, and raises FDIR.  No GCR cell modelling — the
// controller returns logical 512-byte sectors (docs/machines/lisa/lisa.md §13).

#include "lisa_fdc.h"

#include "cpu.h" // cpu_get_pc — guest PC for the floppy command trace
#include "floppy_internal.h" // iwm_sectors_per_track / iwm_disk_image_offset (Sony geometry)
#include "image.h"
#include "log.h"
#include "system.h" // system_cpu — current CPU for the floppy command trace

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("floppy");

#define FDC_RAM_BYTES 1024 // controller RAM addressable by the 68000 (odd bytes)

// Command-block byte indices within the shared RAM (docs/machines/lisa/lisa.md §13.2; offsets
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

// Command-issue values (docs/machines/lisa/lisa.md §13.1).
#define CMD_EXEC     0x81 // execute the RWTS command
#define CMD_SEEK     0x83 // seek
#define CMD_JSR      0x84 // JSR to a host-downloaded routine in controller RAM ($00C003)
#define CMD_CLRSTAT  0x85 // clear interrupt status
#define CMD_SETMASK  0x86 // set interrupt mask
#define CMD_CLRMASK  0x87 // clear interrupt mask
#define CMD_COLDWAIT 0x88 // wait in ROM for cold start
#define CMD_LOOP     0x89 // loop in ROM

// RWTS sub-commands (docs/machines/lisa/lisa.md §13.2): 0/7 read, 1 write, 2 unclamp, …
#define RWTS_READ     0x00
#define RWTS_WRITE    0x01
#define RWTS_UNCLAMP  0x02
#define RWTS_READNOCK 0x07

// Drive-status / interrupt-event bits ($00C05F).  These are LATCHED events, not
// live state: the controller raises FDIR and sets the matching bit, the host's
// interrupt/drain handler reads them, and CLRSTAT ($85) clears them.  (The disk
// stays physically attached via fdc->image regardless — reads don't depend on
// these bits.)  MacWorks' startup drains events until ($C05F & $77) == 0, so a
// persistently-set "present" bit here would loop forever (docs/machines/lisa/lisa.md §13.3).
#define DRVSTAT_DISKIN1   0x01 // drive 1 (lower) disk-inserted event
#define DRVSTAT_COMPLETE1 0x04 // drive 1 (lower) RWTS complete
#define DRVSTAT_OR1       0x08 // OR of bits 0-2 (lower-drive summary)
#define DRVSTAT_DISKIN2   0x10 // drive 2 (upper / Lisa-2 internal Sony) disk-inserted
#define DRVSTAT_COMPLETE2 0x40 // drive 2 (upper) RWTS complete
#define DRVSTAT_OR2       0x80 // OR of bits 4-6 (upper-drive summary)

struct lisa_fdc {
    struct scheduler *sched;
    lisa_fdc_fdir_fn fdir_cb;
    void *fdir_ctx;
    bool fdir; // current FDIR (VIA1 PB4) state

    image_t *image; // attached floppy (NULL if none)
    int num_sides; // 1 (400 KB) or 2 (800 KB)

    // Faithful disk reinsert: a physical micro-diskette retains the writes made
    // to it across an eject + reinsert.  The OS overmount protocol relies on this
    // (it writes an overmount_stamp to the boot diskette when it temporarily
    // unmounts it, then verifies that stamp when the user reinserts it to finish
    // the install — LisaOS SOURCE-FSINIT2 rbd / boot_remount).  Our host reinsert
    // opens a fresh image (a new delta) each time, which would lose the write.  So
    // we cache every inserted image by base path and re-use it (with its delta)
    // when the same disk is reinserted.  See docs/notes/lisa_disk_insertion_los.md §5.
    image_t *disk_cache[8];
    int n_cache;

    uint8_t ram[FDC_RAM_BYTES];
};

// Guest PC for the floppy command/access trace (LOG level 1, category
// "floppy").  Only evaluated when the trace is enabled (the LOG macro guards
// its arguments), so this costs nothing in normal operation.  Marked unused so
// builds whose LOG macro drops its variadic arguments (the unit-test harness)
// don't warn — it is genuinely referenced wherever LOG expands its arguments.
__attribute__((unused)) static uint32_t fdc_guest_pc(void) {
    cpu_t *cpu = system_cpu();
    return cpu ? cpu_get_pc(cpu) : 0;
}

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
}

// Report the Sony disk geometry the boot loader's block->(track,sector)
// converter reads (controller byte 10).
static void fdc_update_disktype(lisa_fdc_t *fdc) {
    fdc->ram[FDC_DISKTYPE] = fdc->image ? (fdc->num_sides == 1 ? DISKTYPE_SONY_400K : DISKTYPE_SONY_800K) : 0;
    // "disk in drive": LisaOS's ISDISKIN polls for NON-ZERO, while Xenix's boot
    // loader compares the byte against $FF ("CMPI #$FF") to decide a drive is
    // loaded — so report the full-byte present flag $FF, which satisfies both.
    fdc->ram[FDC_DISKIN] = fdc->image ? 0xFF : 0x00;
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
        // Empty drive: the Sony 6504A controller reports DRVERR ($07 = "no disk
        // in drive"), which the boot ROM maps to its "insert a diskette" prompt.
        // NOT $16: $16 is decimal 22 = CLMPERR (clamp error), which the boot ROM
        // treats as an I/O-board fault and paints as a crossed I/O-board icon.
        fdc->ram[FDC_STATUS] = 0x07; // DRVERR: no disk in drive
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
        // The Sony controller writes the data sector AND its tag (pagelabel)
        // together; persist the 12-byte tag the OS staged in DSKBUFF so later
        // reads see the FS's updated pagelabels.
        disk_write_tag(fdc->image, (size_t)offset / 512, &fdc->ram[FDC_HDR], 12);
        fdc->ram[FDC_STATUS] = (put == 512) ? 0x00 : 0x18; // unwritable on short write
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
    // The 6504A reports a fresh completion code (DISKERR, byte 8) for each
    // command it runs.  Only a media-access command (EXEC/RWTS) can fail with a
    // disk error; every control command — CLRSTAT, ENBLDRV, SEEK, COLDWAIT, … —
    // completes with status OK.  Start each command at OK so a prior read's error
    // never leaks forward: the boot ROM probes an empty drive with EXEC (leaving
    // $07 "no disk"), then the OS Sony driver runs INITDISK (CLRSTAT+ENBLDRV) and
    // reads DISKERR to confirm the drive came up.  Without this reset it saw the
    // stale $07, concluded init failed, and looped INITDISK / raised the spurious
    // "disk not in standard format" alert even though it never read any media.
    fdc->ram[FDC_STATUS] = 0;
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
    // Floppy command trace (enable: `debug.log "floppy" 1`).  One line per
    // command issued, with the resulting status, whether media is present, and
    // the guest PC — so a ROM-vs-OS floppy access can be told apart on a real
    // boot.  $81=EXEC(RWTS) $83=SEEK $84=JSR $85=CLRSTAT $86=ENBLDRV $87=COLDWAIT.
    LOG(1, "fdc cmd=$%02x rwts=$%02x trk=%d sec=%d drv=$%02x img=%d status=$%02x pc=$%06x", cmd, fdc->ram[FDC_RWTS],
        fdc->ram[FDC_TRACK], fdc->ram[FDC_SECTOR], fdc->ram[FDC_DRIVE], fdc->image != NULL, fdc->ram[FDC_STATUS],
        fdc_guest_pc());
}

// === Shared-RAM access ======================================================

// The 6504's 1 KB buffer RAM is shared with the 68000 on the ODD bus bytes of
// $00FCC000-$00FCC7FF (even bytes float — the 6504 is an 8-bit part on the odd
// half of the 16-bit bus).  The iface is mapped from base $00FCC000, so the
// offset parity equals the physical parity: an ODD offset is a RAM byte
// (off>>1); an EVEN offset floats.  Multi-byte (word/long) accesses compose
// big-endian from the constituent bytes — the LisaOS Sony driver pokes the
// command block one odd byte at a time, but Xenix's boot loader reads/writes
// the block with word/long ops at the even base (MOVE.L $00FCC000 to issue a
// command and poll byte 0), so the controller must honour both.
static uint8_t fdc_read_byte(lisa_fdc_t *fdc, uint32_t off) {
    if (off & 1) {
        uint32_t idx = off >> 1;
        uint8_t v = idx < FDC_RAM_BYTES ? fdc->ram[idx] : 0xFF;
        // Trace the disk-presence reads (enable: `debug.log "floppy" 1`): the OS
        // Sony driver polls DISKIN ($41) for "media present" and reads DRVSTAT
        // ($5F) as the interrupt source.  Seeing whether/when these are read,
        // and their value, tells us how the OS decides a disk is in the drive.
        if (idx == FDC_DISKIN || idx == FDC_DRVSTAT)
            LOG(1, "fdc read %s val=$%02x img=%d pc=$%06x", idx == FDC_DISKIN ? "DISKIN" : "DRVSTAT", v,
                fdc->image != NULL, fdc_guest_pc());
        return v;
    }
    return 0xFF; // even byte: no shared RAM on this half of the bus
}

uint8_t lisa_fdc_read8(lisa_fdc_t *fdc, uint32_t offset) {
    return fdc_read_byte(fdc, offset);
}
uint16_t lisa_fdc_read16(lisa_fdc_t *fdc, uint32_t offset) {
    return (uint16_t)((fdc_read_byte(fdc, offset) << 8) | fdc_read_byte(fdc, offset + 1));
}
uint32_t lisa_fdc_read32(lisa_fdc_t *fdc, uint32_t offset) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        v = (v << 8) | fdc_read_byte(fdc, offset + i);
    return v;
}

// Apply a `size`-byte big-endian write to the shared RAM (odd bytes only), then
// issue a command if byte 0 (the command-issue register, $00FCC001) was set
// non-zero.  The command is issued only AFTER the whole block is in place, so a
// single long write carrying both the command and its parameters services the
// parameters first.
static void fdc_write_bytes(lisa_fdc_t *fdc, uint32_t off, uint32_t value, unsigned size) {
    bool cmd_written = false;
    uint8_t cmd_val = 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t a = off + i;
        uint8_t b = (uint8_t)(value >> (8 * (size - 1 - i)));
        if (!(a & 1))
            continue; // even byte: 6504 RAM is on the odd bus bytes only
        uint32_t idx = a >> 1;
        if (idx >= FDC_RAM_BYTES)
            continue;
        fdc->ram[idx] = b;
        if (idx == FDC_CMDREG) {
            cmd_written = true;
            cmd_val = b;
        }
    }
    if (cmd_written && cmd_val != 0)
        fdc_command(fdc, cmd_val);
}

void lisa_fdc_write8(lisa_fdc_t *fdc, uint32_t offset, uint8_t value) {
    fdc_write_bytes(fdc, offset, value, 1);
}
void lisa_fdc_write16(lisa_fdc_t *fdc, uint32_t offset, uint16_t value) {
    fdc_write_bytes(fdc, offset, value, 2);
}
void lisa_fdc_write32(lisa_fdc_t *fdc, uint32_t offset, uint32_t value) {
    fdc_write_bytes(fdc, offset, value, 4);
}

// === Media ==================================================================

void lisa_fdc_insert(lisa_fdc_t *fdc, image_t *image) {
    // Faithful reinsert: if this disk was inserted before, re-use the retained
    // image (it carries any writes — e.g. the boot diskette's overmount_stamp);
    // the freshly-opened `image` is redundant.  Otherwise retain it for future
    // reinserts.  Match by CANONICAL path so the same physical image is
    // recognised whether its path was given absolute or relative.
    if (image) {
        const char *fn = image_get_filename(image);
        char fnreal[PATH_MAX];
        const char *fnc = (fn && realpath(fn, fnreal)) ? fnreal : fn;
        image_t *cached = NULL;
        for (int i = 0; i < fdc->n_cache; i++) {
            const char *cfn = image_get_filename(fdc->disk_cache[i]);
            char creal[PATH_MAX];
            const char *cfnc = (cfn && realpath(cfn, creal)) ? creal : cfn;
            if (fnc && cfnc && strcmp(fnc, cfnc) == 0) {
                cached = fdc->disk_cache[i];
                break;
            }
        }
        if (cached) {
            // Re-point to the retained image so its writes (the boot
            // diskette's overmount_stamp) survive the eject+reinsert.  Do
            // NOT close the redundant fresh `image`: images are owned by the
            // system's tracked list (config->images[]) and freed exactly once
            // at shutdown.  The boot disk's absolute fd= path and this
            // relative reinsert path are tracked as two separate entries, so
            // closing it here would leave a dangling entry that shutdown
            // double-frees (segfault on quit).
            image = cached;
        } else if (fdc->n_cache < (int)(sizeof(fdc->disk_cache) / sizeof(fdc->disk_cache[0]))) {
            fdc->disk_cache[fdc->n_cache++] = image;
        }
    }
    fdc->image = image;
    fdc->num_sides = (image && disk_size(image) > 500000) ? 2 : 1; // 400 KB = 1 side
    fdc_update_disktype(fdc);
    if (image) {
        // Disk insertion raises FDIR (IPL 1) so the OS Sony driver's DISK_INT runs,
        // reads $C05F, sees the disk-inserted event, and sets disk_present:=gooddisk
        // (LisaOS SOURCE-SONY DISK_INT; see docs/notes/lisa_disk_insertion_los.md §2.2).
        // The Lisa-2 internal Sony is the UPPER drive ($80): its disk-in event is
        // DISKIN2 (bit4) + the upper-drive summary (bit7) = $90 (the $C05F per-drive
        // nibble layout — docs/notes/lisa_disk_insertion_los.md §1.1).  (The earlier
        // COMPLETE1|OR1=$0c was the lower-drive nibble, which the OS never matched to
        // the upper drive, so Mount kept returning nodiskpres/614 and the installer
        // silently re-prompted.)
        fdc->ram[FDC_DRVSTAT] |= DRVSTAT_DISKIN2 | DRVSTAT_OR2;
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

// === Parameter memory (battery-backed NVRAM) ================================
//
// The Lisa's parameter memory (boot volume + device-configuration table + UI
// settings, 64 bytes = 32 words) lives at $FCC181 in the controller's shared
// RAM and is battery/standby-backed on real hardware (docs/machines/lisa/lisa.md §13.4).  Our
// fdc->ram is volatile, so the OS's installed configuration (e.g. a ProFile
// added to the device table at clean shutdown) is lost across launches.  These
// save/load the 64-byte PM region to a host file, modelling the battery backup
// so an installed system can be booted from a persisted image.
#define FDC_PM_IDX 192 // $FCC181 → ram index (offset $180 >> 1)
#define FDC_PM_LEN 64 // 32 words

bool lisa_fdc_pram_save(const lisa_fdc_t *fdc, const char *path) {
    if (!fdc || !path || !*path)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t put = fwrite(&fdc->ram[FDC_PM_IDX], 1, FDC_PM_LEN, f);
    fclose(f);
    return put == FDC_PM_LEN;
}

bool lisa_fdc_pram_load(lisa_fdc_t *fdc, const char *path) {
    if (!fdc || !path || !*path)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t got = fread(&fdc->ram[FDC_PM_IDX], 1, FDC_PM_LEN, f);
    fclose(f);
    return got == FDC_PM_LEN;
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
    // Power-up parameter-memory default.  The COPS clock/PM region is battery-backed
    // on real hardware; with no persisted PRAM the boot ROM still needs a boot-device
    // selection.  Seed BootVol=1 (PM byte 4 high nibble = built-in Sony floppy;
    // docs/machines/lisa/pram_format.md §4) plus the checksum word so the ROM auto-boots the
    // built-in floppy.  ProFile-boot tests override this with a full PRAM image
    // (profile.pram_load); see lisa-profile-boot.
    fdc->ram[196] = 0x10; // PM byte 4: BootVol=1 (built-in Sony), NormCont=0
    fdc->ram[254] = 0xFE; // PM bytes 62-63: PRAM validity checksum word
    fdc->ram[255] = 0x00;
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
