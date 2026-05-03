// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// peeler_shell.h
// Integration of peeler library into granny-smith shell for archive extraction.

#ifndef PEELER_SHELL_H
#define PEELER_SHELL_H

// Extract a single archive (`path`) into `out_dir` (defaults to "."
// if NULL/empty). Used by the typed `peeler` root method.
// Returns 0 on success, non-zero on failure (file not found, format
// unsupported, write failure).
int peeler_shell_extract(const char *path, const char *out_dir);

#endif // PEELER_SHELL_H
