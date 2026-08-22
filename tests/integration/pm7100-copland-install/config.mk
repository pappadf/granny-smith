# Integration test configuration: Power Macintosh 7100 — install System 7.5.0
# onto the volume Copland D11E4 will be installed into.
#
# Phase 3 of the Copland bring-up (bring-up plan §6.2).  It starts from the
# formatted, empty volume `pm7100-copland-format` publishes, so it never pays
# for the boot-and-format again:
#
#   pm7100-copland-format   blank HD230SC -> Drive Setup 2.0d5c2 -> Apple_MacOSPrep
#                           + Apple_HFS "untitled"                     [done]
#   pm7100-copland-install  install System 7.5.0 onto it from the seven
#                           floppies                                   [here]
#                           then run the CD's `Install Mac OS`          [todo]
#                           and publish the volume                     [todo]
#   pm7100-copland-boot     latch Caps Lock and boot it                [todo]
#
# WHY NOT A FLOPPY BOOT: the machine boots the prepared 7.5 image the PDM suite
# already uses and runs the Installer off the floppy from there.  Apple is
# explicit that it does not matter which volume you install FROM, and booting
# the Disk Tools floppy would put the DDK CD's Drive Setup 2.0d5c2 out of reach
# (no CD-ROM extensions on that floppy) — which is what phase 2 needed.
#
# THE DESTINATION IS THE POINT.  The Installer defaults to the volume it booted
# from ("Macintosh HD"); the test clicks Switch Disk twice to land on
# "untitled", and every subsequent golden shows "Installing onto the disk
# `untitled`".  Getting this wrong would install 7.5 over the host image and
# silently produce a Copland target with no System at all.
#
# MEDIA: everything is under local/gs-docs/projects/copland/ and deliberately
# not in gs-test-data yet (plan §6.0), so the test SKIPS cleanly where it is
# absent and CI stays green.
#
# RUNTIME: long.  The boot is ~1.6 G instructions, the Installer's "Preparing
# to install" phase alone is ~2.5 G, and then it reads seven floppies.
# `extended` tier because it produces an artifact, not because it is quick.

TEST_NAME := Power Macintosh 7100 — install System 7.5.0 onto Copland's target volume
TEST_DESC := Boot the prepared 7.5 host, run the System 7.5 Installer off the floppies onto the Drive-Setup-2.0d5c2 volume, and publish the result

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 24 MB: inside Copland's 16-32 MB window (plan §2.2).
TEST_ARGS := model=pm7100 ram=24576

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
