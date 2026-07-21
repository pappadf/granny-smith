# Integration test configuration: IIcx Keyboard (System 7.1 Find dialog)
#
# Regression guard for the ADB keyboard "drops most keystrokes" bug on a IIcx
# booted from the System 7.1 (1.4 MB) "Disk Tools" floppy.  The script drives
# Finder ENTIRELY with the mouse to open File > Find... (proving the ADB mouse
# path is healthy), then types "macintosh" into the Find dialog's text field
# via the ADB keyboard path and asserts the field reads exactly "macintosh".
#
# See test.script for the full provenance of the bug and its fix
# (src/core/peripherals/adb.c aborted-keyboard-Talk recovery).

TEST_NAME := IIcx Keyboard (System 7.1 Find dialog)
TEST_DESC := IIcx ADB keyboard under System 7.1: types "macintosh" into Finder's Find dialog and verifies every key lands

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# 8 MB RAM matches the boot-matrix IIcx + System Software row.  The floppy is
# inserted in-script (machine.boot discards the daemon's initial drive state),
# so TEST_ARGS only pins the model + RAM baseline.
TEST_ARGS := model=iicx ram=8192
