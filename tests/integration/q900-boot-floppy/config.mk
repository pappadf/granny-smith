# Integration test: Quadra 900 boots the Disk Tools floppy to the Finder
# (Phase G — the floppy path runs through the SWIM IOP mailbox protocol)
#
# Unlike the Q700's direct SWIM access, every tower floppy transfer is an
# IOP transaction: the .Sony driver posts xmtReq* commands into the SWIM
# IOP's XmtMsg[2] mailbox and the firmware model answers with DriveStatus
# records and sector data.  This pins that path end-to-end.

TEST_NAME := Quadra 900 Disk Tools floppy boot to Finder
TEST_DESC := Floppy boot through the SWIM IOP mailbox path

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q900 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
