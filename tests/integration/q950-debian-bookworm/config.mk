# Integration test configuration: Debian 12-era m68k (Linux 6.1) with XFCE on
# the Quadra 950.
#
# Two disks from gs-test-data's systems/, both ULMO (xz) UDIFs read in place:
#   SCSI 0  system_7_1_20mb_24ac_cd_32bit_penguin_bookworm.dmg -- Mac OS 7.1
#           with Penguin 19, vmlinux-6.1 and its initrd; Penguin's saved
#           settings sit in Startup Items, so Linux boots with no input
#   SCSI 1  debian_12_2gb_q950_xfce.dmg -- the root filesystem, /dev/sdb2 (ext4)
#
# What it leans on: the 68040 MMU and its fault/retry paths, the predecoder's
# fault handling, the FPU's (An)+/-(An) rollback (xfce4-terminal died in
# libfontconfig without it), the 53C96 DMA select, the SCC as a console, DAFB
# at thousands.  The disk carries a libgio fix for GLib's own m68k g_once ABI
# bug (without it xfdesktop dies under qemu-user too).

TEST_NAME := Quadra 950 Debian 12 m68k -- Mac OS 7.1 -> Penguin -> Linux 6.1, systemd, XFCE
TEST_DESC := Boot the Quadra 950 (256 MB, DAFB thousands) through Penguin's auto-boot into Debian 12-era m68k; check systemd, xfdesktop and xfce4-terminal over the serial console and the XFCE desktop on screen.

TEST_ROM := roms/q950-3dc27823.rom
TEST_ARGS := model=q950 ram=262144

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
TEST_TIMEOUT := 1800
