// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appledouble.h
// AppleSingle / AppleDouble (v2) container codec — the standard way to keep a
// classic-Mac file's resource fork and Finder Info on a flat, fork-less host
// filesystem (Linux ext4, the browser's OPFS).  Used to preserve forks when a
// file crosses out of an HFS/HFS+ image (copy-out), to reconstruct them on the
// way back in (image_open fork acquisition), and — later — as the AFP server's
// host-side storage format.  See
// local/gs-docs/proposals/proposal-appledouble-support.md and the format note
// under local/gs-docs/AppleDouble/.
//
// Two on-disk shapes share one header layout:
//   AppleSingle (magic 0x00051600): one self-contained file; MAY carry the
//     data fork (entry id 1).
//   AppleDouble header (magic 0x00051607): the sidecar of an AppleDouble pair;
//     everything EXCEPT the data fork (which stays in the plain data file).
//     By convention the header file for NAME is "._NAME" in the same directory.
//
// Header (big-endian, 68000 convention):
//   +0   magic        (u32)
//   +4   version      (u32)  = 0x00020000
//   +8   filler       (16 bytes, zero)
//   +24  numEntries   (u16)
//   +26  descriptors  numEntries x { entryID(u32), offset(u32), length(u32) }
//        (offset is from the start of the header file)
//
// This codec is I/O-free: parse borrows slices from the caller's buffer (which
// must outlive the returned ad_file_t, like rfork_parse); build returns a
// freshly malloc'd buffer the caller frees.

#pragma once

#ifndef GS_APPLEDOUBLE_H
#define GS_APPLEDOUBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLEDOUBLE_MAGIC  0x00051607u
#define APPLESINGLE_MAGIC  0x00051600u
#define APPLE_FORK_VERSION 0x00020000u

// Predefined entry IDs (subset from the developer note we act on).
typedef enum {
    AD_ENTRY_DATA = 1, // data fork (AppleSingle only in practice)
    AD_ENTRY_RSRC = 2, // resource fork — feeds rfork_parse() verbatim
    AD_ENTRY_REALNAME = 3, // home-filesystem name
    AD_ENTRY_COMMENT = 4, // Finder comment
    AD_ENTRY_DATES = 8, // create/modify/backup/access, secs rel 2000-01-01 GMT
    AD_ENTRY_FINDER = 9, // 16 B FInfo + 16 B FXInfo (our 32-byte Finder Info)
    AD_ENTRY_MACINFO = 10, // locked/protected bits
    AD_ENTRY_SHORTNAME = 13, // AFP short name
    AD_ENTRY_AFPINFO = 14, // AFP attributes word
    AD_ENTRY_DIRID = 15, // AFP directory ID
} ad_entry_id_t;

// The 32-byte Macintosh Finder Info block (FInfo + FXInfo), the payload of
// entry id 9 and the same layout the image VFS exposes at "<file>/finf".
#define AD_FINDER_INFO_SIZE 32u

// The unknown-entry ceiling.  Real files carry a handful; this bounds the
// parse table and is far above any legitimate header.
#define AD_MAX_ENTRIES 32u

// One entry descriptor.  `bytes` borrows into the parsed buffer (parse) or is
// supplied by the caller (build); may be NULL when `len` is 0.
typedef struct {
    uint32_t id;
    const uint8_t *bytes;
    size_t len;
} ad_entry_t;

typedef struct {
    uint32_t magic; // APPLEDOUBLE_MAGIC or APPLESINGLE_MAGIC
    uint32_t version; // APPLE_FORK_VERSION
    size_t n_entries;
    ad_entry_t entries[AD_MAX_ENTRIES];
    // Convenience views into entries[] for the ones we act on (NULL if absent).
    const uint8_t *rsrc;
    size_t rsrc_len; // entry id 2
    const uint8_t *finder;
    size_t finder_len; // entry id 9
    const uint8_t *data;
    size_t data_len; // entry id 1
} ad_file_t;

// True if `buf` begins with a well-formed AppleSingle or AppleDouble header
// (recognised magic + v2 + a self-consistent, in-bounds entry table).  Cheap;
// safe on arbitrary/truncated input.
bool ad_detect(const uint8_t *buf, size_t len);

// Parse a header file (or AppleSingle file).  Borrows slices from `buf`, which
// must outlive `*out`.  Returns 0 on success, -EINVAL on a malformed header.
// Entries whose id/offset/length fall outside the buffer are rejected.
int ad_parse(const uint8_t *buf, size_t len, ad_file_t *out);

// Build a container from `entries` (written in the given order; place metadata
// first and forks last, per the note's guidance).  `applesingle` selects the
// magic — pass false for an AppleDouble header sidecar (and then do NOT include
// an AD_ENTRY_DATA entry).  On success returns 0 and hands back a malloc'd
// buffer in `*out`/`*out_len` that the caller frees.  Returns -EINVAL on bad
// arguments, -ENOMEM on allocation failure.
int ad_build(bool applesingle, const ad_entry_t *entries, size_t n_entries, uint8_t **out, size_t *out_len);

// Convenience for the common copy-out case: build an AppleDouble header sidecar
// carrying Finder Info (entry 9, 32 bytes; omitted if `finder` is NULL) and the
// resource fork (entry 2; omitted if `rsrc_len` is 0).  Returns -EINVAL if both
// are absent (caller should then write no sidecar at all).
int ad_build_sidecar(const uint8_t *rsrc, size_t rsrc_len, const uint8_t *finder /* 32 bytes or NULL */, uint8_t **out,
                     size_t *out_len);

#endif // GS_APPLEDOUBLE_H
