// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// api.h
// Public C entry points for the object model — used by both the
// headless shell's `eval` command and the WASM/JS bridge.

#ifndef GS_OBJECT_API_H
#define GS_OBJECT_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve `path` against the root and write a JSON-encoded value into
// `out_buf` (NUL-terminated, truncated if larger than out_size).
// `args_json` is the method-argument list as a JSON array; it may be
// NULL or "[]" for argument-less calls and attribute reads. When the
// path is an attribute and `args_json` carries exactly one value, the
// call is a setter. Returns 0 on success, negative on error (the error
// message is also serialised into `out_buf`).
int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size);

// Read a subtree's current values into JSON. Currently the same shape
// as gs_eval (single value at `path`); the recursive subtree form is
// reserved for the inspector UI.
int gs_inspect(const char *path, char *out_buf, size_t out_size);

// Path completion — returns a JSON array of candidate completions for
// `partial`. Currently a placeholder that returns an empty array;
// shell-level completion runs through the cmd_complete engine.
int gs_complete(const char *partial, char *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_API_H
