# Integration test configuration: Plus + Beyond Dark Castle from MOOF floppies
#
# Boots the Macintosh Plus from Beyond Dark Castle disk 1 as an Applesauce
# MOOF bitstream capture (the moof-a-day collection's), with disk 2 in the
# external drive: the image layer decodes each MOOF's GCR tracks to its
# sectors (src/core/storage/image_moof.c), so the floppy controller, the
# boot and the VFS all see an ordinary 800K disk.  Checks the VFS view of
# both disks, then plays through the title and the Scores of Merit menu
# into the first room.
#
# The script inserts the disks itself, after the VFS checks: the VFS will
# not serve a file a drive holds open for writing.
#
# The MOOF files live in gs-test-data as apps/Beyond-Dark-Castle-Disk-1.moof
# and -2.moof, fetched by scripts/fetch-test-data.sh.

TEST_NAME := Plus Beyond Dark Castle — boot from MOOF
TEST_DESC := Boot the MOOF bitstream floppy to the game's title, menu and first room; VFS lists both MOOF disks

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
