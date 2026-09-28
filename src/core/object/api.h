// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// api.h
// Public C entry points for the object model — used by both the
// headless shell's `eval` command and the WASM/JS bridge.

#ifndef GS_OBJECT_API_H
#define GS_OBJECT_API_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolve `path` against the root and write a JSON-encoded value into
// `out_buf` (NUL-terminated).  A result larger than out_size is handed to
// the spill hook below when one is set (the mailbox stages it in a buffer
// the client reads: mailbox.h), else replaced by an error.
// `args_json` is the method-argument list as a JSON array; it may be
// NULL or "[]" for argument-less calls and attribute reads. When the
// path is an attribute and `args_json` carries exactly one value, the
// call is a setter. Returns 0 on success, negative on error (the error
// message is also serialised into `out_buf`).
//
// Introspection and completion are reached through the same call:
// `gs_eval("cpu.meta")`, `gs_eval("cpu.meta.attributes")`, and
// `gs_eval("meta.complete", "[\"cpu.d\", 5]")` replace the former
// gs_inspect / gs_complete entry points.
int gs_eval(const char *path, const char *args_json, char *out_buf, size_t out_size);

// The spill hook: given the value and the answer slot, writes a document
// naming where the full result lies and returns true, or false to refuse.
struct value;
typedef bool (*gs_eval_spill_fn)(const struct value *v, char *out, size_t out_size);
void gs_eval_set_spill_hook(gs_eval_spill_fn fn);
// Formats `v` as JSON into a malloc'd buffer of at most `max` bytes; the
// length, or 0 (and *out NULL) when it does not fit.
size_t gs_format_value_json_alloc(const struct value *v, char **out, size_t max);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_API_H
