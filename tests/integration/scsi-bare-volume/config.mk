# Integration test configuration: bare HFS volumes attached as SCSI hard disks
#
# A bare HFS volume image — no Driver Descriptor Map, no partition map, no
# driver; the shape Mini vMac and the archive.org Mac library use — is
# invisible to the ROM's SCSI boot code.  scsi.attach_hd wraps such a volume
# with a synthesised Apple Partition Map and the in-tree GSDisk 68k driver
# (src/core/storage/image_wrap.c, src/core/storage/gsdisk/), so it boots and
# mounts like any other SCSI disk.  These rows boot the two archive.org items
# the web UI's worked example uses, on every 68k ROM family the driver has to
# satisfy (the Plus's SCSILoad has no partition-map check; the SE/30, IIci and
# Quadra loaders verify the driver partition and its checksum).
#
# MEDIA: tests/data/systems/system_7_5_3_25mb_bare.img and
# system_7_0_1_10mb_bare_plus.img — archive.org's AppleMacintoshSystem753 and
# AppleMacintoshSystem701 disk images, byte for byte.  Rows skip cleanly when
# they are absent.  See docs/core/storage/bare-volume-wrapper.md.

TEST_NAME := SCSI bare-volume wrapper
TEST_DESC := Attach naked HFS volumes as SCSI hard disks through the partition-map + GSDisk driver wrapper and boot them on Plus, SE/30, IIci and Quadra ROMs

TEST_ROM := roms/iici-368cadfe.rom

# The script boots each machine itself (several models and ROMs).
TEST_ARGS := model=iici ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
