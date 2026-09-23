// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// peeler.h
// Public API for libpeeler — a C99 library for peeling apart classic
// Macintosh archive formats.

#ifndef PEELER_H
#define PEELER_H

#ifdef __cplusplus
extern "C" {
#endif

// === Includes ===

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === Error Handling ===

// Opaque error object.  NULL means no error.
typedef struct peel_err peel_err_t;

// Return the human-readable error message, or a generic fallback for NULL.
const char *peel_err_msg(const peel_err_t *err);

// Free an error object.  Safe to call with NULL.
void peel_err_free(peel_err_t *err);

// === Byte Buffer ===

// A contiguous byte buffer with explicit ownership tracking.
// Created by the library, freed by the caller via peel_free().
typedef struct {
    uint8_t *data; // Pointer to contents (NULL when size == 0)
    size_t size; // Number of valid bytes
    bool owned; // If true, peel_free() will release data
} peel_buf_t;

// Free the data inside a buffer (if owned) and zero the struct.
void peel_free(peel_buf_t *buf);

// === File Metadata ===

// Metadata for a single file extracted from an archive.
// Fields are best-effort; zeroed when the format does not provide them.
typedef struct {
    // Relative path of the entry, '/' between folder levels -- or just the
    // file's name, for single-file formats.  Each component is a Mac name made
    // safe to write under a directory: a '/' inside a name (legal on HFS)
    // becomes ':', the macOS convention, so it cannot create a separator;
    // a component of only dots ('.', '..', legal Mac names) gets a '_' prefix;
    // an empty one becomes '_'.  So peeler's own names always pass
    // peel_path_is_confined -- but check anyway before writing (below).
    char name[256];
    uint32_t mac_type; // Classic Mac file type  (e.g. 'TEXT')
    uint32_t mac_creator; // Classic Mac creator    (e.g. 'ttxt')
    uint16_t finder_flags; // Finder flags
} peel_file_meta_t;

// === Extracted File ===

// A single file with both forks.  Unused forks have size == 0.
typedef struct {
    peel_file_meta_t meta;
    peel_buf_t data_fork;
    peel_buf_t resource_fork;
} peel_file_t;

// === File List ===

// Flat list of files produced by archive extraction.
typedef struct {
    peel_file_t *files; // Heap-allocated array of extracted files
    int count; // Number of entries
} peel_file_list_t;

// Free every buffer in every file, then the array itself.  Zeroes the struct.
void peel_file_list_free(peel_file_list_t *list);

// === Input Helpers ===

// Read an entire file into an owned buffer.
peel_buf_t peel_read_file(const char *path, peel_err_t **err);

// Copy caller data into a new owned buffer.
peel_buf_t peel_buf_copy(const void *src, size_t len, peel_err_t **err);

// Wrap an existing pointer without copying.
// Caller guarantees pointer lifetime.  peel_free() on the returned buffer
// is a safe no-op for the data pointer.
peel_buf_t peel_buf_wrap(const void *src, size_t len);

// === Format Detection ===

// Identify the outermost format without peeling.
// Returns a short name ("hqx", "bin", "sit", "cpt") or NULL if unknown.
const char *peel_detect(const uint8_t *src, size_t len);

// === Writing extracted files ===

// True if `path` is safe to write beneath an output directory: non-empty, not
// absolute, and no component empty, "." or "..".  Anything that turns peeler
// names into files must check this first -- archive extraction wrote
// attacker-named paths outside its output directory (09-storage F-13).
bool peel_path_is_confined(const char *path);

// The AppleDouble sidecar ("._NAME") that carries an extracted file's
// resource fork and Finder info (type, creator, flags).  Returns 0 and a
// malloc'd buffer in *out / *out_len; 0 with *out == NULL when the file has
// neither, so no sidecar should be written; or a negative errno.  Both the
// peeler CLI and the emulator's archive extraction write sidecars through
// this, so they are identical.
int peel_build_sidecar(const peel_file_t *f, uint8_t **out, size_t *out_len);

// === Main Entry Points ===

// Detect, peel all layers, return extracted files.
// Handles arbitrarily nested formats (e.g. .sit.hqx).
peel_file_list_t peel(const uint8_t *src, size_t len, peel_err_t **err);

// Convenience: read the file at path, then peel().
peel_file_list_t peel_path(const char *path, peel_err_t **err);

// === Per-Format Entry Points (Wrappers: buf → buf) ===

// BinHex 4.0 (.hqx) — peel wrapper, return data fork only.
peel_buf_t peel_hqx(const uint8_t *src, size_t len, peel_err_t **err);

// BinHex 4.0 — peel wrapper, return file with both forks and metadata.
peel_file_t peel_hqx_file(const uint8_t *src, size_t len, peel_err_t **err);

// MacBinary (.bin) — peel wrapper, return data fork only.
peel_buf_t peel_bin(const uint8_t *src, size_t len, peel_err_t **err);

// MacBinary — peel wrapper, return file with both forks and metadata.
peel_file_t peel_bin_file(const uint8_t *src, size_t len, peel_err_t **err);

// === Per-Format Entry Points (Archives: buf → file list) ===

// StuffIt classic / SIT5 (.sit).
peel_file_list_t peel_sit(const uint8_t *src, size_t len, peel_err_t **err);

// Compact Pro (.cpt).
peel_file_list_t peel_cpt(const uint8_t *src, size_t len, peel_err_t **err);

#ifdef __cplusplus
}
#endif

#endif // PEELER_H
