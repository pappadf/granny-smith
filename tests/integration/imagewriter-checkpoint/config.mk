# Integration test: a checkpoint taken in the middle of an ImageWriter job
#
# The printer is fed a two-page job directly (no guest driver): a page of
# text and graphics and a form feed, then part of the second page.  A
# checkpoint is saved there and the job finished; then the checkpoint is
# loaded and the job finished again.  Both documents must be the same PDF:
# the printer's state, the sheet in progress (its dot planes), the document
# so far (the finished first page) and the counters all come back.

TEST_NAME := ImageWriter checkpoint mid-job
TEST_DESC := Save a checkpoint with a job half printed, finish it, restore and finish again: the same PDF both times.

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
