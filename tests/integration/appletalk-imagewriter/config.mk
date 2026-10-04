# Integration test: the AppleTalk ImageWriter (the LocalTalk Option card)
#
# Boots System 6.0.8 on a Plus with the virtual ImageWriter II connected to
# LocalTalk (`machine.imagewriter.connection = "localtalk"`): the card
# publishes "Virtual ImageWriter:ImageWriter@*", the Chooser's AppleTalk
# ImageWriter lists it, and the Finder's Print Directory goes over PAP --
# the same ImageWriter byte stream a serial cable carries, in SendData
# transactions, ended by the workstation's EOF.  The document is the same
# PDF the serial port gives (mac-imagewriter's Faster golden).
#
# A second print with the printer out of paper: the card's statusBits word
# in its OpenConnReply says so (Technical Note NW20), the driver shows its
# out-of-paper alert instead of sending, and after paper is added and
# Continue clicked the job prints.
#
# What it guards: the card's PAP server (appletalk_imagewriter.c), its NBP
# type, the statusBits layout the driver reads, and the job end on EOF.

TEST_NAME := AppleTalk ImageWriter (LocalTalk Option card)
TEST_DESC := Chooser AppleTalk ImageWriter -> Virtual ImageWriter; Print Directory over PAP to a PDF; paper-out alert and recovery.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := hd=$(TEST_TMPDIR)/hd.img --print-dir=$(TEST_RESULTS_DIR)

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
