// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.h
// Mac archive file handling: identification (`.sit` / `.cpt` / `.hqx` /
// `.bin` / `.sea`) and extraction. The implementation wraps the
// third-party "peeler" library — that name is an implementation detail;
// the user-facing concept is "Mac archive", so the typed object surface
// is `archive.*`.

#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdbool.h>

struct class_desc;

// Identify a Mac archive at `path`.
// Returns the format short name ("sit" / "cpt" / "hqx" / "bin" / "sea")
// for a recognised file, or NULL when the file is unreadable or not a
// supported archive. Returned pointer is owned by the peeler library
// and is valid for the lifetime of the program.
const char *archive_identify_file(const char *path);

// Extract the archive at `path` into `out_dir` (defaults to "." when
// NULL/empty). Creates the output directory if it doesn't exist.
// Returns 0 on success, non-zero on failure.
int archive_extract_file(const char *path, const char *out_dir);

// === Object-model class descriptor =========================================
//
// `archive` is a process-singleton namespace registered at shell_init
// alongside rom / vrom / machine / checkpoint. It exposes `identify`
// and `extract` methods.

extern const struct class_desc archive_class;

void archive_init(void);
void archive_delete(void);

#endif // ARCHIVE_H
