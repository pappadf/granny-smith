# Integration test configuration: IIcx + Display Card 24AC — Finder "Restart"
#
# Regression guard for the warm-restart hang: on a IIcx with the 24AC card that
# booted the 32-bit SCSI cdev image, picking Special ▸ Restart used to wedge
# with distorted VRAM.  The Mac restart runs the ROM warm-boot path (mask IRQs →
# 68k RESET → set up MMU → jump to the boot entry), and the RESET instruction is
# meant to reset the external peripherals via the bus /RESET line.  That
# instruction was a no-op, so the SCSI controller and the 24AC card carried
# stale OS-session state into the reboot — garbage boot-screen draw and a
# write_mr phase assertion.  Fixed by modelling the /RESET line (OP_RESET →
# system_reset_devices → scsi_reset_pin + nubus_reset → card_reset).
#
# The boot half is identical to iicx-24ac (same card, same 8-bpp 640x480 mode,
# same pre-built 20 MB System 7.1 SCSI HD with the 24AC cdev + 32-bit
# addressing).  This test then drives the real Special ▸ Restart menu command
# and asserts the machine reboots all the way back to the 8-bpp colour Finder
# desktop — which is what a healthy restart does.
#
# The image is stored in gs-test-data as systems/system_7_1_20mb_24ac_cd_32bit_gc.img.7z
# and auto-extracted to systems/system_7_1_20mb_24ac_cd_32bit_gc.img by
# scripts/fetch-test-data.sh (no TEST_SETUP needed — the .7z extraction is generic).

TEST_NAME := IIcx Display Card 24AC — Finder Restart
TEST_DESC := Boot IIcx + 24AC from the 32-bit SCSI cdev image, pick Special > Restart, and confirm it reboots back to the Finder desktop

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 24AC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
