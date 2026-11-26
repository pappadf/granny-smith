# Integration test configuration: Plus + a copy-protected MOOF capture
#
# A MOOF carrying the kinds of copy protection that live on the track, not
# in the sectors, made at setup without any copyrighted media: the
# emulator's own MOOF writer lays the System 6.0.8 disk down as a Mac
# formats it (files.convert to .moof), and make-protected-moof.py edits the
# bits -- a $DE in front of one data mark, and a field that is no sector,
# with an exact count of self-sync bytes, in a gap on another track.  Every
# sector still reads, so the Plus boots from it.
#
# At the Finder a small guest stub (stub.s) does what a protection check
# does: it hooks the Sony driver's data-field read (JRdData, $022E), _Reads
# a block of each track, and from where the driver left the head reads on
# through the IWM, looking for the signature and counting the sync bytes.
# Both are there only because the drive serves the captured tracks.  Then
# the tracks are checked to stay the capture's through a consolidated and a
# quick checkpoint restore, a second drive holding the same file, an eject,
# the same capture gzipped and inside a zip, and a .moof export taken while
# the disk is mounted; and a write-protected disk's track takes no raw IWM
# writes.  Last, a consolidated checkpoint restores with the MOOF moved
# away (the disk comes back as its sectors, and the guest reads it), and in
# two more processes a quick checkpoint is refused once the file holds
# another capture of the same size and time stamp.

TEST_NAME := Plus copy-protected MOOF — the drive serves the captured tracks
TEST_DESC := A guest hook on the Sony driver finds a track signature and an exact sync count that exist only in the bits

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
