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

struct ns;

// What detection sees.
typedef struct {
    peel_probe_t p; // head and tail of the data fork, and its size
    source_t *data; // the data fork (for a detector that must read more)
    source_t *rsrc; // the resource fork, or NULL
} format_probe_t;

typedef enum { GS_FMT_WRAPPER, GS_FMT_NAMESPACE } format_kind_t;

typedef struct format {
    const char *name; // "udif", "ndif", "dc42", "hqx", "bin", "gz", "disk", "sit", ...
    format_kind_t kind;
    const char *doc; // one line, for listings
    bool (*detect)(const format_probe_t *p);
    // Wrapper: the payload's forks (new references; *rsrc may be NULL).
    int (*unwrap)(const format_probe_t *p, source_t **data, source_t **rsrc);
    // Namespace: the tree.  NULL when the source is not usable after all.
    struct ns *(*open_namespace)(source_t *data, source_t *rsrc);
} format_t;

// Register a namespace format (appended; detection order is registration
// order).  Idempotent per name.
void format_register(const format_t *f);

// Every format, wrappers first.  *count set.
const format_t *const *format_list(int *count);

// Fill a probe of `data` (+ `rsrc`, not retained).  0 or a negative errno.
int format_probe_init(format_probe_t *p, source_t *data, source_t *rsrc);
void format_probe_free(format_probe_t *p);

// The first format of `kind` whose detector accepts the probe, or NULL.
const format_t *format_detect(const format_probe_t *p, format_kind_t kind);

// The result of unwrapping every wrapper layer.
typedef struct {
    source_t *data; // innermost data fork (a reference)
    source_t *rsrc; // its resource fork, or NULL (a reference)
    char chain[96]; // the wrappers peeled, outermost first: "bin+ndif"; "" for none
    // A DiskCopy 4.2 layer's container (a reference), for its tag section,
    // or NULL when there was none.
    source_t *dc42;
    // The last peeler wrapper's container and format (a reference), so a
    // payload that is no namespace can be shown as that one-file wrapper.
    source_t *peeler_outer;
    const char *peeler_format;
    // A wrapper that detected but would not open (a codec we lack, a UDIF
    // chunk too large to read in place), ending the loop: its name and
    // error, or NULL / 0.  The payload is then not the disk.
    const char *failed_format;
    int failed_rc;
} format_unwrapped_t;

// Peel wrapper layers off (`data`, `rsrc`) until none detects.  Always
// succeeds for readable input (zero layers is a result); a wrapper that
// detects but fails to open ends the loop there.  Release with
// format_unwrapped_free.
int format_unwrap(source_t *data, source_t *rsrc, format_unwrapped_t *out);
void format_unwrapped_free(format_unwrapped_t *u);

// Open (`data`, `rsrc`) as a namespace: unwrap, detect a namespace format,
// open it; failing that, a peeler wrapper that was peeled is itself the
// namespace (its one file).  NULL with *err (-ENOTDIR: not a tree).
// *format (may be NULL) gets the namespace format's name.
struct ns *format_open_namespace(source_t *data, source_t *rsrc, const char **format, int *err);

// True when format_open_namespace would succeed on the format test alone
// (no namespace is opened): the VFS listing's "expandable" flag.
bool format_is_namespace(source_t *data, source_t *rsrc);

// The namespace format the registry recognises (`data` already unwrapped),
// or NULL: what files.probe and scsi.identify_cdrom report.
const format_t *format_contents(source_t *data, source_t *rsrc);

// Opener the registry uses to show a peeler wrapper as a one-file
// namespace; installed by the VFS with its archive namespace.
void format_set_wrapper_namespace(struct ns *(*open)(source_t *src, const char *format));

// === DiskCopy 4.2 ===========================================================

#define DISKCOPY_HEADER_SIZE 0x54

// The data and tag section sizes of a DiskCopy 4.2 image whose 0x54-byte
// header is `hdr`, in a file of `file_size` bytes.  True when it is one.
bool dc42_parse_header(const uint8_t *hdr, size_t len, uint64_t file_size, uint32_t *data_size, uint32_t *tag_size);

// Header fields past the two section sizes: the two checksums, the disk
// format (0 = 400K GCR, 1 = 800K GCR, 2 = 720K MFM, 3 = 1440K MFM), the
// format byte ($12 Mac 400K, $22 Mac 800K and MFM, $02 Lisa 400K) and the
// $0100 "private" word the parser takes as the signature.
#define DC42_NAME_FIELD      64 // the name: a Pascal string padded to 64 bytes
#define DC42_NAME_MAX        63
#define DC42_OFF_DATA_SUM    0x48
#define DC42_OFF_TAG_SUM     0x4C
#define DC42_OFF_DISK_FORMAT 0x50
#define DC42_OFF_FORMAT_BYTE 0x51

// Fold `n` bytes (even) of big-endian words into a running DiskCopy 4.2
// checksum: add each word, rotate right by one.  Start from 0; the data
// sum covers the whole data section, the tag sum every tag but the first
// sector's (DiskCopy skips it, so a tool that recomputes it does too).
uint32_t dc42_checksum(uint32_t sum, const uint8_t *p, size_t n);

// Make the 64-byte name field from a C string: a Pascal string of at most
// DC42_NAME_MAX bytes (longer is cut), zero-padded.
void dc42_name_field(uint8_t field[DC42_NAME_FIELD], const char *name);

// Fill a DiskCopy 4.2 header.  `name_field` goes in as it is, padding and
// all, so a disk keeps the exact field its source file had.
void dc42_build_header(uint8_t hdr[DISKCOPY_HEADER_SIZE], const uint8_t name_field[DC42_NAME_FIELD], uint32_t data_size,
                       uint32_t tag_size, uint32_t data_sum, uint32_t tag_sum, uint8_t disk_format,
                       uint8_t format_byte);

// === LisaEm ProFile images ===================================================
//
// Compatibility with LisaEm: this format exists only so that the Lisa hard
// disk images LisaEm writes (and the many archived ones made with it or its
// companion tools) attach as they are.  Nothing here follows LisaEm's
// emulation; the format is named after it because that is where the images
// come from and what users know them as.
//
// LisaEm keeps a ProFile as a DiskCopy 4.2 file whose tag section holds 20
// bytes per 512-byte block -- the ProFile's 532-byte block split into data
// and tag -- with the blocks in the Lisa OS's logical order.  The emulated
// ProFile serves blocks in the drive's order, which the OS driver's 5:1
// software interleave defines within each group of 16; the wrapper presents
// the file as that: 532-byte blocks, 20-byte tag first, in ProFile block
// order.  See docs/internals/machines/lisa/profile.md.

#define LISAEM_TAG_BYTES  20u // per block
#define LISAEM_INTERLEAVE 16u // blocks per interleave group

// The block count of a LisaEm ProFile image whose DiskCopy 4.2 header is
// `hdr`, in a file of `file_size` bytes, or 0 when it is not one: a valid
// DiskCopy 4.2 header with exactly LISAEM_TAG_BYTES of tag per data block
// and a whole number of interleave groups.  (Apple's DiskCopy writes 12-byte
// tags or none, so no floppy image matches.)
uint32_t lisaem_profile_blocks(const uint8_t *hdr, size_t len, uint64_t file_size);

// The LisaEm (logical) block that holds ProFile block `block`: the inverse
// of the OS driver's interleave, 5 x 13 = 65 = 1 (mod 16).
static inline uint32_t lisaem_logical_block(uint32_t block) {
    uint32_t group = block - block % LISAEM_INTERLEAVE;
    return group + (13u * (block % LISAEM_INTERLEAVE)) % LISAEM_INTERLEAVE;
}

#endif // GS_FORMAT_REGISTRY_H
