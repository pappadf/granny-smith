# The ImageWriter's PAP server (appletalk_imagewriter.c) alone: ATP, NBP,
# the clock and the printer are stubbed in test.c.
TEST_NAME := pap_imagewriter
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/network/appletalk_imagewriter.c \
              ../../../../src/core/network/appletalk_nbp_service.c \
              ../../../../src/core/network/pap_server.c \
              ../../../../src/core/storage/macroman.c
include ../../common.mk
