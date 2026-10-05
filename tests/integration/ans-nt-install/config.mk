# Integration test: installing Windows NT 4.0 on an Apple Network Server 500
#
# The whole installation, from a cold machine to NT's logon screen, the way
# a person does it: three two-line Open Firmware commands off the
# powermac-nt-hal boot floppy (setup.of, boot.of, bootdisk.of), every
# Setup screen answered on the serial console or the ADB keyboard, and
# nothing poked, patched or restored from a checkpoint in between.
#
# MEDIA-GATED.  The 2.26NT ROM and the boot floppy are in the test data; the
# NT CD, a blank install disk and the Product ID are not redistributable and
# come from $GS_EXTRA_MEDIA_DIR.  The row SKIPS cleanly where any of them is
# absent; test.script's header lists what goes where.
#
# TEST_ROM is the production ROM only because the runner insists on one that
# exists at every data revision: the row boots the 2.26NT ROM itself, because
# the production 1.1.22 ROM cannot run NT at all.

TEST_NAME := ANS Windows NT install
TEST_DESC := Installs Windows NT 4.0 PowerPC on an emulated Network Server 500 from the powermac-nt-hal boot floppy, text-mode and GUI Setup, and boots the result to the logon screen

TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# Turns $GS_EXTRA_MEDIA_DIR/windows-nt-ppc-ans/product-id.txt, when there is
# one, into $(WORK_DIR)/nt-pid.script: the script language cannot index a
# string, so the ID is typed from a generated list of key presses.
# ($EXTRA_MEDIA is the runner's own variable; TEST_SETUP is eval'd there.)
TEST_SETUP := sh ans-nt-install/nt-pid.sh "$EXTRA_MEDIA/windows-nt-ppc-ans/product-id.txt" $(WORK_DIR)/nt-pid.script

# The whole installation is ~10 minutes on a fast host; the runner's default
# 900 s leaves a CI runner no margin.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
