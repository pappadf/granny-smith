# Integration test configuration: IIci A/UX 2.0.1 HD Boot
#
# Boots A/UX 2.0.1 from gs-test-data's systems/aux_2_0_1_80mb_iici.dmg, an
# HD80SC that granny-smith installed from the 2.0.1 floppies and CD: the Mac
# side runs A/UX Startup, which boots the kernel straight to the graphical
# login; root logs in, the A/UX Finder comes up, CommandShell opens from the
# Apple menu and runs `uname -a`.  The disk is a UDZO UDIF read in place; the
# delta goes to the run's private GS_STORAGE_CACHE.

TEST_NAME := IIci A/UX 2.0.1 HD Boot
TEST_DESC := Boot A/UX 2.0.1 on the IIci (8 MB, built-in video) from an installed HD80SC: graphical login as root, A/UX Finder, CommandShell, uname.

# Macintosh IIci ROM (checksum 0x368CADFE)
TEST_ROM := roms/iici-368cadfe.rom

TEST_ARGS := model=iici ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
