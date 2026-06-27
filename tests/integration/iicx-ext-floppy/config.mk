# Integration test configuration: IIcx External Floppy Insert (System 7.1)
#
# Regression guard for the report that, on a IIcx booted to the System 7.1
# Finder desktop, inserting a floppy into the EXTERNAL drive (FD1) hung the
# machine — the disk never appeared on the desktop.  What actually happened: a
# SWIM drive-selection bug routed FD0 head-step commands to FD1 (in ISM mode
# the drive must be chosen by the ISM DRIVE1/DRIVE2 bits, not the stale IWM
# SELECT line), so an FD0 resource read stalled, the Finder bombed with
# "error type 41", and the ROM spun in its post-error wait loop.
#
# Fixed in src/core/peripherals/floppy.c (floppy_disk_control).  This test now
# asserts the Install disk mounts and its icon appears on the desktop; it goes
# red if the FD1 mount path regresses.  See test.script for full provenance.

TEST_NAME := IIcx External Floppy Insert (System 7.1)
TEST_DESC := Inserting a floppy into the IIcx external drive (FD1) under System 7.1 mounts it on the desktop (regression guard for the old Finder error-type-41 bomb)

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom

# 8 MB RAM, matching the boot-matrix IIcx + System Software rows and the
# sibling iicx-keyboard test.  Floppies are inserted in-script.
TEST_ARGS := model=iicx ram=8192
