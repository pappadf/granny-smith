# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# The embedded Lua standard library for scripts (gs_lua_lib.lua), turned
# into a C byte array by scripts/bin2c.py, as the LaserWriter prelude is.
# Included by both builds and by the lua_bridge unit suite; every path is
# absolute so the rule runs from any directory.

GS_LUA_SAVED_GOAL := $(.DEFAULT_GOAL)

GS_LUA_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
GS_LUA_ROOT := $(abspath $(GS_LUA_MK_DIR)/../../..)
GS_LUA_OUT := $(GS_LUA_ROOT)/build/gs_lua
GS_LUA_LIB_HEADER := $(GS_LUA_OUT)/gs_lua_lib.h

$(GS_LUA_LIB_HEADER): $(GS_LUA_MK_DIR)/gs_lua_lib.lua $(GS_LUA_ROOT)/scripts/bin2c.py
	@mkdir -p $(GS_LUA_OUT)
	python3 $(GS_LUA_ROOT)/scripts/bin2c.py --out $@ --guard GS_LUA_LIB_H \
	  gs_lua_lib_lua=$(GS_LUA_MK_DIR)/gs_lua_lib.lua

.DEFAULT_GOAL := $(GS_LUA_SAVED_GOAL)
