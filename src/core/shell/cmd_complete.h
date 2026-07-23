// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.h
// Tab completion engine for the typed object-model shell.

#pragma once

#ifndef CMD_COMPLETE_H
#define CMD_COMPLETE_H

// Tab completion maximum items.  Sized for the typed-tree root, which
// has ~70 root methods plus ~12 attached child objects (cpu, memory,
// scsi, floppy, mouse, keyboard, screen, vfs, find, debugger, …).  The
// cap exists so the JSON-encoded completion buffer (4 KiB) doesn't
// overflow; bumping past ~250 risks that.
#define CMD_MAX_COMPLETIONS 200

// Completion result: a fixed-capacity list of borrowed candidate
// strings (they point at static class-member names or a per-call
// string pool inside the completer).
struct completion {
    const char *items[CMD_MAX_COMPLETIONS];
    int count;
};

// Run tab completion for the given line at cursor_pos.
// Fills out->items with matching completions.
void shell_complete(const char *line, int cursor_pos, struct completion *out);

#endif // CMD_COMPLETE_H
