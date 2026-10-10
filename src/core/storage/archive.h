// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.h
// Archive file handling: identification (`.sit` / `.cpt` / `.zip` / `.tar` /
// `.hqx` / `.bin` / `.gz`) and extraction, and the `files.archive` object.
// An archive is also a namespace of the VFS (namespace.h): `ls app.sit`
// lists it, `cp app.sit/Readme .` copies one file out.

#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdbool.h>

struct class_desc;

// Identify an archive at `path` (any VFS path: it may be inside an image or
// another archive).  Returns the format short name ("sit" / "cpt" / "zip" /
// "tar" / "hqx" / "bin" / "gz") for a recognised file, or NULL when the file is
// unreadable or not a supported archive.  The name is a static string of the
// format registry (peel_identify), valid for the lifetime of the program.
// Reads at most 64 KiB from each end of the file.
const char *archive_identify_file(const char *path);

// Extract the archive at `path` into `out_dir` (defaults to "." when
// NULL/empty): its tree copied out, each file's resource fork and Finder
// info in an AppleDouble "._" sidecar.  Creates the output directory if it
// doesn't exist.  Returns 0 on success, -ECANCELED when cancelled, non-zero
// on other failure.
int archive_extract_file(const char *path, const char *out_dir);

// === Object-model class descriptor =========================================
//
// `files.archive` is a process-singleton node created with `files` by
// core_init. It exposes `identify` and `extract` methods.  `parent` is the
// node it is attached under (`files`).

struct object;
void archive_init(struct object *parent);
void archive_delete(void);

#endif // ARCHIVE_H
