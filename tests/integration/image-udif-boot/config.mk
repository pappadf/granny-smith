# Integration test: boot from a disk stored as UDIF (.dmg).
#
# Every hard disk the web app stores is a GS-profile UDIF read in place
# (docs/internals/core/storage/image.md).  This boots one end to end: the
# System 7.5 disk converted with files.convert, booted on a IIci to the same
# desktop as the raw image (desktop.png was captured from the raw boot);
# checkpointed both ways -- a quick checkpoint that references the disk, a
# consolidated one that embeds it -- and restored in a fresh process; and exported, as
# .dmg and raw, the two decoding to the same bytes and the .dmg booting.

TEST_NAME := Boot from UDIF (IIci, System 7.5)
TEST_DESC := Convert to .dmg, boot to the raw boot's desktop, quick and consolidated checkpoint/restore, export and boot the export

TEST_ROM := roms/iici-368cadfe.rom

# Two processes (save, then restore in a fresh one).
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
