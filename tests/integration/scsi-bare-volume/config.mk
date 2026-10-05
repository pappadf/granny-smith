# Integration test configuration: bare HFS volumes (and driverless partitioned
# disks) attached as SCSI hard disks
#
# A bare HFS volume image — no Driver Descriptor Map, no partition map, no
# driver; the shape Mini vMac and the archive.org Mac library use — is
# invisible to the ROM's SCSI boot code.  scsi.attach_hd wraps such a volume
# with a synthesised Apple Partition Map and the in-tree GSDisk 68k driver
# (src/core/storage/image_wrap.c, src/core/storage/gsdisk/), so it boots and
# mounts like any other SCSI disk.  These rows boot the two archive.org items
# the web UI's worked example uses on one machine of every family that boots
# Mac OS from SCSI — Plus, SE/30, IIci, Quadra 700, IIfx, Quadra 840AV, Power
# Mac 6100 and 7500 (the Plus's SCSILoad has no partition-map check; the later
# loaders verify the driver partition and its checksum; the PowerPC ROMs run
# the 68k driver under emulation).
#
# MEDIA: tests/data/systems/system_7_5_3_25mb_bare.img and
# system_7_0_1_10mb_bare_plus.img — archive.org's AppleMacintoshSystem753 and
# AppleMacintoshSystem701 disk images, byte for byte.  Rows skip cleanly when
# they are absent.  See docs/core/storage/bare-volume-wrapper.md.

TEST_NAME := SCSI bare-volume wrapper
TEST_DESC := Attach naked HFS volumes and driverless partitioned disks as SCSI hard disks through the partition-map + GSDisk driver wrapper and boot them on one machine of every Mac OS family with SCSI

TEST_ROM := roms/iici-368cadfe.rom

# The script boots each machine itself (several models and ROMs).
TEST_ARGS := model=iici ram=8192

# The q840av-71 row's volume: the AV suite's System 7.1 disk with its partition
# map and driver stripped off (the extraction is skipped when that disk is
# absent, and the row then skips).  The driverless row's disk: the bare 7.5.3
# volume put behind a partition map with no driver, the shape Disk Copy and
# SheepShaver write.  The trimmed row's: the bare 7.5.3 volume cut off after
# its last allocation block in use.
TEST_SETUP := mkdir -p "$(WORK_DIR)" && { [ ! -f "$(TEST_DATA)/systems/system_7_1_77mb_av.img" ] || python3 "$(TEST_DATA)/../../scripts/apm-extract-hfs.py" "$(TEST_DATA)/systems/system_7_1_77mb_av.img" "$(WORK_DIR)/av71_bare.img"; } && { [ ! -f "$(TEST_DATA)/systems/system_7_5_3_25mb_bare.img" ] || python3 "$(TEST_DATA)/../../scripts/hfs-to-driverless-apm.py" "$(TEST_DATA)/systems/system_7_5_3_25mb_bare.img" "$(WORK_DIR)/driverless753.img" && python3 "$(TEST_DATA)/../../scripts/hfs-trim.py" "$(TEST_DATA)/systems/system_7_5_3_25mb_bare.img" "$(WORK_DIR)/trimmed753.img"; }

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
