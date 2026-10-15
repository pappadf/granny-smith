TEST_NAME := lua_bridge
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS = ../../../../src/core/job/seam.c \
              ../../../../src/core/object/value.c \
              ../../../../src/core/object/object.c \
              ../../../../src/core/object/meta.c \
              ../../../../src/core/object/parse.c \
              ../../../../src/core/object/value_format.c \
              ../../../../src/core/lua/gs_lua.c \
              ../../../../src/core/platform_hooks.c \
              $(LUA_SRC)
include ../../common.mk
include ../../../../src/core/lua/gs_lua.mk

# The bridge compiles against the vendored Lua and embeds the generated
# library header (gs_lua.mk).
CFLAGS += $(LUA_INCLUDES) $(LUA_CFLAGS) -I$(GS_LUA_OUT)
$(OBJ_DIR)/src/core/lua/gs_lua.o: $(GS_LUA_LIB_HEADER)
