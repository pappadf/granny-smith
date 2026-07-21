# Integration test: Apple Lisa 2 — boot LOS from the installed ProFile hard disk.
#
# Boots the Lisa 2 (rev-H ROM) with NO floppy, from the ProFile image produced by
# the lisa-los-profile install test, then boots to the Office System desktop.
#
# This test deliberately exercises the image's OWN on-disk PRAM snapshot rather
# than papering over it.  It seeds a `--coldboot` PRAM: BootVol=2 (so the ROM
# auto-boots the parallel-port ProFile) but an empty device table and a corrupted
# checksum.  The OS then computes pm_good=false (SOURCE-STARTUP INIT_CONFIG) and
# restores the device configuration from the boot volume's MDDF snapshot — the
# copy the installer writes at a *clean shutdown from the ProFile*.  So a good
# image (clean-shutdown snapshot listing the ProFile) boots to the desktop, while
# a broken image (e.g. saved from the install menu without an orderly shutdown,
# leaving a floppy-only snapshot) fails with error 10738.
#
# A fully-seeded hardware PRAM (the device table + valid checksum) would instead
# carry the boot itself and MASK a broken on-disk snapshot — which is why this
# test seeds --coldboot.  (The pm_good=true path is still covered: lisa-los-profile
# reboots the freshly installed ProFile with a full seed and matches the desktop.)
# PRAM format + synthesis: docs/machines/lisa/pram_format.md.
#
# Setup copies the installed image to a writable scratch copy (so the staged
# input is never modified) and synthesizes the cold-boot PRAM with seed_pram.py.
#
# Input (gitignored proprietary data, staged by a maintainer for CI): the
# installed ProFile image — produced by running lisa-los-profile and copying its
# lisa-los-installed.image to
# tests/data/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image.

TEST_NAME := Apple Lisa 2 boot LOS from the ProFile
TEST_DESC := Cold-boots the installed ProFile via its on-disk PRAM snapshot (no floppy); catches a non-clean-shutdown image

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=2048
TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image" && python3 lisa-profile-boot/seed_pram.py "$(WORK_DIR)/profile.pram" --coldboot
