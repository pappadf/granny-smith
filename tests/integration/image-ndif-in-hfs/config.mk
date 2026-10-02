# Integration test: an NDIF disk image stored in an HFS volume attaches as a
# CD-ROM -- the way a Toast image of a disc full of Disk Copy 6 images is
# used.
#
# lib/make-hfs-volume.py builds the HFS volume at setup and writes the System
# 6.0.8 floppy into it as an NDIF file: zero-fill, copy and ADC chunks, with
# the chunk map in the file's 'bcem' resource.  Both layers are written from
# their specs, not from the emulator's readers.  Opening the NDIF reads its
# resource fork through the HFS catalog and its chunks through the data
# fork's extent, so the attach exercises the wrapper over a nested source.

TEST_NAME := storage.ndif_in_hfs
TEST_DESC := Attach an NDIF image held inside an HFS disk image as a SCSI CD-ROM

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := python3 lib/make-hfs-volume.py "$(WORK_DIR)/toast.img" 2048 "Disk.ndif=$(TEST_DATA)/systems/System_6_0_8.dsk@ndif"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
