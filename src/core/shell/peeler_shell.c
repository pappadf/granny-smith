// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// peeler_shell.c
// Integration of peeler library into granny-smith shell for archive extraction.

#include "log.h"
#include "peeler.h"
#include "shell.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Configuration for peeler operations
typedef struct {
    const char *output_dir;
    int verbose;
    int file_count;
    int probe_only; // Only test format detection without extraction
} peeler_ctx_t;

// Recursively create directory path
static int ensure_dir_exists(const peeler_ctx_t *ctx, const char *path) {
    // Create mutable copy for dirname
    char *path_copy = strdup(path);
    if (!path_copy)
        return -1;

    char *dir = dirname(path_copy);
    char full_path[1024];

    // Build full path with output directory
    if (snprintf(full_path, sizeof(full_path), "%s/%s", ctx->output_dir, dir) >= (int)sizeof(full_path)) {
        fprintf(stderr, "peeler: path too long\n");
        free(path_copy);
        return -1;
    }
    free(path_copy);

    // Create each directory component
    char *p = full_path;
    if (*p == '/')
        p++;

    while ((p = strchr(p, '/'))) {
        *p = '\0';
        if (mkdir(full_path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "peeler: cannot create directory '%s': %s\n", full_path, strerror(errno));
            *p = '/';
            return -1;
        }
        *p = '/';
        p++;
    }

    // Create final directory
    if (mkdir(full_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "peeler: cannot create directory '%s': %s\n", full_path, strerror(errno));
        return -1;
    }

    return 0;
}

// Write a single extracted file to disk
static int write_extracted_file(const peeler_ctx_t *ctx, const peel_file_t *file) {
    const char *name = file->meta.name;
    // Fall back to "untitled" when the archive entry has no name
    if (!name[0])
        name = "untitled";

    if (ctx->verbose) {
        printf("Extracting: %s\n", name);
    }

    // Ensure parent directory exists
    if (ensure_dir_exists(ctx, name) != 0) {
        return -1;
    }

    // Build full output path
    char full_path[1024];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", ctx->output_dir, name) >= (int)sizeof(full_path)) {
        fprintf(stderr, "peeler: path too long\n");
        return -1;
    }

    // Write data fork to file
    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        fprintf(stderr, "peeler: cannot create file '%s': %s\n", full_path, strerror(errno));
        return -1;
    }

    if (file->data_fork.size > 0) {
        size_t written = fwrite(file->data_fork.data, 1, file->data_fork.size, fp);
        if (written != file->data_fork.size) {
            fprintf(stderr, "peeler: write error: %s\n", strerror(errno));
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    // Resource forks are not typically used in the emulator's MEMFS
    // For now, we silently ignore them

    return 0;
}

// Process a single archive file
static int process_archive(const peeler_ctx_t *ctx, const char *filepath) {
    // Peel the archive — handles all layers automatically
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_path(filepath, &err);

    if (err) {
        fprintf(stderr, "peeler: failed to extract '%s': %s\n", filepath, peel_err_msg(err));
        peel_err_free(err);
        return -1;
    }

    if (list.count == 0) {
        fprintf(stderr, "peeler: no files extracted from '%s'\n", filepath);
        peel_file_list_free(&list);
        return -1;
    }

    // Write each extracted file
    int status = 0;
    for (int i = 0; i < list.count; i++) {
        if (write_extracted_file(ctx, &list.files[i]) != 0) {
            status = -1;
            break;
        }
    }

    int count = list.count;
    peel_file_list_free(&list);

    if (status == 0) {
        // Update the caller's file count on success (via cast — ctx is stack-owned)
        ((peeler_ctx_t *)ctx)->file_count += count;
    }

    return status;
}

// Public entry point: extract a single archive at `path` into `out_dir`.
// Used by the typed `peeler` root method to bypass shell_dispatch.
// Returns 0 on success, non-zero on failure.
int peeler_shell_extract(const char *path, const char *out_dir) {
    if (!path)
        return -1;
    peeler_ctx_t ctx = {
        .output_dir = (out_dir && *out_dir) ? out_dir : ".", .verbose = 0, .file_count = 0, .probe_only = 0};
    if (mkdir(ctx.output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "peeler: cannot create output directory '%s': %s\n", ctx.output_dir, strerror(errno));
        return -1;
    }
    int rc = process_archive(&ctx, path);
    if (rc == 0)
        printf("Successfully extracted '%s' (%d file%s)\n", path, ctx.file_count, ctx.file_count == 1 ? "" : "s");
    return rc;
}
