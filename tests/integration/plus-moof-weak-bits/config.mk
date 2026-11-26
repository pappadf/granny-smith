# Integration test configuration: Plus + a MOOF with a weak area
#
# A MOOF carrying a weak area -- records separated by runs of 17 cells with
# no flux transition, where a sector's data field was -- made at setup
# without any copyrighted media: the emulator's own MOOF writer lays the
# System 6.0.8 disk down as a Mac formats it (files.convert to .moof), and
# make-weak-moof.py edits the bits of one track and write-protects the
# disk.  The Plus boots from the plain MOOF with the weak one in the
# external drive.
#
# At the Finder a small guest stub (stub.s) reads the weak area as
# software that depends on it does: it hooks the Sony driver's data-field
# read (JRdData, $022E), _Reads the weak sector fifteen times -- once per
# revolution -- copying the nibbles after its address field each time,
# counts the records each read sees and all of them together, and passes
# only when the first read sees fewer than 70 % of them and all reads
# together at least that many.  It passes only because the drive reads the
# runs as fresh noise on every revolution; reading the same nibbles every
# time, the first read sees them all.  Then the reads are checked to be
# deterministic: a consolidated and a quick checkpoint saved between reads
# 7 and 8 restore to the same remaining reads, nibble for nibble; the same
# commands from the Finder again give the same reads; and a second process
# gives the same digest of all fifteen.

TEST_NAME := Plus weak-bit MOOF — a weak area reads differently every revolution
TEST_DESC := A guest reader that needs the weak area's reads to differ sees them differ, deterministically and across checkpoints

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
