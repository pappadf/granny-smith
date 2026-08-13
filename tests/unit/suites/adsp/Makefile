TEST_NAME := adsp
TEST_SRCS := test.c stub_host.c
TEST_HARNESS := isolated
# The ADSP engine plus the object-model substrate its observability surface
# is built on.  DDP and the scheduler are stubbed (stub_host.c): the suite
# runs two engine instances back to back over an in-process loopback.
EXTRA_SRCS := ../../../../src/core/network/appletalk_adsp.c \
              ../../../../src/core/object/value.c \
              ../../../../src/core/object/object.c \
              ../../../../src/core/object/meta.c
include ../../common.mk
