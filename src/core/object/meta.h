// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// meta.h
// The `meta` class. Every node carries an implicit `meta` attribute
// whose value is a synthetic introspection node — itself an ordinary
// object-model node. Schema queries become regular `object_eval` calls:
//
//     machine.cpu.meta.class       -> "cpu"
//     machine.cpu.meta.path        -> "machine.cpu"
//     machine.cpu.meta.children    -> [...sub-object names...]
//     machine.cpu.meta.attributes  -> [...attribute names...]
//     machine.cpu.meta.methods     -> [...method names...]
//     meta.complete(line, c)       -> tab-completion candidates
//     meta.member("pc")            -> short description of the named member

#ifndef GS_OBJECT_META_H
#define GS_OBJECT_META_H

#include <stddef.h>

#include "object.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Singleton meta class. Useful for `object_class(o) == meta_class()`
// recognition (the path printer needs it to print `<inspected>.meta`).
const class_desc_t *meta_class(void);

// Return the synthetic Meta node bound to `inspected`. Lazily creates
// the node and caches it on the inspected object's `meta_node` slot.
// Returns NULL on alloc failure or if `inspected` is NULL.
struct object *meta_node_for(struct object *inspected);

// Free the cached Meta node on `inspected`, if any. Called from
// object_delete so a meta cache cannot outlive its inspected node.
void meta_node_release(struct object *inspected);

// Provider for `meta.complete(line, cursor)`. The shell module
// registers this at init time with a wrapper around shell_complete.
// When no provider is registered (e.g. unit tests that don't link the
// shell), the method returns an empty list.
typedef value_t (*meta_complete_fn)(const char *line, int cursor);
void meta_complete_register(meta_complete_fn fn);

// Type descriptor {kind, width, presentation, enum} for a slot: what
// meta.members exports for attributes, arguments and results.
value_t meta_type_descriptor(value_kind_t kind, uint8_t width, uint16_t presentation, const char *const *enum_values);
const char *meta_presentation_text(uint16_t flags); // "hex", "path", … or NULL

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_META_H
