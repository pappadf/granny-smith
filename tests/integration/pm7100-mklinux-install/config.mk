# Integration test configuration: Power Macintosh 7100 — install MkLinux DR3.
#
# Phase 2 of the MkLinux bring-up (bring-up plan §5.2).  It starts from the two
# disks `pm7100-mklinux-prep` publishes, so it never re-pays for the CD mount,
# the pdisk session or the five Finder copies:
#
#   pm7100-mklinux-prep     7.5 host + five Mac Files; target with root+swap  [done]
#   pm7100-mklinux-install  the Booter hands off to Mach_Kernel; the Red Hat-
#                           derived installer runs to "Done"; lilo.conf is
#                           pointed at the installed root                     [here]
#   pm7100-mklinux-boot     boot it to a login prompt                         [next]
#
# THE BRING-UP CLIFF IS THE FIRST RUNG.  Everything before the Booter's
# "Boot MkLinux" button is Mac OS on proven emulation; the moment it jumps into
# Mach_Kernel this machine is running its third kernel (after 7.5 and Copland's
# NuKernel).  One emulator defect stood in the way and is fixed —
# notes/bat-context-sync.md: a BAT register write must not steer instruction
# fetch until the processor context-synchronizes, and DR3's start.s writes a
# BAT pair whose half-written state relocates the kernel's own text off the end
# of RAM.
#
# KEYBOARD.  DR3's ADB driver reads the arrow cluster at ADB $3B-$3E and treats
# $7B-$7E as the right-hand modifiers, so the installer rows type the raw
# keycodes rather than the "up"/"down" names (notes/dr3-guest-quirks.md §2).
#
# RUNTIME: long.  The Mach boot is ~1.3 G instructions from the splash click,
# the installer's own screens are cheap, and the package install dominates at
# ~13 G.  `extended` tier because it produces an artifact, not because it is
# quick.
#
# MEDIA: under local/gs-docs/projects/mklinux/, deliberately not in
# gs-test-data yet (plan §5), so the test SKIPS cleanly where it is absent.

TEST_NAME := Power Macintosh 7100 — install MkLinux DR3 onto the prepared target
TEST_DESC := Boot the Booter into Mach_Kernel, run the DR3 installer onto /dev/sdb2, point lilo.conf at it, and publish both disks

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 40 MB — the 7100 RAM ladder's first rung at or above the plan's 32 MB.
TEST_ARGS := model=pm7100 ram=40960

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
