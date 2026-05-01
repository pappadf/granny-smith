// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_api.h
// Public C entry points for the object model — used by both the
// headless shell's `eval` command and (later) the WASM/JS bridge. See
// proposal-module-object-model.md §7.
//
// In M2 these are skeletons that exercise the path resolver against
// the root tree. Full method dispatch and JSON argument decoding land
// in subsequent milestones.

#ifndef GS_OBJECT_GS_API_H
#define GS_OBJECT_GS_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve `path` against the root and write a JSON-encoded value into
// `out_buf` (NUL-terminated, truncated if larger than out_size). For
// M2: read-only; `args_json` is reserved for method arguments and may
// be NULL or "[]". Returns 0 on success, negative on error (the error
// message is also serialised into `out_buf`).
int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size);

// Read a subtree's current values into JSON. M2 returns the same shape
// as gs_eval (single value at `path`); the recursive subtree form
// arrives with the inspector UI in M11.
int gs_inspect(const char *path, char *out_buf, size_t out_size);

// Path completion — returns a JSON array of candidate completions for
// `partial`. M2: shell-level completion remains the legacy
// `cmd_complete` engine (per the M1/M2 plan). This entry point is a
// placeholder that returns an empty array so the JS bridge has the
// symbol to call.
int gs_complete(const char *partial, char *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_GS_API_H
