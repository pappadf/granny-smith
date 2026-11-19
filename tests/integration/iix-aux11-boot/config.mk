# Integration test configuration: IIx A/UX 1.1 HD Boot
#
# Boots A/UX 1.1 from gs-test-data's systems/aux_1_1_80mb_iix.dmg, an HD80SC
# that granny-smith installed from the 26-disk floppy set: sash on the Mac
# side, `launch`, the kernel from the disk, single-user, `init 2`, a root
# login and `uname -a`.  The disk is a UDZO UDIF read in place; the delta
# goes to the run's private GS_STORAGE_CACHE.
#
# The Macintosh II Video Card runs Apple's 342-0008-A declaration ROM: the
# real Display Card 8*24 ROM cannot run A/UX 1.1 (its driver calls
# _GetVideoDefault, an A-line trap the kernel does not support).

TEST_NAME := IIx A/UX 1.1 HD Boot
TEST_DESC := Boot A/UX 1.1 on the IIx (8 MB, Macintosh II Video Card) from an installed HD80SC: sash, launch, single-user, multi-user, root login, uname.

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iix ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
