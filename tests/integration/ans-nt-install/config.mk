# Integration test: installing Windows NT 4.0 on an Apple Network Server 500
#
# The whole installation, from a cold machine to NT's logon screen, the way
# a person does it: three two-line Open Firmware commands off the
# powermac-nt-hal boot floppy (setup.of, boot.of, bootdisk.of), every
# Setup screen answered on the serial console or the ADB keyboard, and
# nothing poked, patched or restored from a checkpoint in between.
#
# FIXTURE-GATED.  None of the media can be committed: the 2.26NT ROM, the
# NT CD, the floppy (five of its files are Microsoft's) and the Product ID
# all belong to whoever runs it.  The row SKIPS cleanly where they are
# absent; test.script's header lists what goes where.
#
# TEST_ROM is only here to satisfy the runner, which insists on a ROM from
# tests/data: the row boots the 2.26NT ROM from the fixture directory
# explicitly, because the production 1.1.22 ROM cannot run NT at all.

TEST_NAME := ANS Windows NT install
TEST_DESC := Installs Windows NT 4.0 PowerPC on an emulated Network Server 500 from the powermac-nt-hal boot floppy, text-mode and GUI Setup, and boots the result to the logon screen

TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# Turns the fixture directory's product-id.txt, when there is one, into
# $(WORK_DIR)/nt-pid.script: the script language cannot index a string, so
# the ID is typed from a generated list of key presses.
TEST_SETUP := sh ans-nt-install/nt-pid.sh ../../local/gs-docs/projects/windows-nt-ppc-ans/product-id.txt $(WORK_DIR)/nt-pid.script

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
