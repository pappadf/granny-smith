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
#
# Finally, to leave a CLEANLY-UNMOUNTED artifact (so the saved image cold-boots
# without the "startup disk was in use" scavenge), the test reboots the machine
# off the freshly installed ProFile (no floppy; a synthesized clean-shutdown PRAM
# auto-boots it — see lisa-profile-boot), reaches the Office System desktop, and
# presses the soft power-off switch (`power.off`).  Pressing it at the floppy
# install menu is a no-op — the floppy-booted installer does not act on the COPS
# $FB switch — so the orderly FS_Shutdown (boot volume MDDF mountinfo := unmounted)
# only happens once the OS is running FROM the ProFile.  The image is saved after
# the screen goes blank (powered off).
#
# TEST_SETUP synthesizes the boot PRAM with the shared seed_pram.py (reused from
# lisa-profile-boot): BootVol=2 + the ProFile in the device-config table, exactly
# what the installer leaves in PRAM at clean shutdown.

TEST_NAME := Apple Lisa 2 LOS 3.1 ProFile install
TEST_DESC := Full LOS 3.1 install onto the ProFile, then reboot off it and cleanly power off

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-1.image
TEST_SETUP := python3 lisa-profile-boot/seed_pram.py "$(WORK_DIR)/profile.pram"
