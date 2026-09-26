# NCR 53C96 chip-model unit test.
# Links scsi_53c96.c against a mock bus (the scsi_external_* / scsi_pop /
# scsi_push / scsi_get_bus_phase functions defined in test.c) plus a small
# RAM disk, so the select → CDB → pseudo-DMA read → status/message flow is
# exercised deterministically without the full scsi.c/image.c stack.

TEST_NAME    := scsi96
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := shell memory debugger peripherals assert
EXTRA_SRCS   := ../../../../src/core/peripherals/scsi_53c96.c
include ../../common.mk
