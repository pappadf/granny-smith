# Integration test configuration: Power Macintosh 7100 — install Mac OS 8
# D11E4 "Spaz" (Copland) onto the target volume.
#
# Phase 4-5 of the Copland bring-up (bring-up plan §6.2).  It starts from the
# volume `pm7100-copland-install` publishes — Drive-Setup-2.0d5c2-formatted
# with System 7.5.0 already on it — so it never re-pays the ~92 G instructions
# that install cost:
#
#   pm7100-copland-format       blank HD230SC -> Drive Setup 2.0d5c2 ->
#                               Apple_MacOSPrep + Apple_HFS "untitled"  [done]
#   pm7100-copland-install      System 7.5.0 onto it from seven floppies [done]
#   pm7100-copland-install-os8  the DDK CD's `Install Mac OS`            [here]
#   pm7100-copland-boot         latch Caps Lock and boot it              [todo]
#
# The whole point of the two tests before this one is the four installer rules
# that gate this one (plan §5): 200 `'mach'` (a NuBus PDM), 202 16 MB of RAM,
# 204 a 230 MB volume, 207 a `SecondaryLoader` / `Apple_MacOSPrep` partition
# that only Drive Setup 2.0d5c2 writes, and 210 a System 7.5.0-or-later on the
# target.  Rule 207 is the one that defeats SheepShaver.
#
# MEDIA: everything is under local/gs-docs/projects/copland/ and deliberately
# not in gs-test-data yet (plan §6.0), so the test SKIPS cleanly where it is
# absent and CI stays green.

TEST_NAME := Power Macintosh 7100 — install Mac OS 8 D11E4 (Copland) onto the target volume
TEST_DESC := Boot the prepared 7.5 host and run the DDK CD's Install Mac OS onto the 7.5 volume, then publish it

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 24 MB: inside Copland's 16-32 MB window (plan §2.2), and past rule 202.
TEST_ARGS := model=pm7100 ram=24576

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
