# Integration test configuration: AppleShare from the beige G3 over LocalTalk
#
# Mac OS 9's Open Transport LocalTalk driver on Heathrow sends each frame by
# DBDMA and ends it with WR0 "reset Tx underrun/EOM", then waits for the
# Tx Underrun/EOM External/Status interrupt (WR15 bit 6) on channel B's own
# Heathrow source before it sends the next frame; it takes a received
# frame's end from the Special Receive Condition the DMA leaves standing
# after the CRC bytes.  None of that was modelled, so the G3 sent its
# lapENQ/lapRTS and never a data frame, and the Chooser listed nothing.
# This row boots 9.2.1, lists the emulated server in the Chooser's
# AppleShare panel, logs in as Guest and reaches the volume list.

TEST_NAME := AppleShare from the beige G3
TEST_DESC := pmg3dt, Mac OS 9.2.1: Chooser -> AppleShare lists Shared Folders over LocalTalk (ESCC channel B by DBDMA), logs in as Guest and lists the volume

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

# A fresh share: the server keeps its catalog in "<share>/.gs-afp".
TEST_SETUP := rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && printf 'hello\n' > "$(WORK_DIR)/share/hello"

TEST_ARGS := model=pmg3dt ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
