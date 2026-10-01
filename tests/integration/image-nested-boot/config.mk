# Integration test: a floppy image stored inside another disk image boots.
#
# The image-* unit rows read nested images through the VFS; this one hands
# a nested path to a drive.  The outer volume is a classic HFS disk built at
# setup by lib/make-hfs-volume.py (written from Inside Macintosh, not from
# the emulator's reader) holding the System 6.0.8 floppy as a plain file,
# and fd0= names the floppy through it: the drive reads every boot sector
# through the HFS file's extent in the outer image.

TEST_NAME := storage.nested_boot
TEST_DESC := Boot a Plus from a floppy image held as a file inside an HFS disk image

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := python3 lib/make-hfs-volume.py "$(WORK_DIR)/outer.img" 2048 "inner.img=$(TEST_DATA)/systems/System_6_0_8.dsk"
TEST_ARGS := fd0=$(WORK_DIR)/outer.img/partition1/inner.img

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
