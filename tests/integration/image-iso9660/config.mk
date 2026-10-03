# Integration test: an ISO 9660 disc is a disk the VFS descends into.
#
# The iso9660 unit suite covers the reader over discs it lays out byte by
# byte; this covers the plumbing -- detection by the format registry, the
# bare-volume namespace, Joliet names through the VFS -- on a disc made by
# make-fixture.py from ECMA-119 and the Joliet spec, independently of the
# reader.  The disc carries a floppy image, so descent goes on through the
# file into the HFS volume inside it.
#
# A second disc is a hybrid: the same ISO tree, with Apple's "AA" Finder info
# on its Read Me, plus a partition map and an HFS volume (made by
# lib/make-hfs-volume.py) -- the HFS side lists as partitionN, the ISO side
# as "iso9660".

TEST_NAME := storage.iso9660
TEST_DESC := Descend into an ISO 9660 disc (Joliet names), through a floppy image on it, and into both sides of an ISO+HFS hybrid with Apple Finder info

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := python3 image-iso9660/make-fixture.py "$(WORK_DIR)/disc.iso" "Read Me.txt=image-iso9660/config.mk" "Disk Images/System 6.0.8.dsk=$(TEST_DATA)/systems/System_6_0_8.dsk" && python3 lib/make-hfs-volume.py "$(WORK_DIR)/hybrid-hfs.img" 1024 "Hello.txt=image-iso9660/config.mk" && python3 image-iso9660/make-fixture.py --finder "Read Me.txt=TEXT/ttxt" --finder-out "Read Me.txt=$(WORK_DIR)/readme.finf" --hybrid-hfs "$(WORK_DIR)/hybrid-hfs.img" "$(WORK_DIR)/hybrid.iso" "Read Me.txt=image-iso9660/config.mk"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
