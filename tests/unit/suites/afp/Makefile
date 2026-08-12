TEST_NAME := afp
TEST_SRCS := test.c stub_atalk.c
TEST_HARNESS := isolated
# The AFP server plus the four persistence/state modules it sits on. The
# transport (appletalk.c) is stubbed in stub_atalk.c so the suite can drive
# afp_handle_command() directly, with no guest and no wire.
EXTRA_SRCS := ../../../../src/core/network/appletalk_server.c \
              ../../../../src/core/network/afp_catalog.c \
              ../../../../src/core/network/afp_desktop.c \
              ../../../../src/core/network/afp_fork.c \
              ../../../../src/core/network/afp_meta.c \
              ../../../../src/core/storage/appledouble.c
include ../../common.mk
