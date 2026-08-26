# Integration test configuration: boot MkLinux DR3 on every PowerPC Mac we model.
#
# The keeper test of the MkLinux work: one published 169 MB disk, carrying Mac
# OS 7.6 on Apple_HFS partition 5 and MkLinux DR3's ext2 root and swap on
# Apple_UNIX_SVR2 partitions 6 and 7, booted through the MkLinux Booter to a
# root login on the **6100, 7100, 8100 and 7500** — both PowerPC generations,
# NuBus and PCI, from the same bytes.
#
# ONE IMAGE, FOUR MACHINES.  That is the whole claim, and it is why this runs
# on every commit rather than nightly: nothing on the disk is machine-specific,
# so a row that breaks is the machine model changing under us, not the media.
# The three NuBus rows are matched against a single shared `mklinux-login.png`
# — the console DR3 prints is identical on all of them.
#
# WHY 7.6 AND NOT 7.5.  The Mac OS side is not incidental here; it is the boot
# path, since MkLinux never starts the machine itself.  A 7500 rejects a 7.5
# startup volume outright with DSErrCode $66 (dsOldSystem) — "This startup disk
# will not work on this Macintosh model" — before the Booter is ever reached.
# 7.6 boots all four.  The 7.5 image is still published and still reproduced by
# `pdm-mklinux-disk` / `pdm-mklinux-install`; it is simply not the one that can
# cover this whole spread.
#
# THE ROWS ASSERT THE DISK, NOT ONLY A PICTURE.  Every row is corroborated by
# a structural check that a screenshot cannot fake — the machine has left the
# 68k address space, and (on the rows that log in) `sync` moves bytes inside
# the ext2 root at 42303488.  This is not academic: an earlier version of this
# suite went green against goldens that were mid-boot frames, because REGEN
# captured whatever was on screen when a row was failing.
#
# TWO ROMS.  The NuBus rows use TEST_ROM below; the 7500 row loads the TNT ROM
# by path, the same fixture `tnt-hd-boot` uses.

TEST_NAME := Power Macintosh — boot MkLinux DR3 from a single Mac OS + MkLinux disk
TEST_DESC := Boot one 169 MB disk holding Mac OS 7.6 and MkLinux DR3 through the MkLinux Booter to a root login, on all four PowerPC models

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).  The
# pm7500 row loads roms/pm7500-pm8500-pm9500-96cd923d.rom itself.
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom
TEST_ARGS := model=pm7100 ram=24576
TEST_TIER := matrix

# MEDIA: `systems/mklinux_dr3_76_single_169mb.img.7z` in gs-test-data since
# revision 9e807e1, extracted in place by scripts/fetch-test-data.sh.  Unlike
# the disk/install tests this one needs nothing from local/, so it runs
# wherever the pinned test corpus is present.
