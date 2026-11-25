# Lua 5.4.7 (vendored)

The Lua interpreter and its standard library, unmodified, from
<https://www.lua.org/ftp/lua-5.4.7.tar.gz> (`src/` less `lua.c`, `luac.c`
and the upstream Makefile).  MIT license: see `LICENSE` here and
`THIRD_PARTY_NOTICES.md` at the repository root.

Both builds compile every `.c` in this directory (`src/sources.mk`,
`LUA_SRC`).  The emulator's bridge to it is `src/core/lua/`
(`docs/internals/core/lua/lua.md`).  To upgrade, replace the files with the
next release's `src/` and update the version here and in the notices.
