# Integration test: Apple Lisa 2 — boot LOS from the installed ProFile hard disk.
#
# Boots the Lisa 2 (rev-H ROM) with NO floppy, from the ProFile image produced by
# the lisa-los-profile install test, after synthetically seeding the parameter
# memory (PRAM) with exactly what the Office System installer writes at a clean
# shutdown: the device-configuration table listing the ProFile, plus BootVol=2
# (so the ROM auto-boots the parallel-port ProFile).  With that PRAM the boot ROM
# auto-boots the ProFile, the OS loader loads SYSTEM.OS, FIND_PM_IDS resolves the
# ProFile driver from the seeded device table (no "can't find boot cd"/10738
# fallback), and the OS comes up.  PRAM format + synthesis: docs/lisa_pram_format.md.
#
# Setup copies the installed image to a writable scratch copy (so the staged
# input is never modified) and synthesizes the PRAM with seed_pram.py.
#
# Input (gitignored proprietary data, staged by a maintainer for CI): the
# installed ProFile image — produced by running lisa-los-profile and copying its
# lisa-los-installed.image to
# tests/data/systems/LisaOfficeSystem-3.1/los-installed.image.

TEST_NAME := Apple Lisa 2 boot LOS from the ProFile
TEST_DESC := Boots the installed ProFile via a synthetic PRAM (auto-boot, no floppy)

TEST_ROM := roms/098917B2-LisaH.rom
TEST_ARGS := model=lisa ram=2048
TEST_SETUP := cp "$(TEST_DATA)/systems/LisaOfficeSystem-3.1/los-installed.image" "$(WORK_DIR)/profile.image" && python3 lisa-profile-boot/seed_pram.py "$(WORK_DIR)/profile.pram"
