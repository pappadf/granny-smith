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

// Run a Lua file to completion on the calling stack -- not as a job: it
// cannot suspend, so a mode it starts is waited for with the platform's
// frame runner where there is one (job.h).  A job runs a file with dofile,
// which can suspend.  Relative `require` paths inside it resolve against the
// file's directory.  Returns 0 when the chunk ends, 1 when it called quit(),
// -1 when it failed (the error has been reported).
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

// The debugger's text in the shell spelling (`$name`, `&&`, `${expr:08x}`),
// compiled to Lua once and run non-yieldably.  A breakpoint condition is
// checked when it is set (VK_NONE, or VK_ERROR "invalid breakpoint condition
// '...': why") and evaluated at each hit (an error counts as true).  A
// template (a logpoint's message) is expanded with `n` named bindings ahead
// of the globals; a failing `${}` reads as its message.
struct value;
struct value gs_lua_condition_check(const char *text);
bool gs_lua_condition_eval(const char *text);
struct value gs_lua_expand(const char *text, int n, const char *const *names, const struct value *vals);

// The terminal's two forms (gs_lua_lib.lua, gs.shell_line): whether `text`
// is one command line (`files.ls /tmp`, `machine.cpu.d0 = 0x10`) rather than
// a Lua chunk -- nothing is evaluated to decide -- and whether it is a Lua
// chunk that is not finished yet (Enter breaks the line).
bool gs_lua_is_command_line(const char *text);
bool gs_lua_needs_continuation(const char *text);

// Run one terminal line on the calling stack -- not as a job -- with the
// terminal's printing (shell.run).  0 ok, -1 failed (reported).
int gs_lua_run_line(const char *text);

// The `vars` table as a list of 'name=value' strings (shell.vars).
struct value gs_lua_vars(void);

// Release the state (tests; the platforms never need to).
void gs_lua_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // GS_LUA_H
