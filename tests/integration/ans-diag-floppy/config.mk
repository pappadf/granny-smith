# Integration test: the Network Server boots Apple's diagnostic floppy
# (proposal-apple-network-server-500-700; the floppy datapath).
#
# The Apple Network Server has one boot path that is neither SCSI nor the
# network: the front keyswitch in its Service position makes Open
# Firmware boot its `diag-device` — `cd disk6 fd:diags`, the internal
# floppy — which is how a technician runs the *Network Server Diagnostic
# Utility* (a bootable 1440 KB HFS floppy carrying an XCOFF `diags`,
# version 1.1 of 08/28/96).  This row is that path end to end: the
# firmware's own `swim3` package opens the chip, senses the drive and the
# media, seeks, and reads whole tracks through DBDMA channel 1 into the
# XCOFF loader; the utility then paints its Level One menu on the 54M30
# and takes its commands from the ADB keyboard.
#
# The firmware package drives the SWIM3 unlike the Mac OS `.Sony` driver
# (sense on Handshake bit 3, head select without a sense read, whole-track
# continuous transfers), so this is the row that pins the shared SWIM3
# model's behaviour under a second driver — see
# docs/core/peripherals/swim3.md and docs/machines/tnt/tnt.md.
#
# The floppy image is Apple software and cannot live in this repository;
# it is in the private gs-test-data (systems/ans_diagnostic_utility_1_1.img.7z,
# 110 KB) and the row skips cleanly when it has not been fetched.

TEST_NAME := ANS diagnostic floppy
TEST_DESC := Boots the Network Server Diagnostic Utility 1.1 from the internal floppy with the keyswitch in Service, and drives its menu from the ADB keyboard

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
