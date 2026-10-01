// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// format_registry.h
// The one table of formats a byte source can be in.  It replaces three
// probe orders -- the image opener's, the VFS mount probe's and SCSI media
// validation's -- and peeler's own.
//
// Two kinds of format:
//   - a wrapper turns a source into another source (plus, for the two-fork
//     ones, a resource fork): UDIF, NDIF, DiskCopy 4.2, and peeler's BinHex,
//     MacBinary and gzip.  Wrappers are tried first and unwrapped in a loop,
//     so foo.img.bin, foo.img.hqx and foo.dmg all come out as the disk;
//   - a namespace format opens a source as a tree (namespace.h): a
//     partitioned or bare disk, and peeler's archives.  These register at
//     startup from the layer that implements them (the VFS), so this
//     module, the storage engine and their unit tests need none of it.
//
// Detection reads a bounded probe: 64 KiB of head and of tail
// (PEEL_DETECT_BUDGET), plus the resource fork when one exists.

#ifndef GS_FORMAT_REGISTRY_H
#define GS_FORMAT_REGISTRY_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>

struct gs_namespace;

// What detection sees.
typedef struct {
    peel_probe_t p; // head and tail of the data fork, and its size
    gs_source_t *data; // the data fork (for a detector that must read more)
    gs_source_t *rsrc; // the resource fork, or NULL
} gs_probe_t;

typedef enum { GS_FMT_WRAPPER, GS_FMT_NAMESPACE } gs_fmt_kind_t;

typedef struct gs_format {
    const char *name; // "udif", "ndif", "dc42", "hqx", "bin", "gz", "disk", "sit", ...
    gs_fmt_kind_t kind;
    const char *doc; // one line, for listings
    bool (*detect)(const gs_probe_t *p);
    // Wrapper: the payload's forks (new references; *rsrc may be NULL).
    int (*unwrap)(const gs_probe_t *p, gs_source_t **data, gs_source_t **rsrc);
    // Namespace: the tree.  NULL when the source is not usable after all.
    struct gs_namespace *(*open_namespace)(gs_source_t *data, gs_source_t *rsrc);
} gs_format_t;

// Register a namespace format (appended; detection order is registration
// order).  Idempotent per name.
void gs_format_register(const gs_format_t *f);

// Every format, wrappers first.  *count set.
const gs_format_t *const *gs_formats(int *count);

// Fill a probe of `data` (+ `rsrc`, not retained).  0 or a negative errno.
int gs_probe_init(gs_probe_t *p, gs_source_t *data, gs_source_t *rsrc);
void gs_probe_free(gs_probe_t *p);

// The first format of `kind` whose detector accepts the probe, or NULL.
const gs_format_t *gs_format_detect(const gs_probe_t *p, gs_fmt_kind_t kind);

// The result of unwrapping every wrapper layer.
typedef struct {
    gs_source_t *data; // innermost data fork (a reference)
    gs_source_t *rsrc; // its resource fork, or NULL (a reference)
    char chain[96]; // the wrappers peeled, outermost first: "bin+ndif"; "" for none
    // A DiskCopy 4.2 layer's container (a reference), for its tag section,
    // or NULL when there was none.
    gs_source_t *dc42;
    // The last peeler wrapper's container and format (a reference), so a
    // payload that is no namespace can be shown as that one-file wrapper.
    gs_source_t *peeler_outer;
    const char *peeler_format;
} gs_unwrapped_t;

// Peel wrapper layers off (`data`, `rsrc`) until none detects.  Always
// succeeds for readable input (zero layers is a result); a wrapper that
// detects but fails to open ends the loop there.  Release with
// gs_unwrapped_free.
int gs_format_unwrap(gs_source_t *data, gs_source_t *rsrc, gs_unwrapped_t *out);
void gs_unwrapped_free(gs_unwrapped_t *u);

// Open (`data`, `rsrc`) as a namespace: unwrap, detect a namespace format,
// open it; failing that, a peeler wrapper that was peeled is itself the
// namespace (its one file).  NULL with *err (-ENOTDIR: not a tree).
// *format (may be NULL) gets the namespace format's name.
struct gs_namespace *gs_format_open_namespace(gs_source_t *data, gs_source_t *rsrc, const char **format, int *err);

// True when gs_format_open_namespace would succeed on the format test alone
// (no namespace is opened): the VFS listing's "expandable" flag.
bool gs_format_is_namespace(gs_source_t *data, gs_source_t *rsrc);

// The namespace format the registry recognises (`data` already unwrapped),
// or NULL: what files.probe and scsi.identify_cdrom report.
const gs_format_t *gs_format_contents(gs_source_t *data, gs_source_t *rsrc);

// Opener the registry uses to show a peeler wrapper as a one-file
// namespace; installed by the VFS with its archive namespace.
void gs_format_set_wrapper_namespace(struct gs_namespace *(*open)(gs_source_t *src, const char *format));

// === DiskCopy 4.2 ===========================================================

#define DISKCOPY_HEADER_SIZE 0x54

// The data and tag section sizes of a DiskCopy 4.2 image whose 0x54-byte
// header is `hdr`, in a file of `file_size` bytes.  True when it is one.
bool dc42_parse_header(const uint8_t *hdr, size_t len, uint64_t file_size, uint32_t *data_size, uint32_t *tag_size);

#endif // GS_FORMAT_REGISTRY_H
