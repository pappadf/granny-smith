// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// resource_fork.h
// Read-only parser for the classic-Mac resource-fork on-disk format.  Given
// a contiguous buffer of fork bytes, it surfaces the type list, ref list,
// and per-resource bytes/name/attrs through a small C API.  Consumed by
// image_vfs.c (synthetic /rsrc/<TYPE>/<id> path tree) and the upcoming
// `re` orchestrator.
//
// The parser does not copy resource bytes — the supplied fork buffer must
// outlive the returned rfork_t.  The parser does build a small index over
// the type list, ref list, and name list so lookups are O(types) + O(ids)
// rather than scanning the fork on every call.
//
// On-disk format (big-endian throughout):
//   +0   data_offset     (u32) — byte offset of data area in fork
//   +4   map_offset      (u32) — byte offset of map area in fork
//   +8   data_length     (u32) — size of data area
//   +12  map_length      (u32) — size of map area
//   data area:           per resource: 4-byte length (u32 BE) + length bytes
//   map area:
//     +0..15  copy of fork header (reserved)
//     +16..21 reserved
//     +22..23 fork attrs (u16)
//     +24..25 offset to type list, relative to map start (u16)
//     +26..27 offset to name list, relative to map start (u16)
//     [at type-list offset]:
//        +0..1   num types - 1 (u16) — yes, minus one
//        +2.. 8 bytes per type:
//          +0  type 4-CC
//          +4  count - 1 (u16) — minus one again
//          +6  ref-list offset, relative to type-list start (u16)
//        ref list per type:
//          +0  id            (i16)
//          +2  name offset   (i16, -1 = no name; relative to name-list start)
//          +4  attrs byte + 24-bit data offset (relative to data-area start)
//          +8  reserved      (u32)
//     [at name-list offset]:
//        Pascal strings (1-byte length + MacRoman bytes)

#pragma once

#ifndef GS_RESOURCE_FORK_H
#define GS_RESOURCE_FORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Resource attrs byte bits, from Inside Macintosh.
#define RFORK_ATTR_SYSHEAP    0x40
#define RFORK_ATTR_PURGEABLE  0x20
#define RFORK_ATTR_LOCKED     0x10
#define RFORK_ATTR_PROTECTED  0x08
#define RFORK_ATTR_PRELOAD    0x04
#define RFORK_ATTR_CHANGED    0x02
#define RFORK_ATTR_COMPRESSED 0x01

typedef struct rfork rfork_t;

// Parse a resource fork from a buffer.  The buffer must outlive the
// returned rfork_t (no copy).  Returns NULL on corruption with *errmsg_out
// set to a static string describing the failure.
rfork_t *rfork_parse(const uint8_t *fork_bytes, size_t fork_len, const char **errmsg_out);
void rfork_free(rfork_t *rf);

// Top-of-fork attrs word (the "fork attributes" field at map+22).
uint16_t rfork_attrs(const rfork_t *rf);

// Enumerate type 4-CCs (catalog order).  rfork_type_at returns a pointer
// to 4 bytes owned by the rfork_t (no copy); the bytes are MacRoman.
size_t rfork_num_types(const rfork_t *rf);
const uint8_t *rfork_type_at(const rfork_t *rf, size_t idx);

// Enumerate IDs under a type (ref-list order).  Returns 0 if the type is
// not present in the fork.
size_t rfork_num_resources(const rfork_t *rf, const uint8_t type[4]);
int16_t rfork_id_at(const rfork_t *rf, const uint8_t type[4], size_t idx);

// Resolve one resource.  Returns 0 on hit and populates *bytes_out (pointer
// into the fork buffer), *size_out, *name_out (UTF-8, NUL-terminated; "" if
// no name), and *attrs_out.  Returns -ENOENT on miss.  Any out parameter
// may be NULL.
int rfork_lookup(const rfork_t *rf, const uint8_t type[4], int16_t id, const uint8_t **bytes_out, size_t *size_out,
                 const char **name_out, uint8_t *attrs_out);

// === Path-component helpers (used by image_vfs.c and re/) ===================

// Format a 4-CC type as a UTF-8 path component (MacRoman -> UTF-8).
// Always NUL-terminates when cap > 0.
void rfork_type_to_path(const uint8_t type[4], char *out, size_t cap);

// Parse a UTF-8 path component back into a 4-CC (MacRoman).  Most callers
// pass plain-ASCII type names like "CODE", "vers", "STR " (with trailing
// space) — the inverse transcoder rejects components that don't decode to
// exactly four MacRoman bytes.  Returns 0 on success, -EINVAL otherwise.
int rfork_type_from_path(const char *path_component, uint8_t out[4]);

// Format a signed-16-bit resource ID into a base-10 path component
// ("128", "0", "-16").  Always NUL-terminates when cap > 0.
void rfork_id_to_path(int16_t id, char *out, size_t cap);

// Parse a base-10 path component as a resource ID.  Rejects empty input,
// trailing junk, and values outside the int16 range.  Returns 0 on
// success, -EINVAL otherwise.
int rfork_id_from_path(const char *path_component, int16_t *out);

// Format the .info sidecar JSON for a resource.  Output shape:
//   {"name":"Foo","attrs":["preload","locked"],"size":1576}\n
// Returns the number of bytes written (excluding NUL), or -EINVAL if the
// buffer is too small.
int rfork_info_format(const char *name, uint8_t attrs, size_t size, char *out, size_t cap);

#endif // GS_RESOURCE_FORK_H
