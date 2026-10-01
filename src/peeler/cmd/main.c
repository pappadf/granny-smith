// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// main.c
// CLI entry point for the `peeler` tool.
//
// Usage:  peeler <archive> [<output-dir>]
//         peeler list <archive>
//         peeler extract <archive> <member> [<output-dir>]
//
// The first form reads the archive, peels all layers, and writes each
// extracted file to the output directory.  Resource forks are emitted as
// AppleDouble (._) sidecar files.
//
// `list` and `extract` use the structure-first API: `list` reads only the
// archive's headers and directory; `extract` decodes one member.  Both see
// through wrappers (a .sit.hqx lists the .sit's contents).

#include "peeler.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// Static Helpers
// ============================================================================

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
    if (!peel_path_is_confined(name)) {
        fprintf(stderr, "peeler: refusing entry '%s': it would land outside the output directory\n", name);
        return false;
    }
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

// Write the AppleDouble sidecar ("._<name>") carrying the file's resource
// fork and Finder info, through the same builder the emulator's archive
// extraction uses, so the two produce the same sidecar.  A file with neither
// gets none.
static bool write_appledouble(const char *dir, const peel_file_t *f) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (peel_build_sidecar(f, &buf, &len) != 0)
        return false;
    if (!buf)
        return true; // nothing to preserve

    const char *name = f->meta.name[0] ? f->meta.name : "unnamed";
    bool ok = false;
    char path[1024];
    // Insert "._" before the filename component (e.g. "dir/sub/._file").
    const char *slash = strrchr(name, '/');
    int n = slash ? snprintf(path, sizeof(path), "%s/%.*s/._%s", dir, (int)(slash - name), name, slash + 1)
                  : snprintf(path, sizeof(path), "%s/._%s", dir, name);
    if (!peel_path_is_confined(name))
        fprintf(stderr, "peeler: refusing entry '%s': it would land outside the output directory\n", name);
    else if (n <= 0 || (size_t)n >= sizeof(path))
        fprintf(stderr, "peeler: path too long for '._%s'\n", name);
    else if (!ensure_parent_dirs(path))
        fprintf(stderr, "peeler: cannot create directories for '._%s'\n", name);
    else
        ok = write_blob(path, buf, len);
    free(buf);
    return ok;
}

// Print usage text and exit.
static void usage(const char *progname) {
    fprintf(stderr,
            "usage: %s <archive> [<output-dir>]\n"
            "       %s list <archive>\n"
            "       %s extract <archive> <member> [<output-dir>]\n",
            progname, progname, progname);
}

// ============================================================================
// Structure-first subcommands
// ============================================================================

// Open `path` and see through wrapper layers: while the archive is a
// wrapper whose payload is itself an archive, open the payload instead.
// The payload of a two-fork wrapper is its data fork, or its resource fork
// when only that holds a recognised format (a .sea.bin).
static peel_archive_t *open_unwrapped(const char *path, peel_err_t **err) {
    peel_source_t *src = peel_source_file(path, err);
    if (!src)
        return NULL;
    peel_archive_t *a = peel_open(src, NULL, NULL, err);
    peel_source_release(src);
    for (int depth = 0; a && peel_is_wrapper(a) && depth < 32; depth++) {
        peel_archive_t *inner = NULL;
        for (int fork = PEEL_FORK_DATA; fork <= PEEL_FORK_RSRC && !inner; fork++) {
            peel_err_t *e = NULL;
            peel_source_t *payload = peel_open_fork(a, 0, fork, &e);
            if (payload && peel_source_size(payload) > 0)
                inner = peel_open(payload, NULL, NULL, &e);
            peel_err_free(e);
            peel_source_release(payload);
        }
        if (!inner)
            break; // the wrapper's one file is what there is
        peel_close(a);
        a = inner;
    }
    return a;
}

// One letter per tier, for the listing.
static char tier_char(peel_tier_t t) {
    static const char c[] = "RIESW";
    return (unsigned)t < sizeof(c) - 1 ? c[t] : '?';
}

// `peeler list <archive>`: one line per entry.
static int cmd_list(const char *path) {
    peel_err_t *err = NULL;
    peel_archive_t *a = open_unwrapped(path, &err);
    if (!a) {
        fprintf(stderr, "peeler: %s\n", peel_err_msg(err));
        peel_err_free(err);
        return 1;
    }
    printf("# %s, %d entries (tiers: R random, I indexed, E earned, S stream, W whole)\n", peel_format(a),
           peel_count(a));
    for (int i = 0; i < peel_count(a); i++) {
        const peel_entry_t *e = peel_entry(a, i);
        if (e->is_dir) {
            printf("%-4s %12s %12s     %s/\n", "dir", "-", "-", e->path);
            continue;
        }
        // The Mac type as four characters, "----" when there is none.
        char type[5] = "----";
        if (e->mac_type)
            for (int k = 0; k < 4; k++) {
                char c = (char)(e->mac_type >> (24 - 8 * k));
                type[k] = (c >= 32 && c < 127) ? c : '?';
            }
        printf("%c%c   %12llu %12llu %s %s\n", tier_char(e->data_tier), e->rsrc_len ? tier_char(e->rsrc_tier) : '-',
               (unsigned long long)e->data_len, (unsigned long long)e->rsrc_len, type, e->path);
    }
    peel_close(a);
    return 0;
}

// Copy a whole source into `fp` a chunk at a time.  True on success.
static bool copy_source(peel_source_t *s, FILE *fp) {
    uint8_t buf[65536];
    uint64_t off = 0, size = peel_source_size(s);
    while (off < size) {
        size_t want = size - off < sizeof(buf) ? (size_t)(size - off) : sizeof(buf);
        if (peel_source_read_exact(s, off, buf, want) != 0 || fwrite(buf, 1, want, fp) != want)
            return false;
        off += want;
    }
    return true;
}

// `peeler extract <archive> <member> [<output-dir>]`: one member, its data
// fork under its own name and its resource fork and Finder info in the
// AppleDouble sidecar.
static int cmd_extract(const char *path, const char *member, const char *out_dir) {
    peel_err_t *err = NULL;
    peel_archive_t *a = open_unwrapped(path, &err);
    if (!a) {
        fprintf(stderr, "peeler: %s\n", peel_err_msg(err));
        peel_err_free(err);
        return 1;
    }
    int i = peel_lookup(a, member);
    const peel_entry_t *e = peel_entry(a, i);
    if (!e || e->is_dir) {
        fprintf(stderr, "peeler: no file '%s' in '%s'\n", member, path);
        peel_close(a);
        return 1;
    }
    if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "peeler: cannot create '%s': %s\n", out_dir, strerror(errno));
        peel_close(a);
        return 1;
    }
    const char *base = strrchr(e->path, '/');
    base = base ? base + 1 : e->path;
    peel_file_t f;
    memset(&f, 0, sizeof(f));
    snprintf(f.meta.name, sizeof(f.meta.name), "%.255s", base);
    f.meta.mac_type = e->mac_type;
    f.meta.mac_creator = e->mac_creator;
    f.meta.finder_flags = e->finder_flags;

    int rc = 1;
    char dst[1024];
    peel_source_t *data = peel_open_fork(a, i, PEEL_FORK_DATA, &err);
    FILE *fp = NULL;
    if (!data) {
        fprintf(stderr, "peeler: %s\n", peel_err_msg(err));
    } else if (!build_path(dst, sizeof(dst), out_dir, f.meta.name) || !(fp = fopen(dst, "wb"))) {
        fprintf(stderr, "peeler: cannot create '%s'\n", dst);
    } else if (!copy_source(data, fp)) {
        fprintf(stderr, "peeler: '%s' did not decode\n", member);
    } else {
        f.resource_fork = peel_read_fork(a, i, PEEL_FORK_RSRC, &err);
        if (err)
            fprintf(stderr, "peeler: %s\n", peel_err_msg(err));
        else
            rc = write_appledouble(out_dir, &f) ? 0 : 1;
    }
    if (fp && fclose(fp) != 0)
        rc = 1;
    peel_err_free(err);
    peel_free(&f.resource_fork);
    peel_source_release(data);
    peel_close(a);
    return rc;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "list") == 0)
        return cmd_list(argv[2]);
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "extract") == 0)
        return cmd_extract(argv[2], argv[3], argc == 5 ? argv[4] : ".");
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

        // The AppleDouble sidecar: resource fork and Finder metadata, if
        // the file has either.
        if (!write_appledouble(output_dir, f)) {
            fprintf(stderr, "peeler: failed to write '._%s'\n", f->meta.name);
            failures++;
        }
    }

    peel_file_list_free(&files);
    return failures > 0 ? 1 : 0;
}
