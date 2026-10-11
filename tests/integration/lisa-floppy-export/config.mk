# Integration test configuration: Apple Lisa 2 — export the Sony diskette.
# machine.floppy.drive.0.export writes the inserted diskette (source + delta +
# the in-memory sector tags) to a new file; a .dc42 name makes it a
# self-contained DiskCopy 4.2 file (image_export_to).

TEST_NAME := Apple Lisa 2 floppy export
TEST_DESC := Exports the inserted diskette (with its 12-byte sector tags) as DiskCopy 4.2 and raw, and re-inserts it

TEST_ROM := roms/lisa2-revh-098917b2.rom

TEST_ARGS := model=lisa ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
