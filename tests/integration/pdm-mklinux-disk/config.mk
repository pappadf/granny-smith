# Integration test configuration: build the drive MkLinux DR3 installs onto.
#
# Half one of recreating `systems/mklinux_dr3_single_169mb.img` from nothing
# but the stock 7.5 volume and the DR3 disc:
#
#   pdm-mklinux-disk     partition, bless, and load the Mac OS side   [here]
#   pdm-mklinux-install  the DR3 installer onto /dev/sda6             [next]
#
# The split is the same reasoning the Copland trio uses: the disk build is
# slow, deterministic and almost never changes, while the installer is what
# gets iterated on.  Publishing the Mac-OS-only drive once means an install
# attempt never re-pays for Drive Setup, pdisk and five Finder drags.
#
# ONE DRIVE, TWO OPERATING SYSTEMS.  Drive Setup cannot make A/UX partitions
# and pdisk cannot write drivers or an HFS volume — but Drive Setup will leave
# part of a drive Unallocated when its Mac OS volume is shrunk, and pdisk fills
# that.  Neither has to do the other's job.  The recipe, measured step by step,
# is gs-docs projects/mklinux/notes/single-disk-recipe.md.
#
# MEDIA: the DR3 disc is a 663 MB hybrid ISO under
# local/gs-docs/projects/mklinux/ and is not in gs-test-data, so this test
# SKIPS cleanly where it is absent.  The image it produces IS in gs-test-data,
# so the per-commit boot test needs none of this.

TEST_NAME := Power Macintosh 7100 — build the single Mac OS + MkLinux drive
TEST_DESC := Partition a blank HD160SC with Drive Setup 1.5 and pdisk off the DR3 disc, copy the five Mac Files into a System Folder and bless the new volume with it

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 24 MB — the 7100's own ram_default, and past DR3's 16 MB floor.
TEST_ARGS := model=pm7100 ram=24576

# WHICH IMAGE THIS PAIR PRODUCES.  The 7.5 one, and only that one.  `mklinux-boot`
# moved to `systems/mklinux_dr3_76_single_169mb.img` when the 7500 joined it: a
# PCI Power Mac rejects a 7.5 startup volume with DSErrCode $66 (dsOldSystem)
# before the Booter runs.  That image was made by swapping the System Folder on
# a copy of this one -- same map, byte-identical ext2 root -- not by a separate
# install, so this pair is still where the MkLinux side actually comes from.
#
# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
