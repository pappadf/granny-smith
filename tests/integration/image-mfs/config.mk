# Integration test: an MFS floppy (System 2.0.1) is a disk the VFS reads.
#
# The mfs unit suite covers the reader over volumes it lays out; this reads
# a real 400K MFS floppy through the registry and the bare-volume namespace:
# its flat directory, both forks and the Finder info, and a copy-out of the
# whole volume with resource forks kept as AppleDouble sidecars.

TEST_NAME := storage.mfs
TEST_DESC := Read a System 2.0.1 MFS floppy: its files, both forks, and a copy-out of the volume

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
