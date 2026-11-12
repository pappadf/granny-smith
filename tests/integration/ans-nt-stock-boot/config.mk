# Integration test: booting an installed Windows NT 4.0 on the Apple Network Server
# with the production ROM (Open Firmware 1.1.22)
#
# The disk ans-nt-install produces, from cold, on the ROM every Network
# Server shipped with: the powermac-nt-hal boot floppy's setup.of and
# bootdisk.of, NT's kernel on that project's HAL, the logon screen, and a
# logon from the ADB keyboard.  ans-nt-installed-boot is the same on the
# 2.26NT ROM; see test.script for why both exist.
#
# THE MEDIA IS IN gs-test-data: the installed disk
# (systems/winnt_4_0_ppc_ans_512mb_hal5663262.img.7z, with powermac-nt-hal
# 5663262's HAL) and the floppy (systems/ans_nt_boot_floppy_5663262.img.7z,
# nothing of Microsoft's on it), from gs-test-data bc9af37 onward.  At an
# older pin the row SKIPS.

TEST_NAME := ANS Windows NT installed boot, production ROM
TEST_DESC := Cold-boots an installed Windows NT 4.0 PowerPC disk on the Network Server 500 on the production 1.1.22 ROM through the powermac-nt-hal floppy's bootdisk.of to the logon screen, and logs on as Administrator from the ADB keyboard

TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# A cold boot to the desktop is a few minutes on a fast host; room for a
# slower CI runner.
TEST_TIMEOUT := 1800

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
