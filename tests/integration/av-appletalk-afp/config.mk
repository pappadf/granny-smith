# Integration test configuration: AppleShare from the AV Quadras
#
# The 840AV and 660AV send a LocalTalk data frame through the PSC's DMA
# channel 5 (SCC B), not through the SCC's data register: the ROM's .MPP
# writes only lapENQ/lapRTS by hand.  With that channel unmodelled the
# guest's every DDP frame was lost, and the Chooser listed nothing.  This
# row boots System 7.1 on each model and lists the emulated server in the
# Chooser's AppleShare panel; on the 840AV it also logs in as Guest and
# reaches the volume list.

TEST_NAME := AppleShare from the AV Quadras
TEST_DESC := 840AV and 660AV, System 7.1: Chooser -> AppleShare lists Shared Folders over LocalTalk (PSC SCC DMA); the 840AV logs in as Guest and lists the volume.

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom

# A fresh share: the server keeps its catalog in "<share>/.gs-afp".
TEST_SETUP := rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && printf 'hello\n' > "$(WORK_DIR)/share/hello"

TEST_ARGS := model=q840av ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
