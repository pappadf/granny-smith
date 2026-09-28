# Integration test: booting an installed Windows NT 4.0 on the Apple Network Server
#
# The disk ans-nt-install produces, from cold: the 2.26NT ROM, the
# powermac-nt-hal boot floppy's setup.of and bootdisk.of, NT's kernel on
# that project's HAL, the logon screen, and a logon from the ADB keyboard.
# See test.script for the rungs.
#
# THE MEDIA IS IN gs-test-data, not local/: the 2.26NT ROM
# (roms/ans500-ans700-2.26nt-962f6c13-50348b3d0126096b.rom), the installed
# disk (systems/winnt_4_0_ppc_ans_512mb.img.7z) and the floppy it was
# installed from (systems/ans_nt_boot_floppy_daea7d6.img.7z), all from
# gs-test-data 9b20396 onward.  At an older pin the row SKIPS.
#
# TEST_ROM is the production ROM only because the runner insists on a ROM
# that exists at every pin: the row boots the 2.26NT ROM itself, and skips
# when that ROM is absent too.

TEST_NAME := ANS Windows NT installed boot
TEST_DESC := Cold-boots an installed Windows NT 4.0 PowerPC disk on the Network Server 500 through the powermac-nt-hal floppy's bootdisk.of to the logon screen, and logs on as Administrator from the ADB keyboard

TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
