# The REPL's printing of objects (core/shell/shell.c): an object's
# attribute table reads every attribute through object_attr_get, so the
# member's presentation_flags (VFLAG_HEX, VFLAG_SENSITIVE) reach the
# printed text the way they reach node_get.  test.c captures the output
# sink (out_route) and stubs the shell's other collaborators.

TEST_NAME    := shell_print
TEST_SRCS    := test.c
TEST_HARNESS := isolated
OMIT_STUBS   := shell
EXTRA_SRCS   := ../../../../src/core/shell/shell.c \
                ../../../../src/core/job/seam.c \
                ../../../../src/core/object/object.c \
                ../../../../src/core/object/value.c \
                ../../../../src/core/object/meta.c \
                ../../../../src/core/object/parse.c \
                ../../../../src/core/object/value_format.c
include ../../common.mk
