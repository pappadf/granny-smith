# Integration test configuration: AppleShare from the AV Quadras over EtherTalk
#
# The 840AV's and 660AV's built-in Ethernet end to end.  Choosing
# "EtherTalk Built In" in the Network control panel opens .ENET, which
# runs three MACE loopback self-tests (internal, internal with MENDEC,
# external): a 79-byte frame out through PSC channel 2, back 83 bytes
# (FCS appended) through channel 1's chain buffers.  Any failure and the
# panel says "Could not switch to EtherTalk" and falls back to LocalTalk --
# which is what happened while the MACE was a register stub (and its
# address PROM failed .ENET's checksum before that).  Then EtherTalk Phase
# 2 comes up with no router, the Chooser's AppleShare panel finds the
# emulated server by an NBP lookup over Ethernet, and on the 840AV the row
# logs in as Guest, mounts "Shared" and copies a 410 KB file off it:
# every byte over AFP/ASP/ATP in extended-DDP ELAP frames.  The 660AV
# repeats the switch and the lookup.

TEST_NAME := AppleShare from the AV Quadras over EtherTalk
TEST_DESC := 840AV and 660AV, System 7.1: Network -> EtherTalk Built In (.ENET's MACE loopback self-tests through PSC channels 1/2), Chooser -> AppleShare lists Shared Folders over Ethernet; the 840AV logs in, mounts the volume and copies a 410 KB file off it.

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom

# A fresh share (the server keeps its catalog in "<share>/.gs-afp") holding
# a payload big enough for many multi-packet ATP responses -- the 410 KB
# MacTest floppy image is always-fetched test data, its content irrelevant.
TEST_SETUP := rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && cp "$(TEST_DATA)/apps/MacTest-Plus.image" "$(WORK_DIR)/share/payload.image"

TEST_ARGS := model=q840av ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
