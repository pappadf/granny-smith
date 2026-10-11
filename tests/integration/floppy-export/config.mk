# Integration test configuration: Mac floppy export (Plus, IWM).
# machine.floppy.drive[N].disk.export writes the inserted disk to a new file;
# the name picks the format (image_export_to).

TEST_NAME := Floppy disk export
TEST_DESC := disk.export to raw and to DiskCopy 4.2 (with the sector tags), refusing to overwrite

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := fd0=$(TEST_DATA)/systems/System_6_0_8.dsk

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
