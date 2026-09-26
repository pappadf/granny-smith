# Integration test: the floppy drives are the profile's (M7,
# N-06, #177).
#
# The floppy controller always has two drive selects, and the object model
# exposed drive[1] on every machine.  On a one-drive Quadra 700 an insert
# into drive 1 succeeded -- a disk in a drive the machine does not have --
# and on the PDM and TNT boards drive 1 read "empty" but refused as
# "occupied", a phantom the core's auto-select relied on to stay off it.
# Drive selection is now bounded by the profile's floppy_slots, the object
# model shows only real drives, and a nonexistent drive holds nothing.

TEST_NAME := Floppy slots
TEST_DESC := drive[1] does not exist on one-drive machines (q700, pm6100, ans500); a second fd= finds no free drive

TEST_ROM := roms/q700-q900-420dbff3.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/System_3_2_0.dsk" "$(WORK_DIR)/a.dsk" && cp "$(TEST_DATA)/systems/System_3_3_0.dsk" "$(WORK_DIR)/b.dsk"
# Two fd= on a one-drive machine: the first goes to drive 0, the second finds
# no free drive (it used to land in the q700's nonexistent drive 1).
TEST_ARGS := model=q700 fd=$(WORK_DIR)/a.dsk fd=$(WORK_DIR)/b.dsk

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
