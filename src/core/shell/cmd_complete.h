// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.h
// Tab completion engine for the typed object-model shell.

#pragma once

#ifndef CMD_COMPLETE_H
#define CMD_COMPLETE_H

#include <stdbool.h>
#include <stdint.h>

// Sanity bound on the candidates of one completion.  The item table grows on
// demand; this only stops a pathological source (a directory of tens of
// thousands of entries) from building a menu nobody can read -- past it
// the result says `truncated`.
#define CMD_MAX_COMPLETIONS 4096

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

// Completion result: a growable list of candidate strings (each points at a
// static class-member name or at a string the completion owns), plus the
// half-open [start, end) span of line text each candidate replaces. `end` is
// the cursor; `start` is the current word's first character — except for
// filesystem-path candidates, which are bare entry names and replace only
// the basename after the word's last '/'.
struct completion {
    const char **items;
    // Detail per item: kind and one-line doc (static strings).
    uint8_t *kinds;
    const char **docs;
    int count;
    int cap; // allocated length of items / kinds / docs
    // Composed candidate strings (indexed children, `name.`, `name=`, file
    // names) this completion owns; completion_free releases them.
    char **owned;
    int n_owned;
    int cap_owned;
    int start;
    int end;
    // The detail the next pushed candidates get (set by the completer).
    comp_kind_t cur_kind;
    const char *cur_doc;
    // Argument context: set when the cursor is in an argument position of a
    // resolved method.  arg_index is the DECLARED slot (a `name=` argument
    // names its own slot).
    bool has_context;
    char ctx_method[256];
    int ctx_arg_index;
    const char *ctx_arg_name;
    // Set when candidates were dropped: CMD_MAX_COMPLETIONS was reached,
    // a candidate did not fit a buffer, or memory ran out.
    bool truncated;
};

// Lifetime: everything `items` and `docs` point at lives until
// completion_free (or the next shell_complete on the same struct).  Each
// struct completion has its own storage, so two completions never share it.

// Run tab completion for the given line at cursor_pos into `out`, which must
// be zero-initialised or hold an earlier result (emptied first).  Release it
// with completion_free.
void shell_complete(const char *line, int cursor_pos, struct completion *out);

// Release what a completion holds and leave it empty (zeroed).
void completion_free(struct completion *c);

#endif // CMD_COMPLETE_H
