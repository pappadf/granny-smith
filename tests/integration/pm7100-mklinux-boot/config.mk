# Integration test configuration: Power Macintosh 7100 — boot MkLinux DR3.
#
# The keeper test, and the last phase of the MkLinux bring-up (bring-up plan
# §5.3).  It starts from the pair `pm7100-mklinux-install` publishes: a 7.5
# host volume whose lilo.conf names the installed root and whose MkLinux
# control panel defaults to MkLinux, and the target carrying the installed
# system.
#
# Row-per-milestone rather than one linear script, so partial progress is
# visible instead of one red light.  `macos-control` is not optional: a boot
# that clicks "Boot MacOS" must still reach the 7.5 Finder, because without
# that control "MkLinux broke" and "the disk broke" look identical.
#
# ONE DISK.  System 7.5 on Apple_HFS partition 5, MkLinux's ext2 root on
# Apple_UNIX_SVR2 partition 6, swap on 7 — 169 MB total, 89 MiB of it
# non-zero.  Drive Setup 1.5 is willing to leave part of a drive Unallocated,
# and pdisk fills that with the MkLinux partitions; the recipe is in
# notes/single-disk-recipe.md.
#
# THE MODEL ROWS ASSERT THE DISK, NOT A PICTURE.  A golden is written by REGEN
# and matched by the verify run, so a row that fails while its golden is being
# captured records its own failure as the reference and reports green forever.
# The pm6100/pm8100 rows therefore type `sync` and require the root filesystem
# to have changed — and that is not academic: the pm8100's console stops
# repainting after the Mach banner, so it LOOKS hung while it is in fact
# running a shell.

TEST_NAME := Power Macintosh — boot MkLinux DR3 from a single Mac OS + MkLinux disk
TEST_DESC := Boot one 169 MB disk holding System 7.5 and MkLinux DR3 through the MkLinux Booter to a root login, on all three PowerPC models

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom
TEST_ARGS := model=pm7100 ram=40960
TEST_TIER := extended

# MEDIA: `systems/mklinux_dr3_single_169mb.img.7z` in gs-test-data since
# revision 2590a822, extracted in place by scripts/fetch-test-data.sh.  Unlike
# the prep/install tests this one needs nothing from local/, so it runs
# wherever the pinned test corpus is present.
