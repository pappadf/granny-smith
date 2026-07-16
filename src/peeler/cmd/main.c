// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// main.c
// CLI entry point for the `peeler` tool.
//
// Usage:  peeler <archive> [<output-dir>]
//
// Reads the archive, peels all layers, and writes each extracted file to
// the output directory.  Resource forks are emitted as AppleDouble (._)
// sidecar files.

#include "peeler.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// AppleDouble magic and version — appledouble.md § "File Identification"
#define APPLEDOUBLE_MAGIC   0x00051607
#define APPLEDOUBLE_VERSION 0x00020000

// AppleDouble entry IDs — appledouble.md § "Standard Entry IDs"
#define AD_ENTRY_FINDER_INFO 9
#define AD_ENTRY_RSRC_FORK   2

// Fixed sizes within the AppleDouble header
#define AD_HEADER_SIZE 26 // magic(4) + version(4) + filler(16) + count(2)
#define AD_ENTRY_SIZE  12 // id(4) + offset(4) + length(4)
#define AD_FINDER_LEN  32 // FinderInfo(16) + ExtendedFinderInfo(16)

// ============================================================================
// Static Helpers
// ============================================================================

// Write a 32-bit big-endian value to a byte pointer.
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

// Write a 16-bit big-endian value to a byte pointer.
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

// Build a file path from directory and filename, writing into buf.
// Returns false if the combined path would overflow the buffer.
static bool build_path(char *buf, size_t buf_size, const char *dir, const char *name) {
    int n = snprintf(buf, buf_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < buf_size;
}

// Recursively create all parent directories for the given file path.
// Similar to `mkdir -p` on the parent directory.
static bool ensure_parent_dirs(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, len + 1);

    // Walk the path and create each directory component
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            tmp[i] = '/';
        }
    }
    return true;
}

// Write raw bytes to a file.  Returns true on success.
static bool write_blob(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    bool ok = (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

// Write the data fork of a file to the output directory.
static bool write_data_fork(const char *dir, const peel_file_t *f) {
    const char *name = f->meta.name[0] ? f->meta.name : "unnamed";
    char path[1024];
    if (!build_path(path, sizeof(path), dir, name)) {
        fprintf(stderr, "peeler: path too long for '%s'\n", name);
        return false;
    }
    if (!ensure_parent_dirs(path)) {
        fprintf(stderr, "peeler: cannot create directories for '%s'\n", name);
        return false;
    }
    return write_blob(path, f->data_fork.data, f->data_fork.size);
}

// Build an AppleDouble header file containing Finder info and the resource
// fork.  Layout: [header][finder_entry_desc][rsrc_entry_desc][finder_data][rsrc_data]
// appledouble.md § "Writing & Updating Rules"
static bool write_appledouble(const char *dir, const peel_file_t *f) {
    const char *name = f->meta.name[0] ? f->meta.name : "unnamed";

    // Build ._<name> sidecar path, inserting ._ before the filename
    // component (e.g. "dir/subdir/._file" not "dir/._ subdir/file").
    char path[1024];
    const char *slash = strrchr(name, '/');
    int n;
    if (slash) {
        // name contains a directory component
        n = snprintf(path, sizeof(path), "%s/%.*s/._%s", dir,
                     (int)(slash - name), name, slash + 1);
    } else {
        n = snprintf(path, sizeof(path), "%s/._%s", dir, name);
    }
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "peeler: path too long for '._%s'\n", name);
        return false;
    }
    if (!ensure_parent_dirs(path)) {
        fprintf(stderr, "peeler: cannot create directories for '._%s'\n", name);
        return false;
    }

    // Layout depends on whether resource fork data is present:
    //   - With rsrc: header(26) + 2 descriptors(24) + FinderInfo(32) + rsrc data
    //   - Without:   header(26) + 1 descriptor(12)  + FinderInfo(32)
    bool has_rsrc = (f->resource_fork.size > 0);
    size_t num_entries = has_rsrc ? 2 : 1;
    uint32_t finder_offset = (uint32_t)(AD_HEADER_SIZE + num_entries * AD_ENTRY_SIZE);
    uint32_t rsrc_offset = finder_offset + AD_FINDER_LEN;
    size_t total = has_rsrc ? rsrc_offset + f->resource_fork.size
                            : finder_offset + AD_FINDER_LEN;

    uint8_t *buf = calloc(1, total);
    if (!buf) {
        return false;
    }

    // Fixed header — appledouble.md § "Fixed Header"
    uint8_t *p = buf;
    put_be32(p, APPLEDOUBLE_MAGIC);
    p += 4;
    put_be32(p, APPLEDOUBLE_VERSION);
    p += 4;
    // 16 bytes filler (already zero from calloc)
    p += 16;
    put_be16(p, (uint16_t)num_entries);
    p += 2;

    // Entry descriptor 1: Finder Info — appledouble.md § "Entry Descriptors"
    put_be32(p, AD_ENTRY_FINDER_INFO);
    p += 4;
    put_be32(p, finder_offset);
    p += 4;
    put_be32(p, AD_FINDER_LEN);
    p += 4;

    // Entry descriptor 2: Resource Fork (only if present)
    if (has_rsrc) {
        put_be32(p, AD_ENTRY_RSRC_FORK);
        p += 4;
        put_be32(p, rsrc_offset);
        p += 4;
        put_be32(p, (uint32_t)f->resource_fork.size);
        p += 4;
    }

    // Finder Info payload: type(4) + creator(4) + flags(2) + padding(22)
    // appledouble.md § "Finder Info"
    uint8_t *finder = buf + finder_offset;
    put_be32(finder, f->meta.mac_type);
    put_be32(finder + 4, f->meta.mac_creator);
    put_be16(finder + 8, f->meta.finder_flags);
    // Remaining 22 bytes are zero (from calloc)

    // Resource fork payload (only if present)
    if (has_rsrc) {
        memcpy(buf + rsrc_offset, f->resource_fork.data, f->resource_fork.size);
    }

    bool ok = write_blob(path, buf, total);
    free(buf);
    return ok;
}

// Print usage text and exit.
static void usage(const char *progname) {
    fprintf(stderr, "usage: %s <archive> [<output-dir>]\n", progname);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_dir = (argc == 3) ? argv[2] : ".";

    // Create output directory if it does not exist (ignore EEXIST)
    if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "peeler: cannot create '%s': %s\n", output_dir, strerror(errno));
        return 1;
    }

    // Peel the archive
    peel_err_t *err = NULL;
    peel_file_list_t files = peel_path(input_path, &err);
    if (err) {
        fprintf(stderr, "peeler: %s\n", peel_err_msg(err));
        peel_err_free(err);
        return 1;
    }

    // Write each extracted file to disk
    int failures = 0;
    for (int i = 0; i < files.count; i++) {
        const peel_file_t *f = &files.files[i];

        // Write data fork (always, even if empty — Mac archives track
        // files that have only a resource fork or metadata).
        if (!write_data_fork(output_dir, f)) {
            fprintf(stderr, "peeler: failed to write '%s'\n", f->meta.name);
            failures++;
        }

        // Write resource fork as AppleDouble sidecar.  Create a sidecar
        // whenever there is resource fork data OR Finder metadata
        // (type/creator/flags), since the sidecar carries both.
        if (f->resource_fork.size > 0 ||
            f->meta.mac_type != 0 || f->meta.mac_creator != 0 ||
            f->meta.finder_flags != 0) {
            if (!write_appledouble(output_dir, f)) {
                fprintf(stderr, "peeler: failed to write '._%s'\n", f->meta.name);
                failures++;
            }
        }
    }

    peel_file_list_free(&files);
    return failures > 0 ? 1 : 0;
}
