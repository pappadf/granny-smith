# byte_fifo.h — the shared ring behind the 53C96's and MESH's SCSI FIFOs.
#
# Header-only, so there is nothing to link: this exists because the ring itself
# had no direct test.  Both chips only ever exercised it through their register
# interfaces, which cannot distinguish a correct ring from one whose wrap errors
# happen to cancel out over a whole transfer.
TEST_NAME := byte_fifo
TEST_SRCS := test.c
TEST_HARNESS := isolated
include ../../common.mk
