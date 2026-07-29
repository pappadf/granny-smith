# RE-HOSTED IIcx -> IIci (§7): this is the NCR 5380 pseudo-DMA regression guard,
# and on the IIci that controller sits behind the MDU/RBV board rather than a
# second GLUE machine, so the same guard covers a different bus path. It also
# takes the IIci x 6.0.8 cell, which nothing else covered.
#
# The move was blocked for a while and the reason is recorded in git: the IIcx
# version drove the whole HD SC Setup choreography on fixed instruction budgets
# tuned to that host, and on the IIci the first of them landed on "Welcome to
# Macintosh" instead of the desktop — after which every click acted on the
# splash and every golden captured it, passing while verifying nothing. So this
# is a re-authoring, not a model= change: every budget is now a condition
# (§5.2), and the coordinates were re-derived from measured frames on this host.
#
# Integration test configuration: IIci + built-in RBV video — format a blank HD
# under System 6.0.8.
#
# Boots a Macintosh IIci from the SSW 6.0.8 800K boot floppy (Disk2of4, volume
# "Utilities"), launches Apple HD SC Setup v2.0.3 off that floppy, and formats a
# freshly-created blank HD20SC SCSI disk at ID 0 — all driven by mouse clicks,
# matched pixel-exact at each milestone.
#
# This is the regression guard for the NCR 5380 BLIND-vs-DRQ primer-gate fix
# (scsi.c write_uint8 / scsi.h SCSI_BLIND_SEL): before the fix the primer-slot
# gate ran on Mac OS's DRQ pseudo-DMA writes too and dropped the leading $00 of
# every zero-filled block, so HD SC Setup's HFS volume-init wrote only the MDB
# and one bitmap block, then aborted ("unable to mount volume"). The gate now
# applies to the BLIND window only (A/UX's CLR.B primer), so Mac OS's DRQ writes
# land intact and the format completes.

TEST_NAME := IIci Format Blank HD (built-in RBV, System 6.0.8)
TEST_DESC := Boot IIci from SSW 6.0.8 floppy, run Apple HD SC Setup, format a blank HD20SC over the MDU's 5380

# IIci ROM (checksum 0x368CADFE). Video is the machine's built-in RBV, so no
# NuBus declrom is discovered or needed.
TEST_ROM := roms/iici-368cadfe.rom

# 8 MB: a IIci does not complete POST below that (proposal-emulator-bug-fixes.md
# §3), so this is the floor rather than a choice.
TEST_ARGS := model=iici ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
