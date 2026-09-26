TEST_NAME := afp
TEST_SRCS := test.c stub_atalk.c
TEST_HARNESS := isolated
# A small listing limit (the product's is 65535), so the test that reaches it
# makes 1,024 files rather than 65,536 (it stays above 600, which another
# test lists in full).
EXTRA_CFLAGS := -DAFP_MAX_ENUM_ENTRIES=1023u
# The AFP server plus the four persistence/state modules it sits on. The
# transport (appletalk.c) is stubbed in stub_atalk.c so the suite can drive
# afp_handle_command() directly, with no guest and no wire.
EXTRA_SRCS := ../../../../src/core/network/appletalk_server.c \
              ../../../../src/core/network/afp_volume.c \
              ../../../../src/core/network/afp_params.c \
              ../../../../src/core/network/afp_enum.c \
              ../../../../src/core/crc32.c \
              ../../../../src/core/storage/storage_util.c \
              ../../../../src/core/network/afp_applog.c \
              ../../../../src/core/network/afp_catalog.c \
              ../../../../src/core/network/afp_desktop.c \
              ../../../../src/core/network/afp_fork.c \
              ../../../../src/core/network/afp_meta.c \
              ../../../../src/core/storage/macroman.c \
              ../../../../src/peeler/lib/appledouble.c
include ../../common.mk
