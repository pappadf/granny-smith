# Integration test: storage.export_raw flattens a disk image to a raw file.
# The NDIF bcem/ADC decode path is covered by the `ndif` unit suite; this
# exercises the export plumbing (VFS resolve -> image_open -> flatten to raw)
# and round-trips the result back through the VFS to prove it re-mounts.

TEST_NAME := storage.export_raw
TEST_DESC := Export a disk image to a flat raw file and re-mount it via the VFS

TEST_ROM := roms/iix-iicx-se30-97221136.rom
