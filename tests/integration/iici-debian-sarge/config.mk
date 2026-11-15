# Integration test configuration: Debian 3.1 "sarge" (Linux 2.2.25) on the IIci.
#
# Two disks from gs-test-data's systems/, both ULMO (xz) UDIFs read in place:
#   SCSI 0  system_7_1_20mb_24ac_cd_32bit_penguin_sarge.dmg -- Mac OS 7.1 with
#           Penguin 19 and the kernel; Penguin's saved settings sit in Startup
#           Items, so the machine goes from Mac OS to Linux with no input
#   SCSI 1  debian_3_1_512mb_iici.dmg -- the root filesystem, /dev/sdb2 (ext2)
#
# Linux needs the 68030 MMU (PTEST levels, MMUSR), bus-error retry, the 5380,
# ADB and the Display Card 8*24's CLUT to be right; built-in RBV video is
# switched off because Linux 2.2 cannot keep its frame buffer at physical 0.

TEST_NAME := IIci Debian 3.1 sarge -- Mac OS 7.1 -> Penguin -> Linux 2.2 login and X11
TEST_DESC := Boot the IIci (32 MB, 8*24 card) from Mac OS 7.1 through Penguin's auto-boot into Debian sarge; log in on the console and start X (twm, xterm, xclock).

TEST_ROM := roms/iici-368cadfe.rom
TEST_ARGS := model=iici ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
