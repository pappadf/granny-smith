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

// Shell command: peeler
static uint64_t cmd_peeler(int argc, char *argv[]) {
    // Parse options
    peeler_ctx_t ctx = {.output_dir = ".", .verbose = 0, .file_count = 0, .probe_only = 0};

    int opt_idx = 1;
    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-o") == 0) {
            opt_idx++;
            if (opt_idx >= argc) {
                fprintf(stderr, "peeler: -o requires directory argument\n");
                return 0;
            }
            ctx.output_dir = argv[opt_idx];
            opt_idx++;
        } else if (strcmp(argv[opt_idx], "-v") == 0 || strcmp(argv[opt_idx], "--verbose") == 0) {
            ctx.verbose = 1;
            opt_idx++;
        } else if (strcmp(argv[opt_idx], "-p") == 0 || strcmp(argv[opt_idx], "--probe") == 0) {
            ctx.probe_only = 1;
            opt_idx++;
        } else if (strcmp(argv[opt_idx], "-h") == 0 || strcmp(argv[opt_idx], "--help") == 0) {
            printf("Usage: peeler [options] <archive1> [<archive2> ...]\n");
            printf("Unpacks classic Macintosh archives (StuffIt, BinHex, CompactPro, MacBinary).\n\n");
            printf("Options:\n");
            printf("  -o <dir>       Extract files to specified directory (default: .)\n");
            printf("  -v, --verbose  Enable verbose output\n");
            printf("  -p, --probe    Test format detection without extracting\n");
            printf("  -h, --help     Show this help message\n");
            return 0;
        } else {
            fprintf(stderr, "peeler: unknown option '%s'\n", argv[opt_idx]);
            return 0;
        }
    }

    // Check for input files
    if (opt_idx >= argc) {
        fprintf(stderr, "peeler: no input files specified\n");
        fprintf(stderr, "Usage: peeler [options] <archive1> [<archive2> ...]\n");
        return 0;
    }

    // Handle probe mode
    if (ctx.probe_only) {
        int status = 0;
        for (int i = opt_idx; i < argc; i++) {
            // Read the file and detect its format
            peel_err_t *err = NULL;
            peel_buf_t buf = peel_read_file(argv[i], &err);
            if (err) {
                fprintf(stderr, "peeler: cannot open '%s': %s\n", argv[i], peel_err_msg(err));
                peel_err_free(err);
                status = 1;
                continue;
            }

            const char *format = peel_detect(buf.data, buf.size);
            if (format) {
                printf("%s: Supported (%s format detected)\n", argv[i], format);
            } else {
                printf("%s: NOT a supported format\n", argv[i]);
                status = 1;
            }
            peel_free(&buf);
        }
        return status;
    }

    // Ensure output directory exists
    if (mkdir(ctx.output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "peeler: cannot create output directory '%s': %s\n", ctx.output_dir, strerror(errno));
        return 0;
    }

    // Process each archive
    int status = 0;
    for (int i = opt_idx; i < argc; i++) {
        if (process_archive(&ctx, argv[i]) != 0) {
            status = 1;
        } else {
            printf("Successfully extracted '%s' (%d file%s)\n", argv[i], ctx.file_count,
                   ctx.file_count == 1 ? "" : "s");
            ctx.file_count = 0; // Reset for next archive
        }
    }

    return status;
}

// Initialize peeler shell integration
void peeler_shell_init(void) {
    register_cmd("peeler", "Archive",
                 "peeler [-o <dir>] [-v] [-p] <archive> ... – unpack Mac archives (.sit, .hqx, .cpt, .bin)",
                 cmd_peeler);
}
