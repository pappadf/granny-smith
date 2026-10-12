// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit test for the Lisa intelligent floppy controller's command/status
// contract (lisa_fdc.c).
//
// The 6504A reports a fresh completion code (DISKERR, shared-RAM byte 8) for
// each command.  Only a media-access command (EXEC/RWTS) can fail with a disk
// error; every control command — CLRSTAT, ENBLDRV, SEEK, … — completes OK.
//
// Regression for the spurious LOS "micro diskette is not in the standard Lisa
// Office System format" alert seen when booting from the ProFile with no floppy
// inserted: the boot ROM probes the empty built-in drive (EXEC -> DRVERR $07),
// then the OS Sony driver runs INITDISK (CLRSTAT + ENBLDRV) and reads the status
// byte to confirm the drive initialised.  When the stale $07 leaked through the
// control commands, the driver concluded init had failed and raised the alert —
// although it never read any media.  These tests pin the per-command status
// reset so the leak cannot return.  Deterministic; no emulator/ROM/MMU/scheduler.

#include "lisa_fdc.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Shared-RAM byte N is accessed at offset 2*N+1 (the controller RAM sits on the
// odd bytes of the 68000 bus; see lisa_fdc.c).  A non-zero write to byte 0 (the
// command-issue register) issues a command.
#define OFF(n)   (2u * (n) + 1u)
#define B_CMD    0
#define B_RWTS   1
#define B_DRIVE  2
#define B_STATUS 8

#define CMD_EXEC    0x81
#define CMD_CLRSTAT 0x85
#define CMD_ENBLDRV 0x86 // set interrupt mask
#define CMD_DISDRV  0x87 // clear interrupt mask
#define B_IMSK      44 // $C059: interrupt mask
#define B_IST       47 // $C05F: latched interrupt events
#define RWTS_READ   0x00
#define DRIVE_UPPER 0x80 // built-in (upper) drive
#define DRVERR      0x07 // "no disk in drive"

static bool fdir; // the FDIR line as last driven by the controller

static void fdir_cb(void *ctx, bool asserted) {
    (void)ctx;
    fdir = asserted;
}

static lisa_fdc_t *make_fdc(void) {
    // The scheduler is only used to defer EXEC's completion interrupt; the
    // isolated harness stubs scheduler_* as no-ops, so a sentinel pointer is
    // enough and the deferred event never has to fire for these checks.
    lisa_fdc_t *fdc = lisa_fdc_init((struct scheduler *)0x1, fdir_cb, NULL, NULL, NULL);
    ASSERT_TRUE(fdc != NULL);
    return fdc;
}

static void issue(lisa_fdc_t *fdc, uint8_t cmd) {
    lisa_fdc_write8(fdc, OFF(B_CMD), cmd);
}
static uint8_t status(lisa_fdc_t *fdc) {
    return lisa_fdc_read8(fdc, OFF(B_STATUS));
}

// The ROM's empty-drive boot probe: an EXEC read with no media latches DRVERR.
static void probe_empty_drive(lisa_fdc_t *fdc) {
    lisa_fdc_write8(fdc, OFF(B_RWTS), RWTS_READ);
    lisa_fdc_write8(fdc, OFF(B_DRIVE), DRIVE_UPPER);
    issue(fdc, CMD_EXEC);
}

// Sanity: reading sector 0 of an empty drive reports DRVERR (the boot ROM maps
// this to its "insert a diskette" prompt — must not regress).
TEST(test_empty_drive_read_reports_no_disk) {
    lisa_fdc_t *fdc = make_fdc();
    probe_empty_drive(fdc);
    ASSERT_EQ_INT(status(fdc), DRVERR);
    lisa_fdc_delete(fdc);
}

// CLRSTAT after the empty-drive probe must report OK, not the stale DRVERR.
TEST(test_clrstat_clears_stale_read_error) {
    lisa_fdc_t *fdc = make_fdc();
    probe_empty_drive(fdc);
    ASSERT_EQ_INT(status(fdc), DRVERR); // precondition: error latched
    issue(fdc, CMD_CLRSTAT);
    ASSERT_EQ_INT(status(fdc), 0x00);
    lisa_fdc_delete(fdc);
}

// ENBLDRV after the empty-drive probe must report OK, not the stale DRVERR.
TEST(test_enbldrv_clears_stale_read_error) {
    lisa_fdc_t *fdc = make_fdc();
    probe_empty_drive(fdc);
    ASSERT_EQ_INT(status(fdc), DRVERR);
    issue(fdc, CMD_ENBLDRV);
    ASSERT_EQ_INT(status(fdc), 0x00);
    lisa_fdc_delete(fdc);
}

// The exact sequence from the boot trace: ROM empty-drive probe (DRVERR) →
// OS INITDISK (CLRSTAT then ENBLDRV).  The status the driver reads after
// INITDISK must be OK so it proceeds to poll DISKIN (which then correctly
// reports "no media") instead of looping / raising the format alert.
TEST(test_initdisk_after_probe_reports_ok) {
    lisa_fdc_t *fdc = make_fdc();
    probe_empty_drive(fdc);
    ASSERT_EQ_INT(status(fdc), DRVERR);
    issue(fdc, CMD_CLRSTAT);
    issue(fdc, CMD_ENBLDRV);
    ASSERT_EQ_INT(status(fdc), 0x00);
    lisa_fdc_delete(fdc);
}

// ---- interrupt mask: FDIR = (IST AND IMsk) != 0 ---------------------------

// Issue one of the mask commands with its mask byte.
static void mask_cmd(lisa_fdc_t *fdc, uint8_t cmd, uint8_t mask) {
    lisa_fdc_write8(fdc, OFF(B_RWTS), mask);
    issue(fdc, cmd);
}

// Power-on: both drives' interrupts enabled.
TEST(test_mask_starts_enabled) {
    lisa_fdc_t *fdc = make_fdc();
    ASSERT_EQ_INT(lisa_fdc_read8(fdc, OFF(B_IMSK)), 0x88);
    lisa_fdc_delete(fdc);
}

// LOS's SONYINT disables floppy interrupts ($87 with $88) when one arrives
// before the Sony driver is configured -- a diskette present at power-on.  FDIR
// must drop although the event stays latched, or the level-1 handler re-enters
// forever; re-enabling ($86, as the driver's INITDISK does) raises it again.
TEST(test_disable_drops_fdir_and_enable_restores_it) {
    lisa_fdc_t *fdc = make_fdc();
    lisa_fdc_write8(fdc, OFF(B_IST), 0x90); // disk inserted on the $80 drive
    mask_cmd(fdc, CMD_ENBLDRV, 0x80);
    ASSERT_TRUE(fdir);
    mask_cmd(fdc, CMD_DISDRV, 0x88);
    ASSERT_TRUE(!fdir);
    ASSERT_EQ_INT(lisa_fdc_read8(fdc, OFF(B_IMSK)), 0x00);
    ASSERT_EQ_INT(lisa_fdc_read8(fdc, OFF(B_IST)), 0x90); // still latched
    mask_cmd(fdc, CMD_ENBLDRV, 0x88);
    ASSERT_TRUE(fdir);
    lisa_fdc_delete(fdc);
}

// Each mask bit gates its own drive's nibble: bit 3 bits 0-3, bit 7 bits 4-7.
TEST(test_mask_gates_per_drive) {
    lisa_fdc_t *fdc = make_fdc();
    lisa_fdc_write8(fdc, OFF(B_IST), 0x90); // event on the $80 drive (high nibble)
    mask_cmd(fdc, CMD_DISDRV, 0x80); // disable that drive only
    ASSERT_TRUE(!fdir);
    mask_cmd(fdc, CMD_DISDRV, 0x08);
    lisa_fdc_write8(fdc, OFF(B_IST), 0x0C); // event on the other drive (low nibble)
    mask_cmd(fdc, CMD_ENBLDRV, 0x08); // enable the other drive
    ASSERT_TRUE(fdir);
    lisa_fdc_delete(fdc);
}

int main(void) {
    RUN(test_empty_drive_read_reports_no_disk);
    RUN(test_clrstat_clears_stale_read_error);
    RUN(test_enbldrv_clears_stale_read_error);
    RUN(test_initdisk_after_probe_reports_ok);
    RUN(test_mask_starts_enabled);
    RUN(test_disable_drops_fdir_and_enable_restores_it);
    RUN(test_mask_gates_per_drive);
    printf("[PASS] All Lisa FDC status tests passed\n");
    return 0;
}
