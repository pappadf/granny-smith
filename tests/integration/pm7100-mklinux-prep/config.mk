# Integration test configuration: Power Macintosh 7100 — prepare the two disks
# MkLinux DR3 installs from and onto.
#
# Phase 1 of the MkLinux bring-up (local/gs-docs/projects/mklinux/bring-up-plan.md
# §5.1).  It builds both halves of the §3 disk design and publishes them:
#
#   pm7100-mklinux-prep     the 7.5 host volume + the partitioned target  [here]
#   pm7100-mklinux-install  the DR3 installer onto that target            [next]
#   pm7100-mklinux-boot     boot the installed system to a login prompt   [last]
#
# TWO DISKS, and Mac OS never touches the MkLinux one (plan §3):
#
#   SCSI 0   system_7_5_0_77mb_mode32_24ac.img   the host we boot (read/write)
#   SCSI 1   $(WORK_DIR)/mklinux-target.img      HD1000SC, the MkLinux target
#   SCSI 3   MkLinux-DR3.iso                     the DR3 disc
#
# The host volume gets the five Mac Files the DR3 README tells a user to copy
# by hand; the target gets an Apple partition map holding only `root` and
# `swap`, written by the CD's own Mac OS `pdisk`.  Nothing machine-specific
# ever lands on the target — that is what makes it reusable on a future
# pm7500 (plan §8).
#
# WHY pdisk AND NOT THE INSTALLER'S OWN EDIT SCREEN (plan §3 route (a)):
# DR3's installer cannot partition a disk that has never been partitioned.
# `install/hd.c`'s partitionDrives() leaves its `cmd` pointer uninitialised
# when a disk carries neither an fdisk signature nor an 'ER' block zero, and
# execs it — see notes/dr3-guest-quirks.md §1.  Route (b), the CD's Mac OS
# pdisk, is the recorded fallback and it works.
#
# MEDIA: everything lives under local/gs-docs/projects/mklinux/ and is not in
# gs-test-data (plan §5 — promotion deferred until all three tests are green),
# so this test SKIPS cleanly where it is absent and CI stays green.
#
# ⚠️ The 7.5 host volume is attached read/write and accumulates delta writes.
# Under the harness those land in the per-test GS_STORAGE_CACHE and tests/data
# is left untouched; driving this by hand from the repo root without setting
# that variable writes through to the shared image.

TEST_NAME := Power Macintosh 7100 — prepare the MkLinux DR3 host volume and target disk
TEST_DESC := Boot 7.5 with the DR3 CD, partition a blank HD1000SC with the CD's pdisk, copy the five Mac Files into the System Folder, and publish both disks

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 40 MB.  The plan asks for 32 MB; the 7100 profile's RAM ladder is
# 8/16/24/40/72/136 MB, so 40 is the first rung at or above it — comfortably
# past DR3's 16 MB floor and inside the era's well-tested range.
TEST_ARGS := model=pm7100 ram=40960

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
