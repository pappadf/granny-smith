# Integration test configuration: Apple Lisa 2 — save the Sony diskette.
# machine.floppy.drive.0.save writes the inserted diskette (source + delta +
# the in-memory sector tags) to a new self-contained DiskCopy 4.2 file.

TEST_NAME := Apple Lisa 2 floppy save
TEST_DESC := Saves the inserted diskette (with its 12-byte sector tags) as a DiskCopy 4.2 file and re-inserts it

TEST_ROM := roms/lisa2-revh-098917b2.rom

TEST_ARGS := model=lisa ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
