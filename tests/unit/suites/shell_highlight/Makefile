TEST_NAME := shell_highlight
TEST_SRCS := test.c
TEST_HARNESS := isolated
# The classification (command line or Lua chunk) is the script engine's own
# (gs.classify), so the suite links the bridge and the vendored Lua.
EXTRA_SRCS = ../../../../src/core/job/seam.c \
              ../../../../src/core/object/value.c \
              ../../../../src/core/object/object.c \
              ../../../../src/core/object/parse.c \
              ../../../../src/core/object/meta.c \
              ../../../../src/core/object/alias.c \
              ../../../../src/core/object/value_format.c \
              ../../../../src/core/vbuf.c \
              ../../../../src/core/lua/gs_lua.c \
              ../../../../src/core/platform_hooks.c \
              ../../../../src/core/shell/highlight.c \
              ../../../../src/core/shell/commands.c \
              ../../../../src/core/shell/syntax.c \
              $(LUA_SRC)
include ../../common.mk
include ../../../../src/core/lua/gs_lua.mk

CFLAGS += $(LUA_INCLUDES) $(LUA_CFLAGS) -I$(GS_LUA_OUT)
$(OBJ_DIR)/src/core/lua/gs_lua.o: $(GS_LUA_LIB_HEADER)
