# Floppy GCR codec, zone geometry and register-index decode.
#
# The subsystem's most testable parts are pure functions and had no coverage at
# all (02-floppy F-30): the GCR encode/decode round trip, the rotate-add-xor
# checksum chain, the Sony zone geometry, and the address->register decode each
# board applies.  Every duplication finding in that report (F-18, F-19, F-20)
# is cheap to make safe with a round-trip test and dangerous without one, and
# several correctness findings (F-07, F-31, F-33) are exactly what a table test
# catches.
#
# floppy_gcr.c is #included rather than linked so the test can reach its static
# encode_sector / decode_sector / decode_gcr.  gcr_stubs.c supplies the image
# and log symbols that belong to subsystems this isolated suite does not link.

TEST_NAME    := floppy
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := gcr_stubs.c
EXTRA_CFLAGS  = -I$(EMU_ROOT)/core/peripherals -I$(EMU_ROOT)/core/storage

include ../../common.mk
