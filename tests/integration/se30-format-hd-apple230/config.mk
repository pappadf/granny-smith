# Integration test configuration: Apple-branded 230 MB drive acceptance
#
# Guards the two things Copland's installer depends on (see
# local/gs-docs/projects/copland/bring-up-plan.md §4.2):
#   1. the drive catalog offers an Apple-shipped mechanism above the
#      installer's 230 MB floor, and
#   2. Apple's own formatter accepts it — i.e. our MODE SENSE page 0x30
#      "APPLE COMPUTER, INC." response really is what makes a drive
#      Apple-branded to Apple's tools, at a capacity no previous catalog
#      entry reached (the old ceiling was HD160SC, 169 MB).
#
# HD230SC rather than the larger HD500SC on purpose: it is the smallest
# catalog entry that clears the floor, so it is both the boundary case and
# the cheapest image to materialize (images are not sparse in the browser's
# OPFS, where a drive costs its full size).

TEST_NAME := SE/30 Apple 230 MB Drive Acceptance
TEST_DESC := Creates an HD230SC (QUANTUM LP240S) image, boots 7.0.1, and verifies Apple HD SC Setup recognises and initializes it

# SE/30 Universal ROM
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# Remove stale delta/journal files so the floppy boots from a clean base image
TEST_SETUP := rm -f $(TEST_DATA)/systems/System_7_0_1.image.delta $(TEST_DATA)/systems/System_7_0_1.image.journal

# Boot from floppy with 8 MB RAM; the HD is created and attached in test.script
TEST_ARGS := fd=$(TEST_DATA)/systems/System_7_0_1.image ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
