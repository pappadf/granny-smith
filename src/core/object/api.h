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
//
// Introspection and completion are reached through the same call:
// `gs_eval("cpu.meta")`, `gs_eval("cpu.meta.attributes")`, and
// `gs_eval("meta.complete", "[\"cpu.d\", 5]")` replace the former
// gs_inspect / gs_complete entry points. See
// proposal-introspection-via-meta-attribute.md.
int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_API_H
