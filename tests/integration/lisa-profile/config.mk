# Integration test configuration: Apple Lisa 2 — ProFile parallel hard disk.
# Boots the Lisa 2 (rev-H boot ROM) to its idle state, attaches a blank ProFile,
# then calls Apple's own boot-ROM PROREAD driver to read the controller's
# device-info block over the real VIA2 parallel handshake — exercising the full
# stack (ROM driver → VIA port-A hooks → lisa_profile device → BSY/CMD lines).

TEST_NAME := Apple Lisa 2 ProFile hard disk
TEST_DESC := Reads the ProFile controller's device-info block via the boot ROM's PROREAD driver

# Interleaved rev-H Lisa 2 boot ROM (341-0175-H / 341-0176-H), 16 KB, checksum 0x098917B2.
TEST_ROM := Lisa/roms/098917B2-LisaH.rom

# Lisa 2, 1 MB.  No floppy: the test drives the ProFile directly via PROREAD.
TEST_ARGS := model=lisa ram=1024
