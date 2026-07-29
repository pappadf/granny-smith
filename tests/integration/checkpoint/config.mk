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

# Media: system_6_0_8_20mb_8_24gc.img, not hd1.zip. Verified 2026-07-27:
# hd1 is the same System 6.0.8 at the same 21,411,840-byte ST225N geometry,
# and the GC image's System Folder is a strict superset (AppleShare
# included), so every geometry/catalog assert holds unchanged — and this
# removes the suite's last TEST_SETUP unzip (§6.1).
# Copy the SCSI HD image and the floppy into the results directory:
# both stay out of tests/data so the originals are never touched.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_RESULTS_DIR)/hd.img" && cp $(TEST_DATA)/systems/System_6_0_8.dsk $(TEST_RESULTS_DIR)/System_6_0_8.dsk

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
