# Integration test configuration: IIci CD-ROM boot
#
# Boots a Macintosh IIci from a bootable Apple system CD-ROM and nothing else —
# no hard disk, no floppy — proving the whole SCSI CD boot chain: the Start
# Manager scans the bus, reads the Driver Descriptor Map, loads the CD driver
# out of the disc's own Apple_Driver43 partition, mounts the Apple_HFS volume
# and boots the System Folder on it.
#
# There is NO CD-specific path in the ROM: a bootable Mac CD is mastered exactly
# like a hard disk (Apple Partition Map + driver partition + blessed System
# Folder), so this exercises the ordinary SCSI boot path against a read-only,
# 2048-byte-block device.  That is the whole point of the test — the CD is a
# transport variant, not a special case.
#
# STATUS: working end to end — the disc boots to the Finder desktop at 8 MB in
# 24-bit addressing, with no PRAM seeding.  Getting there needed one emulator
# fix: READ(6) rejected any transfer over BUF_LIMIT (256 * 512 bytes), which is
# unreachable on a hard disk's 512-byte blocks but caught every CD-ROM READ(6)
# of more than 64 of its 2048-byte blocks.  System 7.5.3 reported the refused
# read as "Not enough memory is available while using “System”".  See the
# cd-desktop row comment.
#
# MEDIA: tests/data/cdroms/SSW-7.5.3-CD.toast — Apple's "Macintosh System 7.5
# Version 7.5.3" CD (part 96073-016A-U, 1996), a 255 MB raw image with a real
# Apple Partition Map.  A .toast file from Toast is a raw sector dump, so it
# needs no conversion.  The test SKIPS cleanly when the image has not been
# fetched (the landable-before-data pattern), so it can land before the media
# is added to gs-test-data.

TEST_NAME := IIci CD-ROM boot
TEST_DESC := Boot a Macintosh IIci from a bootable Apple system CD with no other storage attached, all the way to the Finder desktop

TEST_ROM := roms/iici-368cadfe.rom

# The script boots the machine itself so it can attach the CD after
# construction and control the RAM size per arm.
TEST_ARGS := model=iici ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
