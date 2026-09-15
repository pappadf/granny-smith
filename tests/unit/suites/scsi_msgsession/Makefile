# scsi_msgsession.c — the MESSAGE OUT/IN conversation MESH and the 53C8xx
# SCRIPTS engine both need, and both used to implement separately.
#
# Pure logic with no emulator dependencies, so the real parser links directly.
# It had NO direct test on either side before: the only live negotiation in the
# whole corpus is 29 SDTR exchanges inside one AIX boot.
TEST_NAME := scsi_msgsession
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/peripherals/scsi_msgsession.c
include ../../common.mk
