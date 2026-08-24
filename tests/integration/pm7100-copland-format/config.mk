# Integration test configuration: Power Macintosh 7100 — prepare the target
# volume Copland D11E4 installs onto.
#
# The install half of the Copland bring-up
# (local/gs-docs/projects/copland/bring-up-plan.md §6.2).  It builds the volume
# the companion boot test consumes: a 230 MB Apple-branded drive, formatted
# with Drive Setup 2.0d5c2 off the DDK 0.4 CD, carrying a fresh System 7.5.0
# install and Copland's own `Install Mac OS` on top of it.
#
# PHASES 1-2: this test now produces the FORMATTED, EMPTY target volume.
#
#   1. the machine boots the prepared 7.5 image with the blank target drive and
#      the DDK CD on the same bus, and lands on the Finder desktop      [here]
#   2. format the target with Drive Setup 2.0d5c2 off the CD            [here]
#      and publish it to media/copland_target_formatted_230mb.img       [here]
#   3. install System 7.5.0 onto it from the seven floppies         [pm7100-
#   4. run the CD's `Install Mac OS` onto the same volume            copland-
#   5. export the volume and hand it to pm7100-copland-boot          install]
#
# Phases 3-5 live in a SEPARATE test that starts from the published formatted
# image.  The split is the same reasoning as the plan's §6.4 split between
# install and boot: formatting is slow, deterministic and almost never changes,
# while the two installers are what gets iterated on.  Paying ~5.6 G
# instructions of boot-and-format before every install attempt is not a cost
# worth repeating.
#
# Deliberately NOT a floppy boot (plan §6.2): the 7.5 Disk Tools floppy carries
# no CD-ROM extensions, so Drive Setup 2.0d5c2 — which lives on the DDK CD —
# would be unreachable from it.  Apple explicitly allows installing from any
# 7.5.x volume, so the test boots the prepared image the PDM suite already uses
# and drives both installers from there.  One machine.boot, two installers.
#
# MEDIA: the Copland media lives in local/gs-docs/projects/copland/ and is not
# in gs-test-data (plan §6.0 — promotion deferred until both tests are green),
# so this test runs only where that directory exists and SKIPS cleanly
# everywhere else.  CI stays green without it.
#
# ⚠️ The 7.5 host volume is attached read/write and accumulates delta writes.
# Under the harness those land in the per-test GS_STORAGE_CACHE and tests/data
# is left untouched; driving this by hand from the repo root without setting
# that variable writes through to the shared image.

TEST_NAME := Power Macintosh 7100 — format Copland's target volume (Drive Setup 2.0d5c2)
TEST_DESC := Boot a pm7100 from the prepared 7.5 volume and initialize a blank Apple 230 MB drive with Drive Setup 2.0d5c2 off the Mac OS 8 DDK 0.4 CD, writing the Apple_MacOSPrep loader partition Copland's installer requires

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 24 MB: inside Copland's 16-32 MB window (plan §2.2).  The script re-boots the
# machine itself so it can pin the RTC first.
TEST_ARGS := model=pm7100 ram=24576

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
