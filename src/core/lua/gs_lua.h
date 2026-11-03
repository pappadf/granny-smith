// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// gs_lua.h -- Lua as the script language: one Lua 5.4 state per process,
// the object tree reachable from it through node proxies, and the runners
// the platforms and the shell call (docs/internals/core/lua/lua.md).
//
// A script sees the tree's root children as globals (`machine`,
// `scheduler`, `debug`, ...).  Reading `machine.cpu.pc` is node_get, writing
// it is node_set, calling `machine.boot{model = "plus"}` is node_call with
// the trailing table as the named arguments.  A VK_ERROR from any of them is
// raised as a Lua error (a table with a `text` field) that pcall catches.
#ifndef GS_LUA_H
#define GS_LUA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Run a Lua file to completion on the calling thread (the job thread when
// there is one; the tree accesses go through the seam as every leaf call
// does).  Relative `require` paths inside it resolve against the file's
// directory.  Returns 0 when the chunk ends, 1 when it called quit(), -1
// when it failed (the error has been reported).
int gs_lua_run_file(const char *path);

// Run Lua source text.  With `interactive` the text is first tried as an
// expression whose results are printed, as the REPL prints a bare path.
// Same return values as gs_lua_run_file.
int gs_lua_run_source(const char *src, bool interactive);

// Define `vars.NAME = value` (the --var table a script reads).
void gs_lua_var_set(const char *name, const char *value);

// Ctrl-C: the running chunk unwinds at its next instruction-count hook.
void gs_lua_interrupt(void);

// Where a failed chunk's report goes: `text` is the report as printed
// (newline-terminated; the message and a traceback).  The default prints to
// stderr; the shell installs script_report_error so a job's client gets it
// as an error annotation.
typedef void (*gs_lua_reporter_fn)(const char *file, int line, const char *msg, const char *text);
void gs_lua_set_reporter(gs_lua_reporter_fn fn);

// Release the state (tests; the platforms never need to).
void gs_lua_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // GS_LUA_H
