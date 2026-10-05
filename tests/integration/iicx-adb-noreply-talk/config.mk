# Integration test: IIcx ADB no-reply Talk must not post a button event
# Regression for issue #122.  Boots MacTest on the IIcx, drives the
# Options > Test Selections dialog with five plain clicks, and after every
# release asserts that MBState stays "button up" for the next 100 M
# instructions.  Before the fix the VIA's own 8-cycle shift-out timer
# could raise IFR_SR before the ROM had even started the ADB command, the
# ROM read the command byte back as the reply's first data byte, and the
# mouse driver posted it as a button-down ($3C, bit 7 clear) with no
# report behind it.  Whether that raced depended on the E-clock phase of
# each explicit Talk R0, so the failure appeared and vanished with any
# change to the emulator's timing; this sequence is one that failed.

TEST_NAME := IIcx ADB no-reply Talk
TEST_DESC := A Talk R0 the mouse does not answer must leave MBState alone (issue #122)

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# Same media and RAM as iicx-mactest; MacTest's System is the one that
# showed the stale button-down.
TEST_ARGS := model=iicx ram=4096 fd0=$(TEST_DATA)/apps/MacTest-IIcx-IIci.image

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
