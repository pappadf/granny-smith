# New Age (µPD72070, Apple mode) floppy controller unit test.
#
# Links the real core/peripherals/new_age.c against a fake drive (the
# floppy.h accessors), a fake image and a fake scheduler, and checks the
# contracts the shipped AV driver depends on: the three-phase handshake and
# its MSR bits, invalid-command and nothing-pending ST0 = $80, ST3 for a
# present and a missing drive, background seeks reported through Sense
# Interrupt Status, the /CSTIN poller (suspended while a drive is enabled),
# RQM low through a data command's execution phase, and terminal count
# ending a Read Data normally where running out to EOT ends it abnormally.

TEST_NAME    := new_age
TEST_SRCS    := test.c
TEST_HARNESS := none
EXTRA_SRCS   := ../../../../src/core/peripherals/new_age.c
EXTRA_LDFLAGS := -lm
include ../../common.mk
