// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// meta.h
// The `Meta` class. Every node carries an implicit `meta` attribute
// whose value is a synthetic introspection node — itself an ordinary
// object-model node. Schema queries become regular `gs_eval` calls:
//
//     cpu.meta.class           -> "Cpu"
//     cpu.meta.path            -> "cpu"
//     cpu.meta.children        -> ["d0", "d1", ...]
//     cpu.meta.attributes      -> [...attribute names...]
//     cpu.meta.methods         -> [...method names...]
//     meta.complete(line, c)   -> tab-completion candidates
//     meta.member("pc")        -> short description of the named member
//
// See local/gs-docs/proposals/proposal-introspection-via-meta-attribute.md.

#ifndef GS_OBJECT_META_H
#define GS_OBJECT_META_H

#include <stddef.h>

#include "object.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Singleton Meta class. Useful for `object_class(o) == meta_class()`
// recognition (the path printer needs it to print `<inspected>.meta`).
const class_desc_t *meta_class(void);

// Return the synthetic Meta node bound to `inspected`. Lazily creates
// the node and caches it on the inspected object's `meta_node` slot.
// Returns NULL on alloc failure or if `inspected` is NULL.
struct object *meta_node_for(struct object *inspected);

// Free the cached Meta node on `inspected`, if any. Called from
// object_delete so a meta cache cannot outlive its inspected node.
void meta_node_release(struct object *inspected);

// Compute the dotted path of `obj` (e.g. `"cpu"`, `"cpu.meta"`,
// `"floppy.drives.0"`). The root produces an empty string. Meta nodes
// recurse into their inspected target and append `.meta`. Output is
// NUL-terminated and truncated to `buf_size - 1`.
void object_compute_path(struct object *obj, char *buf, size_t buf_size);

// Provider for `meta.complete(line, cursor)`. The shell module
// registers this at init time with a wrapper around shell_tab_complete.
// When no provider is registered (e.g. unit tests that don't link the
// shell), the method returns an empty list.
typedef value_t (*meta_complete_fn)(const char *line, int cursor);
void meta_set_complete_provider(meta_complete_fn fn);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_META_H
