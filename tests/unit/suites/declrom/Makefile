# Declaration-ROM builder unit test (runtime-vrom proposal stage B).
# Links the real declrom.c + the embedded gsvrom blobs; generates a
# JMFB-shaped image with the builder (splicing the blob's own exec
# blocks) and structurally diffs its directory walk against the
# assembled blob, plus exercises declrom_image_validate's guards.

TEST_NAME    := declrom
TEST_SRCS    := test.c
TEST_HARNESS := isolated
EXTRA_SRCS   := ../../../../src/core/peripherals/nubus/declrom.c \
                ../../../../src/core/peripherals/nubus/gsvrom_data.c

include ../../common.mk
