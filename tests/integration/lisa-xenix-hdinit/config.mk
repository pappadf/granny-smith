# Integration test: Apple Lisa 2 — SCO Xenix 3.0 hard-disk initialization (hdinit).
#
# Follows the "XENIX Installation Guide for the Apple Lisa 2" (May 1984),
# sections 1.5.1 ("Starting XENIX From a Floppy Disk") and 1.5.2 ("Initializing
# the Hard Disk"): boot the Xenix "Boot XProFile Patch" floppy with a blank
# ProFile attached, reach the <BootFloppy> single-user shell, then run `hdinit`,
# answer y / 5, and let it build the root filesystem, fsck it, and write the boot
# block — ending at "Disk initialization complete. / Normal System Shutdown".
#
# This exercises the full Xenix bring-up stack and the three fixes that make it
# run on our emulator:
#   (a) the segment-MMU stack-segment limit (top page of a limit-0 stack valid),
#       without which the Xenix boot loader's self-relocation faults;
#   (b) the ProFile parallel handshake driven by the XProFile-patched kernel's
#       6-byte pf command (the plain Boot floppy sends a truncated 4-byte command);
#   (c) the 68000 group-0 data bus-error saved PC = faulting-instruction + 2, so
#       the kernel recognises mkfs's `TST.B d16(A7)` stack-growth probe and grows
#       the stack instead of killing the process ("Memory fault - core dumped").
#
# The Xenix media is gitignored proprietary data, staged by a maintainer for CI
# under Lisa/Xenix-3.0/.

TEST_NAME := Apple Lisa 2 SCO Xenix 3.0 hdinit
TEST_DESC := Boot Xenix and run hdinit to initialize the ProFile (mkfs + fsck + boot block)

TEST_ROM := roms/lisa2-revh-098917b2.rom

# Lisa 2, 2 MB.  Boot from the Xenix "Boot XProFile Patch" floppy (the plain Boot
# floppy's pf driver cannot drive an external ProFile).
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-Boot-XProFile.dc42
