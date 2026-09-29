// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.h
// Tab completion engine for the typed object-model shell.

#pragma once

#ifndef CMD_COMPLETE_H
#define CMD_COMPLETE_H

#include <stdbool.h>
#include <stdint.h>

// Tab completion maximum items.  Sized for the typed-tree root, which
// has ~70 root methods plus ~12 attached child objects (cpu, memory,
// scsi, floppy, mouse, keyboard, screen, vfs, find, debugger, …).  The
// cap exists so the JSON-encoded completion buffer (4 KiB) doesn't
// overflow; bumping past ~250 risks that.
#define CMD_MAX_COMPLETIONS 200

// Completion result: a fixed-capacity list of borrowed candidate
// strings (they point at static class-member names or a per-call
// string pool inside the completer), plus the half-open [start, end)
// span of line text each candidate replaces. `end` is the cursor;
// `start` is the current word's first character — except for
// filesystem-path candidates, which are bare entry names and replace
// only the basename after the word's last '/'.
// What a candidate is (shell.complete's detail `kind`).
typedef enum {
    COMP_KIND_VALUE = 0, // an argument value: enum / bool / file path
    COMP_KIND_OBJECT,
    COMP_KIND_COLLECTION,
    COMP_KIND_ATTR, // an attribute, or an argument name (`name=`)
    COMP_KIND_METHOD,
    COMP_KIND_ALIAS, // a `$name` binding or alias
    COMP_KIND_KEYWORD,
} comp_kind_t;

// Text of a comp_kind_t ("value", "object", …).
const char *comp_kind_name(comp_kind_t k);

struct completion {
    const char *items[CMD_MAX_COMPLETIONS];
    // Detail per item (borrowed like items): kind, one-line doc, task id.
    uint8_t kinds[CMD_MAX_COMPLETIONS];
    const char *docs[CMD_MAX_COMPLETIONS];
    const char *tasks[CMD_MAX_COMPLETIONS];
    int count;
    int start;
    int end;
    // The detail the next pushed candidates get (set by the completer).
    comp_kind_t cur_kind;
    const char *cur_doc;
    const char *cur_task;
    // Argument context: set when the cursor is in an argument position of a
    // resolved method.  arg_index is the DECLARED slot (a `name=` argument
    // names its own slot).
    bool has_context;
    char ctx_method[256];
    int ctx_arg_index;
    const char *ctx_arg_name;
    // Set when candidates were DROPPED -- the per-call string pool filled, or
    // the item table did.  Without it a short list was indistinguishable from
    // a complete one, so a class with many long member names silently lost
    // completions past the pool's 2 KB with no indication anywhere.
    bool truncated;
};

// LIFETIME, which was previously only true by luck: `items` may point into a
// per-call pool inside the completer, so the pointers are invalidated by the
// NEXT shell_complete() call.  A caller that needs them beyond that must copy
// immediately -- shell_meta_complete_provider does, which is the only reason
// the terminal and meta.complete can both work today, and nothing here said
// so.

// Run tab completion for the given line at cursor_pos.
// Fills out->items with matching completions.
void shell_complete(const char *line, int cursor_pos, struct completion *out);

#endif // CMD_COMPLETE_H
