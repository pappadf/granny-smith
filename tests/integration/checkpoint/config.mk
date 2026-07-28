# Integration test: Checkpoint save/load across processes (Plus)
#
# Merged from the former checkpoint + checkpoint2 pair (proposal-
# integration-test-rework §2.2): same ROM, media, golden and flow — they
# differed only in runner plumbing. This keeps checkpoint2's static
# scripts and persistent results directory (debuggable) plus its
# post-restore mouse drive, and checkpoint's cross-configuration
# restore: step 2 boots a SCSI HD, then loads a FLOPPY-booted
# checkpoint, so the restore has to replace a live, differently-shaped
# device set rather than fill in a blank machine.
#
# Also keeps the Plus x 6.0.8-HD matrix cell lit (§7 keeps checkpoint on
# 6.0.8 deliberately) and exercises save/load under a System 6 guest.

TEST_NAME := Checkpoint Save/Load (Plus, cross-process)
TEST_DESC := Floppy checkpoint restored over an HD-booted machine; desktop + post-restore interaction

TEST_ROM := roms/plus-v3-4d1f8172.rom

# Two-step custom runner (save process + restore process).
TEST_RUNNER := run.sh

# Unzip the SCSI HD image and copy the floppy into the results directory:
# both stay out of tests/data so the originals are never touched.
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_RESULTS_DIR) && cp $(TEST_DATA)/systems/System_6_0_8.dsk $(TEST_RESULTS_DIR)/System_6_0_8.dsk

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
