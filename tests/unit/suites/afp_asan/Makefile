# The afp suite again, under AddressSanitizer (10-network unit 0.3).
#
# The AFP server keeps views into arrays that grow by realloc (the desktop
# icon store, the CNID catalog).  Whether a stale view reads freed memory
# that still holds the old bytes depends on the allocator, so a plain run
# can pass by luck where ASan fails every time (10-network F-03).  Native
# only: the wasm32 tier runs the plain suite.

TEST_NAME := afp_asan
TEST_SRCS := ../afp/test.c ../afp/stub_atalk.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/network/appletalk_server.c \
              ../../../../src/core/network/afp_volume.c \
              ../../../../src/core/network/afp_params.c \
              ../../../../src/core/network/afp_enum.c \
              ../../../../src/core/network/afp_catalog.c \
              ../../../../src/core/network/afp_desktop.c \
              ../../../../src/core/network/afp_fork.c \
              ../../../../src/core/network/afp_meta.c \
              ../../../../src/peeler/lib/appledouble.c
EXTRA_CFLAGS := -fsanitize=address -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address
include ../../common.mk
