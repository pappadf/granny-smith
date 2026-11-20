// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// storage_util.h
// Filesystem, string and JSON helpers the storage code shares.  Each of these
// used to exist two to four times across image.c, checkpoint_machine.c,
// archive.c, storage_class.c, image_scratch.c and tools/dump, with differing
// edge cases.  src/peeler keeps its own
// copies so it stays buildable on its own.

#ifndef GS_STORAGE_UTIL_H
#define GS_STORAGE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// strdup that tolerates NULL (returns NULL).
char *gs_strdup(const char *s);

// A freshly malloc'd, formatted string; NULL on OOM or a format error.
char *str_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// mkdir -p: create `dir` and any missing parents.  0 on success or when it
// already exists; a negative errno otherwise.
int mkdir_p(const char *dir);

// Create the directories leading to the file `path` (not the file).  0 or a
// negative errno.
int mkdir_parents(const char *path);

// Remove `path`: a file, a symlink, or a directory tree.  A symlink -- at the
// top or anywhere below -- is removed, never followed.  0 when the path is
// gone afterwards (including when it never existed), else a negative errno.
int rm_tree(const char *path);

// A file replaced whole or not at all.  file_replace_open writes to a sibling,
// `path` plus `tmp_suffix` (".tmp" when NULL); file_replace_commit closes it and,
// when `ok` and every write and the close succeeded, renames it over `path` --
// otherwise it removes it.  A crash, a short write or a full disk leaves the
// old file, never part of the new one.  The suffix is the caller's to choose
// where "<path>.tmp" could be another file's name.
typedef struct {
    FILE *f;
    char *path;
    char *tmp;
} file_replace_t;

FILE *file_replace_open(file_replace_t *a, const char *path, const char *tmp_suffix); // NULL on failure, errno set
int file_replace_commit(file_replace_t *a, bool ok); // 0 or a negative errno

// The whole of `data` to `path`, replaced atomically.  0 or a negative errno.
int file_write_atomic(const char *path, const void *data, size_t len);

// The one host-file opener for reading, under source_host, the host VFS
// backend and read_file: a read-only, close-on-exec descriptor on a regular
// file, or a negative errno (-EISDIR for a directory, -EINVAL for a FIFO,
// socket or device).  The open is non-blocking, so a FIFO with no writer is
// refused instead of hanging the caller; the descriptor it returns blocks as
// usual.  `size` and `mtime` (either may be NULL) receive the file's length
// and modification time.
int open_regular_file(const char *path, uint64_t *size, int64_t *mtime);

// Read all of `path` into a malloc'd buffer (*out; a non-NULL buffer even for
// an empty file) of *out_len bytes.  A file larger than `cap` is refused
// with -EFBIG before anything is allocated: every caller states what it will
// accept.  0 or a negative errno.
int read_file(const char *path, size_t cap, uint8_t **out, size_t *out_len);

// The same for text: *out is NUL-terminated (the terminator not counted in
// *out_len, which may be NULL).  Same cap and errors.
int read_text_file(const char *path, size_t cap, char **out, size_t *out_len);

// Fill `out` with `n` unpredictable bytes: getentropy, else /dev/urandom,
// else a pid/time/counter mix that is at least unique within the process
// (ids and UUIDs that must not collide, not keys).  Never fails.
void random_bytes(void *out, size_t n);

// True when `path` ends in `ext` (".dmg"), ignoring case: the rule for
// media and output file names a user types.
bool path_has_ext(const char *path, const char *ext);

// True when `s` ends in exactly `suffix`: the rule for internal file names
// the emulator itself writes (".tmp", ".delta", ".rsrc").
bool path_has_suffix(const char *s, const char *suffix);

// `dir` and `leaf` joined with exactly one '/', however many trailing
// slashes `dir` and leading slashes `leaf` have (a lone "/" stays the root),
// into `out`.  False when it does not fit `cap` -- a truncated path names a
// different file, so a caller must not use it.
bool path_join(char *out, size_t cap, const char *dir, const char *leaf);

// JSON-escape `src` (the body of a string literal, no quotes) into `dst`
// with the one escape set (json_escape.h): quote, backslash, \b \f \n \r
// \t, and any other control byte as \u00XX.
// Returns the escaped length, or -EINVAL if it does not fit in `cap` with
// its NUL.
int json_escape(const char *src, char *dst, size_t cap);

// The same into a malloc'd string ("" for NULL); NULL on OOM.
char *json_escape_dup(const char *src);

// Write `s` to `fp` as a quoted JSON string literal (NULL writes ""): the
// form for tools and manifest writers.
void json_fputs(FILE *fp, const char *s);

#endif // GS_STORAGE_UTIL_H
