# The shell binding store (shell_var.c), under AddressSanitizer: a binding
# that kept a raw pointer to a destroyed object reads freed memory, which a
# plain run only notices by luck.
TEST_NAME := shell_var
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/job/seam.c \
              ../../../../src/core/object/value.c \
              ../../../../src/core/object/object.c \
              ../../../../src/core/object/parse.c \
              ../../../../src/core/object/meta.c \
              ../../../../src/core/object/alias.c \
              ../../../../src/core/shell/shell_var.c
SANITIZE := -fsanitize=address -fno-omit-frame-pointer
include ../../common.mk
