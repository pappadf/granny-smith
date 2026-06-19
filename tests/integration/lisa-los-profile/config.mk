# Integration test: Apple Lisa 2 — full LOS 3.1 install onto the ProFile hard disk.
#
# Boots the Lisa 2 (rev-H ROM) to the Install menu with a (blank) ProFile attached,
# then warps+clicks through the WHOLE Office System install and verifies it completes:
#   Install -> "use parallel disk?" (ProFile DETECTED, not "no usable disks")
#           -> OK -> Continue (erase/init) -> Don't Share (whole disk to LOS)
#           -> copy diskettes 2,3,4,5 -> reinsert diskette 1 (boot_remount writes the
#              boot tracks) -> back at the menu, now with the ProFile as startup disk.
#
# This exercises the full host->OS path AND the three fixes that make the install run:
#   (a) VIA2 decoded over the whole $D800-$D9FF chip-select window (bit 8 don't-care)
#       so the OS ProFile driver (SYSTEM.CD_PROFILE / PROF_INIT, which addresses VIA2
#       from base $D801) reaches the parallel port — a narrow $D901 mapping silently
#       drops the driver's register writes and the ProFile is never detected;
#   (b) the FDC disk-in event uses the upper-drive $C05F bits (DISKIN2|summary = $90)
#       so the OS detects each inserted diskette (else Mount returns 614 and the
#       installer silently re-prompts);
#   (c) faithful disk reinsert: the boot diskette retains its overmount_stamp across
#       the eject+reinsert so boot_remount accepts it and finishes the install.

TEST_NAME := Apple Lisa 2 LOS 3.1 ProFile install
TEST_DESC := Full LOS 3.1 install onto the ProFile (detect, init, copy all diskettes, write boot tracks)

TEST_ROM := roms/098917B2-LisaH.rom
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/systems/LisaOfficeSystem-3.1/Install1.image
