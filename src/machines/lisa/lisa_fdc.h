// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_fdc.h
// Apple Lisa intelligent floppy controller (6504A coprocessor + 1 KB shared
// RAM).  NOT an Apple IWM: the 68000 issues high-level commands by writing a
// command block into the shared RAM and the coprocessor returns logical
// 512-byte sectors.  Modeled behaviourally (the iop_swim.c pattern), reusing
// disk_read_data / the Sony geometry helpers.  See docs/machines/lisa/lisa.md §13 and
// proposal-machine-lisa-xl.md §4.7.
//
// Shared RAM at physical $00C001 (logical $00FCC001), byte N at $C001 + 2*N
// (the controller RAM sits on the odd bytes of the 68000 bus; the ROM uses
// MOVEP).  Verified against the rev-H boot ROM (RM248.B.TEXT):
//   byte 0  command-issue register ($81 execute, $83 seek, $85 clear-status,
//           $86/$87 int-mask, $88 cold-start, $89 loop); reads 0 once taken
//   byte 1  RWTS command (0/1 read, 2 unclamp/eject, 3 format, …)
//   byte 2  drive ($00 = drive 2, $80 = drive 1)
//   byte 3  side, byte 4 sector, byte 5 track, byte 6 speed, byte 7 confirm
//   byte 8  error status (0 = OK)
//   bytes 500-511  sector tag/header (DSKBUFF)
//   bytes 512-1023 the 512-byte data sector (DSKDATA)
// Completion is signalled by raising FDIR (VIA1 PB4); the ROM polls it.

#ifndef LISA_FDC_H
#define LISA_FDC_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct scheduler;
struct image;
typedef struct image image_t;
typedef struct lisa_fdc lisa_fdc_t;

// Raised/cleared when the controller asserts FDIR (drive interrupt request).
// The machine wires this to VIA1 PB4 (and, if it chooses, IPL 1).
typedef void (*lisa_fdc_fdir_fn)(void *ctx, bool asserted);

// === Lifecycle =============================================================

lisa_fdc_t *lisa_fdc_init(struct scheduler *scheduler, lisa_fdc_fdir_fn fdir_cb, void *fdir_ctx, checkpoint_t *cp);
// Set the disk-controller ROM id ($FCC031) the boot ROM reads to detect the
// machine type (Lisa 1 vs Lisa 2 / fast vs slow timers).  See lisa_fdc.c.
void lisa_fdc_set_diskrom(lisa_fdc_t *fdc, uint8_t id);
void lisa_fdc_delete(lisa_fdc_t *fdc);
void lisa_fdc_checkpoint(lisa_fdc_t *fdc, checkpoint_t *cp);

// === Media ==================================================================

// Attach / detach a 400 KB (or 800 KB) Sony floppy image.  Ownership stays with
// the caller (the machine's image list).
void lisa_fdc_insert(lisa_fdc_t *fdc, image_t *image);
void lisa_fdc_eject(lisa_fdc_t *fdc);
bool lisa_fdc_disk_present(const lisa_fdc_t *fdc);

// Parameter memory (battery-backed NVRAM, 64 bytes at $FCC181): persist/restore
// the OS's boot-volume + device-configuration table across launches.  Returns
// false on I/O error.  Load before booting (the ROM reads it during startup).
bool lisa_fdc_pram_save(const lisa_fdc_t *fdc, const char *path);
bool lisa_fdc_pram_load(lisa_fdc_t *fdc, const char *path);

// === Shared-RAM access (offset = physical address − $00C001) ================
uint8_t lisa_fdc_read8(lisa_fdc_t *fdc, uint32_t offset);
uint16_t lisa_fdc_read16(lisa_fdc_t *fdc, uint32_t offset);
uint32_t lisa_fdc_read32(lisa_fdc_t *fdc, uint32_t offset);
void lisa_fdc_write8(lisa_fdc_t *fdc, uint32_t offset, uint8_t value);
void lisa_fdc_write16(lisa_fdc_t *fdc, uint32_t offset, uint16_t value);
void lisa_fdc_write32(lisa_fdc_t *fdc, uint32_t offset, uint32_t value);

#endif // LISA_FDC_H
