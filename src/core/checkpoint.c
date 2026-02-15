// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint.c
// Checkpoint file I/O: open/close, block read/write, file serialization.
// Extracted from system.c to support multi-machine checkpoint handling.
//
// Two on-disk formats:
//   v2 (GSCHKPT2) — per-block RLE with file/line metadata; used for consolidated checkpoints
//   v3 (GSCHKPT3) — whole-file RLE, no per-block metadata; used for quick checkpoints

#include "checkpoint.h"
#include "build_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// v2 signature for consolidated (full export) checkpoints
static const char CHECKPOINT_MAGIC_V2[] = "GSCHKPT2";

// v3 signature for quick (background auto-save) checkpoints
static const char CHECKPOINT_MAGIC_V3[] = "GSCHKPT3";

// Both signatures share the same length
#define CHECKPOINT_MAGIC_LEN 8

// Blocks >= this size use RLE compression (v2 only)
#define RLE_THRESHOLD 64

// Pre-allocated buffer capacity for quick checkpoint accumulation (~8 MB)
// Must exceed 4 MB RAM + ROM content + peripheral state + per-block headers.
#define QUICK_BUF_CAPACITY (8 * 1024 * 1024)

// Persistent write buffer for quick checkpoints (allocated once, reused)
static uint8_t *g_quick_write_buf = NULL;

// === RLE Compression ===
// Format: sequence of chunks, each either:
//   LIT chunk: marker=0x00, uint32_t count, <count raw bytes>
//   RUN chunk: marker=0x01, uint32_t count, uint8_t value

// RLE-encode data into output buffer. Returns compressed size, 0 on overflow.
// If out is NULL, performs a dry run and returns the exact compressed size needed.
static size_t rle_encode(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_cap) {
    size_t op = 0;
    size_t i = 0;

    while (i < in_size) {
        // check for a run of 4+ identical bytes
        size_t rs = i;
        uint8_t val = in[i];
        while (i < in_size && in[i] == val && (i - rs) < 0xFFFFFFFFu)
            i++;
        size_t run_len = i - rs;

        if (run_len >= 4) {
            // emit RUN chunk: marker(1) + count(4) + value(1) = 6 bytes
            if (out) {
                if (op + 6 > out_cap)
                    return 0;
                out[op] = 0x01;
                uint32_t c = (uint32_t)run_len;
                memcpy(out + op + 1, &c, 4);
                out[op + 5] = val;
            }
            op += 6;
        } else {
            // collect literal bytes until the next run of 4+
            i = rs;
            size_t ls = i;
            while (i < in_size) {
                if (i + 3 < in_size && in[i] == in[i + 1] && in[i] == in[i + 2] && in[i] == in[i + 3])
                    break;
                i++;
                if ((i - ls) >= 0xFFFFFFFFu)
                    break;
            }
            size_t lit_len = i - ls;
            // emit LIT chunk: marker(1) + count(4) + data(lit_len)
            if (out) {
                if (op + 5 + lit_len > out_cap)
                    return 0;
                out[op] = 0x00;
                uint32_t c = (uint32_t)lit_len;
                memcpy(out + op + 1, &c, 4);
                memcpy(out + op + 5, in + ls, lit_len);
            }
            op += 5 + lit_len;
        }
    }
    return op;
}

// RLE-decode compressed data into output buffer. Returns true on success.
static bool rle_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    size_t ip = 0, op = 0;

    while (ip < in_size && op < out_size) {
        uint8_t marker = in[ip++];
        if (ip + 4 > in_size)
            return false;
        uint32_t count;
        memcpy(&count, in + ip, 4);
        ip += 4;

        if (marker == 0x01) {
            // RUN: fill count bytes with next byte value
            if (ip >= in_size)
                return false;
            uint8_t val = in[ip++];
            if (op + count > out_size)
                return false;
            memset(out + op, val, count);
            op += count;
        } else if (marker == 0x00) {
            // LIT: copy count raw bytes
            if (ip + count > in_size)
                return false;
            if (op + count > out_size)
                return false;
            memcpy(out + op, in + ip, count);
            ip += count;
            op += count;
        } else {
            return false;
        }
    }
    return (op == out_size);
}

// Internal checkpoint handle definition (opaque in headers via common.h)
struct checkpoint {
    FILE *file;
    bool is_writing;
    bool error;
    checkpoint_kind_t kind;
    // Buffered I/O for v3 quick format
    uint8_t *buf; // write-accumulation or decompressed-read buffer
    size_t buf_cap; // allocated capacity
    size_t buf_used; // bytes stored (write) or total decompressed size (read)
    size_t buf_pos; // read cursor position (read only)
    bool buf_owned; // true when buf was malloc'd and must be freed
};

// === v3 buffer helpers ===

// Append raw bytes to the accumulation buffer, return true on success
static bool buf_append(checkpoint_t *cp, const void *data, size_t len) {
    if (cp->buf_used + len > cp->buf_cap) {
        printf("Error: Quick checkpoint buffer overflow (%zu + %zu > %zu)\n", cp->buf_used, len, cp->buf_cap);
        cp->error = true;
        return false;
    }
    memcpy(cp->buf + cp->buf_used, data, len);
    cp->buf_used += len;
    return true;
}

// Read raw bytes from the decompressed buffer, return true on success
static bool buf_read(checkpoint_t *cp, void *data, size_t len) {
    if (cp->buf_pos + len > cp->buf_used) {
        printf("Error: Quick checkpoint buffer underflow (%zu + %zu > %zu)\n", cp->buf_pos, len, cp->buf_used);
        cp->error = true;
        return false;
    }
    memcpy(data, cp->buf + cp->buf_pos, len);
    cp->buf_pos += len;
    return true;
}

// === Block I/O ===

// Read a data block with size validation, source metadata, and RLE decompression
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *file, int line) {
    if (!checkpoint || checkpoint->error || checkpoint->is_writing) {
        printf("Error: Invalid checkpoint handle for reading\n");
        if (checkpoint)
            checkpoint->error = true;
        return;
    }

    // v3 buffered read: pull size header + raw data from decompressed buffer
    if (checkpoint->buf) {
        uint32_t stored_size = 0;
        if (!buf_read(checkpoint, &stored_size, sizeof(stored_size)))
            return;
        if ((size_t)stored_size != size) {
            printf("Error: v3 checkpoint size mismatch: expected %zu but got %u at %s:%d\n", size, stored_size,
                   file ? file : "(unknown)", line);
            checkpoint->error = true;
            return;
        }
        if (!buf_read(checkpoint, data, size))
            return;
        return;
    }

    // v2 per-block read path with file/line metadata

    // Read size header
    uint64_t stored_size = 0;
    size_t got = fread(&stored_size, 1, sizeof(stored_size), checkpoint->file);
    if (got != sizeof(stored_size)) {
        printf("Error: Failed to read size header from checkpoint (got %zu)\n", got);
        checkpoint->error = true;
        return;
    }

    // Read filename length and filename (for diagnostics)
    uint32_t fname_len = 0;
    got = fread(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (got != sizeof(fname_len)) {
        printf("Error: Failed to read filename length from checkpoint (got %zu)\n", got);
        checkpoint->error = true;
        return;
    }

    char *saved_file = NULL;
    if (fname_len > 0) {
        saved_file = (char *)malloc((size_t)fname_len + 1);
        if (!saved_file) {
            printf("Error: Out of memory reading checkpoint filename\n");
            checkpoint->error = true;
            return;
        }
        got = fread(saved_file, 1, fname_len, checkpoint->file);
        if (got != fname_len) {
            printf("Error: Failed to read filename from checkpoint (got %zu, expected %u)\n", got, fname_len);
            free(saved_file);
            checkpoint->error = true;
            return;
        }
        saved_file[fname_len] = '\0';
    }

    int32_t saved_line = 0;
    got = fread(&saved_line, 1, sizeof(saved_line), checkpoint->file);
    if (got != sizeof(saved_line)) {
        printf("Error: Failed to read saved line from checkpoint (got %zu)\n", got);
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    if ((uint64_t)size != stored_size) {
        printf("Error: Checkpoint size mismatch: expected %zu at %s:%d but file contains %llu at %s:%d\n", size,
               file ? file : "(unknown)", line, (unsigned long long)stored_size, saved_file ? saved_file : "(unknown)",
               saved_line);
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    // Read compression flag
    uint8_t flag = 0;
    got = fread(&flag, 1, 1, checkpoint->file);
    if (got != 1) {
        printf("Error: Failed to read compression flag from checkpoint\n");
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    if (flag == 0x00) {
        // Raw data: read directly
        size_t nread = fread(data, 1, size, checkpoint->file);
        if (nread != size) {
            printf("Error: Failed to read %zu bytes from checkpoint (got %zu)\n", size, nread);
            checkpoint->error = true;
        }
    } else if (flag == 0x01) {
        // RLE-compressed: read compressed size then compressed data, decode
        uint64_t comp_size = 0;
        got = fread(&comp_size, 1, sizeof(comp_size), checkpoint->file);
        if (got != sizeof(comp_size)) {
            printf("Error: Failed to read RLE compressed size from checkpoint\n");
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        uint8_t *comp_buf = (uint8_t *)malloc((size_t)comp_size);
        if (!comp_buf) {
            printf("Error: Out of memory for RLE decompression (%llu bytes)\n", (unsigned long long)comp_size);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        got = fread(comp_buf, 1, (size_t)comp_size, checkpoint->file);
        if (got != (size_t)comp_size) {
            printf("Error: Failed to read %llu RLE bytes from checkpoint (got %zu)\n", (unsigned long long)comp_size,
                   got);
            free(comp_buf);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        // Decode RLE into output buffer
        if (!rle_decode(comp_buf, (size_t)comp_size, (uint8_t *)data, size)) {
            printf("Error: RLE decompression failed for %zu-byte block at %s:%d\n", size, file ? file : "(unknown)",
                   line);
            free(comp_buf);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        free(comp_buf);
    } else {
        printf("Error: Unknown compression flag 0x%02x in checkpoint\n", flag);
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    if (saved_file)
        free(saved_file);
}

// Write a data block with size header, source metadata, and optional RLE compression
void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *file,
                                      int line) {
    if (!checkpoint || checkpoint->error || !checkpoint->is_writing) {
        printf("Error: Invalid checkpoint handle for writing\n");
        if (checkpoint)
            checkpoint->error = true;
        return;
    }

    // v3 buffered write: append size + raw data to accumulation buffer
    if (checkpoint->buf) {
        uint32_t sz = (uint32_t)size;
        buf_append(checkpoint, &sz, sizeof(sz));
        buf_append(checkpoint, data, size);
        return;
    }

    // v2 per-block write path with file/line metadata and per-block RLE

    // Write 64-bit uncompressed size header for validation on read
    uint64_t store_size = (uint64_t)size;
    size_t w = fwrite(&store_size, 1, sizeof(store_size), checkpoint->file);
    if (w != sizeof(store_size)) {
        printf("Error: Failed to write size header to checkpoint (wrote %zu)\n", w);
        checkpoint->error = true;
        return;
    }

    // Write filename length and filename bytes, then the line number
    uint32_t fname_len = file ? (uint32_t)strlen(file) : 0;
    w = fwrite(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (w != sizeof(fname_len)) {
        printf("Error: Failed to write filename length to checkpoint (wrote %zu)\n", w);
        checkpoint->error = true;
        return;
    }
    if (fname_len > 0) {
        w = fwrite(file, 1, fname_len, checkpoint->file);
        if (w != fname_len) {
            printf("Error: Failed to write filename to checkpoint (wrote %zu)\n", w);
            checkpoint->error = true;
            return;
        }
    }

    int32_t iline = (int32_t)line;
    w = fwrite(&iline, 1, sizeof(iline), checkpoint->file);
    if (w != sizeof(iline)) {
        printf("Error: Failed to write line number to checkpoint (wrote %zu)\n", w);
        checkpoint->error = true;
        return;
    }

    // Choose raw vs RLE based on block size
    if (size < RLE_THRESHOLD) {
        // Small block: flag=0 then raw data
        uint8_t flag = 0x00;
        w = fwrite(&flag, 1, 1, checkpoint->file);
        if (w != 1) {
            checkpoint->error = true;
            return;
        }
        size_t written = fwrite(data, 1, size, checkpoint->file);
        if (written != size) {
            printf("Error: Failed to write %zu bytes to checkpoint (wrote %zu)\n", size, written);
            checkpoint->error = true;
        }
    } else {
        // Large block: flag=1 then RLE-compressed data
        // First pass: compute exact compressed size (dry run with NULL output)
        size_t comp_size = rle_encode((const uint8_t *)data, size, NULL, 0);
        uint8_t *comp_buf = (uint8_t *)malloc(comp_size);
        if (!comp_buf) {
            printf("Error: Out of memory for RLE compression (%zu bytes)\n", comp_size);
            checkpoint->error = true;
            return;
        }
        // Second pass: encode into buffer
        rle_encode((const uint8_t *)data, size, comp_buf, comp_size);

        // Write flag + compressed size + compressed data
        uint8_t flag = 0x01;
        w = fwrite(&flag, 1, 1, checkpoint->file);
        if (w != 1) {
            free(comp_buf);
            checkpoint->error = true;
            return;
        }
        uint64_t cs = (uint64_t)comp_size;
        w = fwrite(&cs, 1, sizeof(cs), checkpoint->file);
        if (w != sizeof(cs)) {
            free(comp_buf);
            checkpoint->error = true;
            return;
        }
        w = fwrite(comp_buf, 1, comp_size, checkpoint->file);
        if (w != comp_size) {
            printf("Error: Failed to write %zu RLE bytes to checkpoint (wrote %zu)\n", comp_size, w);
            free(comp_buf);
            checkpoint->error = true;
            return;
        }
        free(comp_buf);
    }
}

// === Handle Management ===

// Open a checkpoint file for reading (auto-detects v2 or v3 format)
checkpoint_t *checkpoint_open_read(const char *filename) {
    checkpoint_t *cp = (checkpoint_t *)malloc(sizeof(struct checkpoint));
    if (!cp)
        return NULL;

    cp->file = fopen(filename, "rb");
    if (!cp->file) {
        free(cp);
        return NULL;
    }

    // Initialize buffer fields
    cp->buf = NULL;
    cp->buf_cap = 0;
    cp->buf_used = 0;
    cp->buf_pos = 0;
    cp->buf_owned = false;

    // Read magic signature to detect format version
    char magic[CHECKPOINT_MAGIC_LEN];
    size_t got = fread(magic, 1, CHECKPOINT_MAGIC_LEN, cp->file);
    if (got != CHECKPOINT_MAGIC_LEN) {
        printf("Error: %s is not a valid checkpoint (too short)\n", filename);
        fclose(cp->file);
        free(cp);
        return NULL;
    }

    if (memcmp(magic, CHECKPOINT_MAGIC_V3, CHECKPOINT_MAGIC_LEN) == 0) {
        // v3 quick format: read build ID, then sizes, decompress entire payload into buffer
        char file_build_id[BUILD_ID_LEN + 1];
        got = fread(file_build_id, 1, BUILD_ID_LEN, cp->file);
        if (got != BUILD_ID_LEN) {
            printf("Error: Failed to read build ID from %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        file_build_id[BUILD_ID_LEN] = '\0';
        // Validate build ID matches the running application
        if (memcmp(file_build_id, get_build_id(), BUILD_ID_LEN) != 0) {
            printf("Error: Checkpoint build ID mismatch in %s\n", filename);
            printf("  checkpoint: %s\n", file_build_id);
            printf("  current:    %s\n", get_build_id());
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        uint64_t uncompressed_size = 0, compressed_size = 0;
        got = fread(&uncompressed_size, 1, sizeof(uncompressed_size), cp->file);
        if (got != sizeof(uncompressed_size)) {
            printf("Error: Failed to read v3 uncompressed size from %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        got = fread(&compressed_size, 1, sizeof(compressed_size), cp->file);
        if (got != sizeof(compressed_size)) {
            printf("Error: Failed to read v3 compressed size from %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // Allocate and read compressed data
        uint8_t *comp_buf = (uint8_t *)malloc((size_t)compressed_size);
        if (!comp_buf) {
            printf("Error: Out of memory for v3 decompression (%llu bytes)\n", (unsigned long long)compressed_size);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        got = fread(comp_buf, 1, (size_t)compressed_size, cp->file);
        if (got != (size_t)compressed_size) {
            printf("Error: Failed to read v3 compressed data from %s\n", filename);
            free(comp_buf);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // Allocate decompressed buffer and decode
        cp->buf = (uint8_t *)malloc((size_t)uncompressed_size);
        if (!cp->buf) {
            printf("Error: Out of memory for v3 decompressed data (%llu bytes)\n",
                   (unsigned long long)uncompressed_size);
            free(comp_buf);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // If compressed_size == uncompressed_size, data is uncompressed (RLE disabled)
        if (compressed_size == uncompressed_size) {
            memcpy(cp->buf, comp_buf, (size_t)uncompressed_size);
        } else {
            // RLE-compressed data: decode
            if (!rle_decode(comp_buf, (size_t)compressed_size, cp->buf, (size_t)uncompressed_size)) {
                printf("Error: v3 RLE decompression failed for %s\n", filename);
                free(comp_buf);
                free(cp->buf);
                fclose(cp->file);
                free(cp);
                return NULL;
            }
        }
        free(comp_buf);
        cp->buf_cap = (size_t)uncompressed_size;
        cp->buf_used = (size_t)uncompressed_size;
        cp->buf_pos = 0;
        cp->buf_owned = true;
        // Close the file early; all data is in the buffer
        fclose(cp->file);
        cp->file = NULL;
        cp->kind = CHECKPOINT_KIND_QUICK;
    } else if (memcmp(magic, CHECKPOINT_MAGIC_V2, CHECKPOINT_MAGIC_LEN) == 0) {
        // v2 consolidated format: read build ID, then per-block streaming from file
        char file_build_id[BUILD_ID_LEN + 1];
        got = fread(file_build_id, 1, BUILD_ID_LEN, cp->file);
        if (got != BUILD_ID_LEN) {
            printf("Error: Failed to read build ID from %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        file_build_id[BUILD_ID_LEN] = '\0';
        // Validate build ID matches the running application
        if (memcmp(file_build_id, get_build_id(), BUILD_ID_LEN) != 0) {
            printf("Error: Checkpoint build ID mismatch in %s\n", filename);
            printf("  checkpoint: %s\n", file_build_id);
            printf("  current:    %s\n", get_build_id());
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        cp->kind = CHECKPOINT_KIND_CONSOLIDATED;
    } else {
        printf("Error: %s is not a valid Granny Smith checkpoint (bad signature)\n", filename);
        fclose(cp->file);
        free(cp);
        return NULL;
    }

    cp->is_writing = false;
    cp->error = false;
    return cp;
}

// Open a checkpoint file for writing
checkpoint_t *checkpoint_open_write(const char *filename, checkpoint_kind_t kind) {
    checkpoint_t *cp = (checkpoint_t *)malloc(sizeof(struct checkpoint));
    if (!cp)
        return NULL;

    cp->file = fopen(filename, "wb");
    if (!cp->file) {
        free(cp);
        return NULL;
    }

    cp->is_writing = true;
    cp->error = false;
    cp->kind = kind;

    // Initialize buffer fields
    cp->buf = NULL;
    cp->buf_cap = 0;
    cp->buf_used = 0;
    cp->buf_pos = 0;
    cp->buf_owned = false;

    if (kind == CHECKPOINT_KIND_QUICK) {
        // v3 quick: accumulate data into pre-allocated buffer, write at close
        if (!g_quick_write_buf) {
            g_quick_write_buf = (uint8_t *)malloc(QUICK_BUF_CAPACITY);
            if (!g_quick_write_buf) {
                printf("Error: Failed to allocate quick checkpoint buffer (%d bytes)\n", QUICK_BUF_CAPACITY);
                fclose(cp->file);
                free(cp);
                return NULL;
            }
        }
        cp->buf = g_quick_write_buf;
        cp->buf_cap = QUICK_BUF_CAPACITY;
        cp->buf_used = 0;
        cp->buf_owned = false; // static buffer, not freed on close
    } else {
        // v2 consolidated: write magic + build ID immediately, data streamed per-block
        if (fwrite(CHECKPOINT_MAGIC_V2, 1, CHECKPOINT_MAGIC_LEN, cp->file) != CHECKPOINT_MAGIC_LEN) {
            printf("Error: Failed to write checkpoint signature to %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // Write build ID right after the magic signature
        if (fwrite(get_build_id(), 1, BUILD_ID_LEN, cp->file) != BUILD_ID_LEN) {
            printf("Error: Failed to write build ID to %s\n", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
    }

    return cp;
}

// Get the checkpoint kind (consolidated vs quick)
checkpoint_kind_t checkpoint_get_kind(checkpoint_t *checkpoint) {
    if (!checkpoint)
        return CHECKPOINT_KIND_CONSOLIDATED;
    return checkpoint->kind;
}

// Close a checkpoint and free its resources
void checkpoint_close(checkpoint_t *checkpoint) {
    if (!checkpoint)
        return;

    // v3 quick write: write buffer directly (RLE temporarily disabled for testing)
    if (checkpoint->is_writing && checkpoint->buf && !checkpoint->error) {
        size_t raw_size = checkpoint->buf_used;
        // Write v3 header: magic + build ID + uncompressed_size + compressed_size
        fwrite(CHECKPOINT_MAGIC_V3, 1, CHECKPOINT_MAGIC_LEN, checkpoint->file);
        fwrite(get_build_id(), 1, BUILD_ID_LEN, checkpoint->file);
        uint64_t uc = (uint64_t)raw_size;
        fwrite(&uc, 1, sizeof(uc), checkpoint->file);
        uint64_t cs = (uint64_t)raw_size; // "compressed" size = raw size (no compression)
        fwrite(&cs, 1, sizeof(cs), checkpoint->file);
        // Write raw payload directly in one call
        size_t w = fwrite(checkpoint->buf, 1, raw_size, checkpoint->file);
        if (w != raw_size) {
            printf("Error: v3 write failed (wrote %zu of %zu)\n", w, raw_size);
            checkpoint->error = true;
        }
    }

    // Free owned buffer (v3 read mode allocates its own buffer)
    if (checkpoint->buf_owned && checkpoint->buf) {
        free(checkpoint->buf);
    }

    if (checkpoint->file) {
        fclose(checkpoint->file);
    }
    free(checkpoint);
}

// Check if a checkpoint encountered an error during read/write
bool checkpoint_has_error(checkpoint_t *checkpoint) {
    return checkpoint ? checkpoint->error : true;
}

// Flag the checkpoint as having encountered an error
void checkpoint_set_error(checkpoint_t *checkpoint) {
    if (checkpoint)
        checkpoint->error = true;
}

// === File Serialization ===

// Save mode: whether files are stored by value (contents) or by reference (path only)
static bool g_checkpoint_files_as_refs = false;

void checkpoint_set_files_as_refs(bool refs) {
    g_checkpoint_files_as_refs = refs;
}

bool checkpoint_get_files_as_refs(void) {
    return g_checkpoint_files_as_refs;
}

// Write a file to the checkpoint (either content or reference mode)
void checkpoint_write_file_loc(checkpoint_t *checkpoint, const char *path, const char *file, int line) {
    if (!checkpoint || checkpoint->error || !checkpoint->is_writing) {
        printf("Error: Invalid checkpoint handle for writing (file block)\n");
        if (checkpoint)
            checkpoint->error = true;
        return;
    }

    // v3 buffered write: ref-only for persistent files, embed content for volatile files
    if (checkpoint->buf) {
        uint32_t name_len = (path && path[0]) ? (uint32_t)strlen(path) : 0;
        buf_append(checkpoint, &name_len, sizeof(name_len));
        if (name_len) {
            buf_append(checkpoint, path, name_len);
        }
        // Persistent paths (/persist/) survive page reload; volatile paths do not
        bool persistent = (path && strncmp(path, "/persist/", 9) == 0);
        uint8_t has_content = (!persistent && path && path[0]) ? 1 : 0;
        buf_append(checkpoint, &has_content, 1);
        if (has_content) {
            // Read file content into the buffer (ROM is ~128-256 KB, negligible)
            uint64_t content_size = 0;
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                if (sz > 0)
                    content_size = (uint64_t)sz;
                fseek(f, 0, SEEK_SET);
            }
            buf_append(checkpoint, &content_size, sizeof(content_size));
            if (f && content_size > 0) {
                uint8_t fbuf[8192];
                size_t r;
                while ((r = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
                    buf_append(checkpoint, fbuf, r);
                }
                fclose(f);
            } else if (f) {
                fclose(f);
            }
        }
        return;
    }

    // v2 per-block write: content or reference based on global setting

    bool include_content = !g_checkpoint_files_as_refs;
    uint64_t payload_size = 0;
    uint64_t content_size = 0;
    uint32_t name_len = (path && path[0]) ? (uint32_t)strlen(path) : 0;

    if (include_content) {
        if (path && path[0]) {
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                if (sz > 0)
                    content_size = (uint64_t)sz;
                fclose(f);
            }
        }
        payload_size = 4 + name_len + 1 + 8 + content_size;
    } else {
        payload_size = 4 + name_len + 1;
    }

    // Write outer header
    size_t w;
    w = fwrite(&payload_size, 1, sizeof(payload_size), checkpoint->file);
    if (w != sizeof(payload_size)) {
        checkpoint->error = true;
        return;
    }
    uint32_t fname_len = file ? (uint32_t)strlen(file) : 0;
    w = fwrite(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (w != sizeof(fname_len)) {
        checkpoint->error = true;
        return;
    }
    if (fname_len) {
        w = fwrite(file, 1, fname_len, checkpoint->file);
        if (w != fname_len) {
            checkpoint->error = true;
            return;
        }
    }
    int32_t iline = (int32_t)line;
    w = fwrite(&iline, 1, sizeof(iline), checkpoint->file);
    if (w != sizeof(iline)) {
        checkpoint->error = true;
        return;
    }

    // Write payload: name length and name bytes
    w = fwrite(&name_len, 1, sizeof(name_len), checkpoint->file);
    if (w != sizeof(name_len)) {
        checkpoint->error = true;
        return;
    }
    if (name_len) {
        w = fwrite(path, 1, name_len, checkpoint->file);
        if (w != name_len) {
            checkpoint->error = true;
            return;
        }
    }
    // Content flag
    uint8_t has_content = include_content ? 1 : 0;
    w = fwrite(&has_content, 1, 1, checkpoint->file);
    if (w != 1) {
        checkpoint->error = true;
        return;
    }
    // Optional content
    if (has_content) {
        w = fwrite(&content_size, 1, sizeof(content_size), checkpoint->file);
        if (w != sizeof(content_size)) {
            checkpoint->error = true;
            return;
        }
        if (content_size > 0) {
            FILE *f = fopen(path, "rb");
            if (!f) {
                size_t chunk = 4096;
                static uint8_t zbuf[4096];
                uint64_t remaining = content_size;
                while (remaining > 0) {
                    size_t to_write = (remaining > chunk) ? chunk : (size_t)remaining;
                    size_t wr = fwrite(zbuf, 1, to_write, checkpoint->file);
                    if (wr != to_write) {
                        checkpoint->error = true;
                        return;
                    }
                    remaining -= wr;
                }
            } else {
                uint8_t buf[8192];
                size_t r;
                while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
                    size_t wr = fwrite(buf, 1, r, checkpoint->file);
                    if (wr != r) {
                        fclose(f);
                        checkpoint->error = true;
                        return;
                    }
                }
                fclose(f);
            }
        }
    }
}

// Read a file block from the checkpoint into a buffer
size_t checkpoint_read_file_loc(checkpoint_t *checkpoint, uint8_t *dest, size_t capacity, char **out_path,
                                const char *file, int line) {
    if (out_path)
        *out_path = NULL;
    if (!checkpoint || checkpoint->error || checkpoint->is_writing) {
        printf("Error: Invalid checkpoint handle for reading (file block)\n");
        if (checkpoint)
            checkpoint->error = true;
        return 0;
    }

    // v3 buffered read: path + optional content from decompressed buffer
    if (checkpoint->buf) {
        uint32_t name_len = 0;
        if (!buf_read(checkpoint, &name_len, sizeof(name_len)))
            return 0;
        char *name = NULL;
        if (name_len) {
            name = (char *)malloc(name_len + 1);
            if (!name) {
                checkpoint->error = true;
                return 0;
            }
            if (!buf_read(checkpoint, name, name_len)) {
                free(name);
                return 0;
            }
            name[name_len] = '\0';
        }
        if (out_path)
            *out_path = name;
        else if (name)
            free(name);

        // Has embedded content? (volatile files are embedded, persistent are ref-only)
        uint8_t has_content = 0;
        if (!buf_read(checkpoint, &has_content, 1))
            return 0;

        size_t loaded = 0;
        if (has_content) {
            // Read embedded content from the buffer
            uint64_t content_size = 0;
            if (!buf_read(checkpoint, &content_size, sizeof(content_size)))
                return 0;
            if (content_size > 0 && dest && capacity > 0) {
                size_t to_copy = ((size_t)content_size > capacity) ? capacity : (size_t)content_size;
                if (!buf_read(checkpoint, dest, to_copy))
                    return loaded;
                loaded = to_copy;
                // Skip any excess content beyond dest capacity
                size_t excess = (size_t)content_size - to_copy;
                if (excess > 0)
                    checkpoint->buf_pos += excess;
            } else if (content_size > 0) {
                // Skip content we can't store (no dest buffer)
                checkpoint->buf_pos += (size_t)content_size;
            }
        } else {
            // Reference-only: read content from the file on disk
            if (out_path && *out_path && **out_path && dest && capacity > 0) {
                FILE *f = fopen(*out_path, "rb");
                if (f) {
                    loaded = fread(dest, 1, capacity, f);
                    fclose(f);
                }
            }
        }
        return loaded;
    }

    // v2 per-block read path
    (void)file;
    (void)line;

    // Read outer header
    uint64_t stored_size = 0;
    size_t got = fread(&stored_size, 1, sizeof(stored_size), checkpoint->file);
    if (got != sizeof(stored_size)) {
        checkpoint->error = true;
        return 0;
    }
    uint32_t fname_len = 0;
    got = fread(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (got != sizeof(fname_len)) {
        checkpoint->error = true;
        return 0;
    }
    if (fname_len) {
        char *saved_file = (char *)malloc(fname_len + 1);
        if (!saved_file) {
            checkpoint->error = true;
            return 0;
        }
        got = fread(saved_file, 1, fname_len, checkpoint->file);
        if (got != fname_len) {
            free(saved_file);
            checkpoint->error = true;
            return 0;
        }
        saved_file[fname_len] = '\0';
        free(saved_file);
    }
    int32_t saved_line = 0;
    got = fread(&saved_line, 1, sizeof(saved_line), checkpoint->file);
    if (got != sizeof(saved_line)) {
        checkpoint->error = true;
        return 0;
    }

    // Read payload (reference always first)
    uint32_t name_len2 = 0;
    got = fread(&name_len2, 1, sizeof(name_len2), checkpoint->file);
    if (got != sizeof(name_len2)) {
        checkpoint->error = true;
        return 0;
    }
    char *name = NULL;
    if (name_len2) {
        name = (char *)malloc(name_len2 + 1);
        if (!name) {
            checkpoint->error = true;
            return 0;
        }
        got = fread(name, 1, name_len2, checkpoint->file);
        if (got != name_len2) {
            free(name);
            checkpoint->error = true;
            return 0;
        }
        name[name_len2] = '\0';
    }
    if (out_path)
        *out_path = name;
    else if (name)
        free(name);

    // Has content?
    uint8_t has_content = 0;
    got = fread(&has_content, 1, 1, checkpoint->file);
    if (got != 1) {
        checkpoint->error = true;
        return 0;
    }

    size_t loaded = 0;
    if (has_content) {
        uint64_t content_size = 0;
        got = fread(&content_size, 1, sizeof(content_size), checkpoint->file);
        if (got != sizeof(content_size)) {
            checkpoint->error = true;
            return 0;
        }
        uint64_t remaining = content_size;
        uint8_t buf[8192];
        while (remaining > 0) {
            size_t to_read = (remaining > sizeof(buf)) ? sizeof(buf) : (size_t)remaining;
            size_t r = fread(buf, 1, to_read, checkpoint->file);
            if (r != to_read) {
                checkpoint->error = true;
                return loaded;
            }
            if (dest && loaded < capacity) {
                size_t can_copy = capacity - loaded;
                if (can_copy > r)
                    can_copy = r;
                memcpy(dest + loaded, buf, can_copy);
                loaded += can_copy;
            }
            remaining -= r;
        }
    } else {
        // Reference-only: try to read from file for convenience
        if (out_path && *out_path && **out_path && dest && capacity > 0) {
            FILE *f = fopen(*out_path, "rb");
            if (f) {
                loaded = fread(dest, 1, capacity, f);
                fclose(f);
            }
        }
    }
    return loaded;
}

// Validate that a checkpoint file's build ID matches the current build.
// Opens the file, reads magic + build ID, compares with the running application.
// Returns true if the build IDs match, false on mismatch or I/O error.
bool checkpoint_validate_build_id(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f)
        return false;

    // Read and verify magic signature
    char magic[CHECKPOINT_MAGIC_LEN];
    if (fread(magic, 1, CHECKPOINT_MAGIC_LEN, f) != CHECKPOINT_MAGIC_LEN) {
        fclose(f);
        return false;
    }
    if (memcmp(magic, CHECKPOINT_MAGIC_V2, CHECKPOINT_MAGIC_LEN) != 0 &&
        memcmp(magic, CHECKPOINT_MAGIC_V3, CHECKPOINT_MAGIC_LEN) != 0) {
        fclose(f);
        return false;
    }

    // Read build ID (immediately follows magic in both formats)
    char file_build_id[BUILD_ID_LEN];
    if (fread(file_build_id, 1, BUILD_ID_LEN, f) != BUILD_ID_LEN) {
        fclose(f);
        return false;
    }
    fclose(f);

    // Compare with the current application's build ID
    return memcmp(file_build_id, get_build_id(), BUILD_ID_LEN) == 0;
}
