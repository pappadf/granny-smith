# Integration test: Apple Lisa 2 — full SCO Xenix 3.0 Operating System install.
#
# Drives the ENTIRE install from the "XENIX Installation Guide for the Apple
# Lisa 2" (May 1984), §1.5.1 - §1.5.4:
#   1.5.1  boot the "Boot XProFile Patch" floppy to the <BootFloppy> shell;
#   1.5.2  run `hdinit` (y / 5) -> mkfs + fsck + boot block on the blank ProFile;
#   1.5.3  reboot, boot the hard disk kernel with `pf(0,0)xenix`;
#   1.5.4  `firsttime` -> "/usr on second profile? n", "install OS? y", then copy
#          all seven "Xenix 3.0" distribution floppies (1-7), answer "n" to the
#          Software Development and Text Processing distributions, and finish at
#          "Xenix Installation complete. / ** Normal System Shutdown **".
#
# This is the comprehensive Xenix test (the lisa-xenix-hdinit test covers only the
# fast hard-disk-init phase).  It exercises the whole stack: the segment-MMU
# stack-segment limit, the ProFile 6-byte pf handshake, the 68000 data-bus-error
# saved-PC stack-growth-probe fix, a guest reboot (an inheriting machine.boot with
# a persisted ProFile image), booting the kernel from the hard disk, and seven
# tar-from-floppy distribution reads with eject/insert disk swaps.
#
# Heavy by design (~1.1B instructions): a full multi-disk OS install.  The Xenix
# media is gitignored proprietary data, staged by a maintainer for CI under
# Lisa/Xenix-3.0/.

TEST_NAME := Apple Lisa 2 SCO Xenix 3.0 full install
TEST_DESC := Full Xenix 3.0 OS install onto the ProFile (hdinit, reboot, firsttime, 7 floppies), then save the cleanly-shut-down image

TEST_ROM := roms/lisa2-revh-098917b2.rom

TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-Boot-XProFile.dc42
