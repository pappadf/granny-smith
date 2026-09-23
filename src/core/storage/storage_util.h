// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_util.h
// Filesystem, string and JSON helpers the storage code shares.  Each of these
// used to exist two to four times across image.c, checkpoint_machine.c,
// archive.c, storage_class.c, image_scratch.c and tools/dump, with differing
// edge cases (09-storage F-56, F-57, F-63, F-36).  src/peeler keeps its own
// copies so it stays buildable on its own.

#ifndef GS_STORAGE_UTIL_H
#define GS_STORAGE_UTIL_H

#include <stddef.h>
#include <stdint.h>

// strdup that tolerates NULL (returns NULL).
char *gs_strdup(const char *s);

// A freshly malloc'd, formatted string; NULL on OOM or a format error.
char *gs_str_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// mkdir -p: create `dir` and any missing parents.  0 on success or when it
// already exists; a negative errno otherwise.
int gs_mkdir_p(const char *dir);

// Create the directories leading to the file `path` (not the file).  0 or a
// negative errno.
int gs_mkdir_parents(const char *path);

// Remove `path`: a file, a symlink, or a directory tree.  A symlink -- at the
// top or anywhere below -- is removed, never followed.  0 when the path is
// gone afterwards (including when it never existed), else a negative errno.
int gs_rm_tree(const char *path);

// Read all of `path` into a malloc'd buffer (*out; a non-NULL buffer even for
// an empty file) of *out_len bytes.  A file larger than `cap` is refused
// with -EFBIG before anything is allocated: every caller states what it will
// accept.  0 or a negative errno.
int gs_read_file(const char *path, size_t cap, uint8_t **out, size_t *out_len);

// JSON-escape `src` (the body of a string literal, no quotes) into `dst`:
// quote, backslash, \b \f \n \r \t, and any other control byte as \u00XX.
// Returns the escaped length, or -EINVAL if it does not fit in `cap` with
// its NUL.
int gs_json_escape(const char *src, char *dst, size_t cap);

// The same into a malloc'd string ("" for NULL); NULL on OOM.
char *gs_json_escape_dup(const char *src);

#endif // GS_STORAGE_UTIL_H
