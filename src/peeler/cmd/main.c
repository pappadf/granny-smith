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
