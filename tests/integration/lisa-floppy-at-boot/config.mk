# Integration test configuration: Apple Lisa 2 -- LOS 3.1 starts from the
# ProFile with a diskette already in the drive at power-on.
#
# Before the Sony driver is configured, the OS answers a floppy interrupt by
# disabling floppy interrupts ($87 with mask $88) and returning.  The
# controller must honour that mask (FDIR = IST AND IMsk), or the latched
# disk-inserted event keeps FDIR up and the level-1 handler runs forever: the
# boot hung at the ROM's hourglass.  With the mask honoured LOS reaches the
# desktop and, once its driver is up, reports the diskette.

TEST_NAME := Apple Lisa 2 LOS 3.1 with a diskette at power-on
TEST_DESC := Boots LOS 3.1 off the ProFile with a floppy already inserted; it must reach the desktop

TEST_ROM := roms/lisa2-revh-098917b2.rom

TEST_ARGS := model=lisa ram=2048

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
