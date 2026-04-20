# Integration test configuration: SE/30 A/UX 3.0.1 Boot (retail SE dist)
# Verifies that the SE/30 boots the A/UX 3.0.1 retail Installation Boot
# Disk (SE) with the retail Install CD attached, and successfully mounts
# the CD-ROM as the root filesystem.  The test passes iff the kernel
# reaches the RTS of vfs_mountroot() at $10032CFC, which is only reached
# when probe_root / real_mount / VFS_ROOT all succeed; any failure panics
# via $10020454 instead.

TEST_NAME := SE/30 A/UX 3.0.1 Boot (retail SE)
TEST_DESC := Boot SE/30 from retail A/UX 3.0.1 SE floppy + CD; mount root from CD-ROM

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/SE30.rom

# Always start clean: copy the boot floppy (DiskCopy 4.2, 800 KB) into
# TEST_TMPDIR so its .delta/.journal are wiped with the tempdir on exit;
# symlink the 400 MB CD ISO instead of copying (delta/journal land next to
# the symlink in TEST_TMPDIR, so they are also wiped on exit); copy the
# formatted HD image produced by the se30-format-hd test.
#
# NOTE: the Makefile's TEST_SETUP extractor expands only $(TEST_DATA) and
# $(TEST_TMPDIR) — any other make variable will be passed through to the
# shell untouched and will resolve to empty.  Keep paths fully inlined, and
# on a single line (extractor is `grep -m1 '^TEST_SETUP'`).
TEST_SETUP := cp "$(TEST_DATA)/aux/aux_3.0.1/Installation Boot Disk SE.image" "$(TEST_TMPDIR)/boot.image" && ln -s "$(TEST_DATA)/aux/aux_3.0.1/APPLE_AUX_3-0-1_RETAIL.iso" "$(TEST_TMPDIR)/install.iso" && cp "$(TEST_DATA)/aux/hd-with-aux-partition.img" "$(TEST_TMPDIR)/hd.img"

# 16 MB RAM required for A/UX boot.
TEST_ARGS := ram=16384 fd=$(TEST_TMPDIR)/boot.image hd=$(TEST_TMPDIR)/hd.img cdrom=$(TEST_TMPDIR)/install.iso
