# Integration test configuration: Mac Plus blank-floppy handling
#
# Ports the unique parts of the legacy web UI's floppy.spec.ts (retired
# with the legacy UI): creating a blank floppy image, running two floppy
# drives at once on a Plus under System 6, the OS's unreadable-disk
# detection, and Finder-driven eject. The full multi-step Finder file-copy
# choreography from the legacy spec is intentionally NOT reproduced — it's
# a fragile pixel dance and the underlying floppy read/write (GCR/IWM) is
# already covered by the boot tests; this keeps the deterministic,
# high-value bits.

TEST_NAME := Plus Blank Floppy
TEST_DESC := Create a blank floppy, mount it as the 2nd drive under System 6, hit the initialize-disk dialog, eject from Finder.

TEST_ROM := roms/Plus_v3.rom

# Boot from System 6.0.8 in the internal drive; the blank floppy is created
# by the script into WORK_DIR and inserted into the external drive.
TEST_ARGS := fd=$(TEST_DATA)/systems/System_6_0_8.dsk
