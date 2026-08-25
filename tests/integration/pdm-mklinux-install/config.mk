# Integration test configuration: install MkLinux DR3 onto the single drive.
#
# Half two of recreating `systems/mklinux_dr3_single_169mb.img` — the image
# `pdm-mklinux-boot` runs on every commit:
#
#   pdm-mklinux-disk     partition, bless, and load the Mac OS side   [done]
#   pdm-mklinux-install  the DR3 installer onto /dev/sda6             [here]
#
# It starts from the drive the first one publishes, so an install attempt never
# re-pays for Drive Setup, pdisk and five Finder drags.
#
# EVERY SCREEN IS WAITED FOR, NOT BUDGETED FOR.  A fixed budget between screens
# desynchronises the sequence the moment one step is slower than measured, and
# REGEN then records the stuck screen under the NEXT screen's name — an earlier
# version of this test shipped `install-done.png` containing a picture of the
# Partition Disks screen, and passed.  Every step now waits for the frame it
# expects and fails naming it.
#
# RUNTIME: long.  The Mach hand-off is ~1 G instructions from the splash click,
# the installer's own screens are cheap, and the 58 MB "Absolute Minimum"
# package set dominates.  `extended` because it produces an artifact, not
# because it is quick.
#
# MEDIA: the DR3 disc (a 663 MB hybrid ISO) and the drive pdm-mklinux-disk
# publishes both live under local/gs-docs/projects/mklinux/ and are not in
# gs-test-data, so this test SKIPS cleanly where they are absent.

TEST_NAME := Power Macintosh 7100 — install MkLinux DR3 onto the single drive
TEST_DESC := Boot the Booter into Mach_Kernel, run the DR3 installer onto /dev/sda6 with the Absolute Minimum package set, point lilo.conf at it and publish the finished image

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 40 MB.  Not the 7100's ram_default (24 MB) — this matches the machine the
# goldens in this directory were captured on, and a golden is not something to
# recapture because a setting drifted.  The installer's screens do not show the
# memory size, but that is not a thing to rely on unnecessarily.
TEST_ARGS := model=pm7100 ram=40960

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
