# Integration test: Apple Lisa 2 — LOS 3.1 installer detects the ProFile hard disk
# Boots the Lisa 2 (rev-H ROM) to the Install menu with a (blank) ProFile attached,
# warps the cursor to Install and clicks it, and verifies the installer DETECTS the
# ProFile on the parallel connector ("Do you want to use the disk attached to the
# parallel connector?" — OK / More / Cancel) rather than reporting "no usable disks".
#
# This exercises the full host→OS path:
# the COPS mouse warp + click, and — critically — the OS ProFile driver
# (SYSTEM.CD_PROFILE / PROF_INIT) successfully reaching the VIA2 parallel port.
# PROF_INIT addresses VIA2 from base $D801, so the device must be decoded over the
# whole $D800-$D9FF chip-select window (bit 8 don't-care); a narrow $D901 mapping
# silently drops the driver's register writes and the ProFile is never detected.

TEST_NAME := Apple Lisa 2 LOS 3.1 ProFile detection
TEST_DESC := Installer detects the attached ProFile (reaches the parallel-connector disk prompt)

TEST_ROM := roms/098917B2-LisaH.rom
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/systems/LisaOfficeSystem-3.1/Install1.image
