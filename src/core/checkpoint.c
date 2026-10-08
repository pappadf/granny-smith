// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint.c
// Checkpoint file I/O: open/close, block read/write, file serialization.
// Extracted from system.c to support multi-machine checkpoint handling.
//
// Two on-disk formats, told apart by the version byte after the "GSCHKPT"
// magic:
//   v2 (GSCHKPT2) — per-block RLE with file/line metadata; used for consolidated checkpoints
//   v3 (GSCHKPT3) — one raw (uncompressed) payload, no per-block metadata; used for quick checkpoints

#include "checkpoint.h"
#include "gs_out.h"

#include "build_id.h"
#include "system.h"
#include "debug/log.h"
#include "io/io_worker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A checkpoint file starts with an 8-byte signature: the magic "GSCHKPT"
// and one ASCII version digit.  A new format is a new digit, not a new
// magic (the web app recognises a checkpoint by the magic alone).
static const char CHECKPOINT_MAGIC[] = "GSCHKPT";
#define CHECKPOINT_MAGIC_BYTES 7
#define CHECKPOINT_VERSION_V2  '2' // consolidated (full export)
#define CHECKPOINT_VERSION_V3  '3' // quick (background auto-save)

// The whole signature: magic + version
#define CHECKPOINT_MAGIC_LEN (CHECKPOINT_MAGIC_BYTES + 1)

// Write the signature for `version` into sig[CHECKPOINT_MAGIC_LEN].
static void checkpoint_signature(char *sig, char version) {
    memcpy(sig, CHECKPOINT_MAGIC, CHECKPOINT_MAGIC_BYTES);
    sig[CHECKPOINT_MAGIC_BYTES] = version;
}

// The version digit of a signature, or 0 when it is not a checkpoint's
// (or names a version this build does not read).
static char checkpoint_signature_version(const char *sig) {
    if (memcmp(sig, CHECKPOINT_MAGIC, CHECKPOINT_MAGIC_BYTES) != 0)
        return 0;
    char v = sig[CHECKPOINT_MAGIC_BYTES];
    return (v == CHECKPOINT_VERSION_V2 || v == CHECKPOINT_VERSION_V3) ? v : 0;
}

LOG_USE_CATEGORY_NAME("ckpt");

// Blocks >= this size use RLE compression (v2 only).  A compressed block
// costs a 9-byte flag + size header and 5-6 bytes per chunk, so below a few
// dozen bytes RLE cannot win and only adds a second pass; most small v2
// blocks are scalar device fields of a handful of bytes.  64 sits safely
// above that break-even; it is not tuned beyond it.
#define RLE_THRESHOLD 64

// Pre-allocated buffer capacity for quick checkpoint accumulation (~8 MB)
// Must exceed 4 MB RAM + ROM content + peripheral state + per-block headers.
#define QUICK_BUF_CAPACITY (8 * 1024 * 1024)

// Upper bound on a single read-path malloc driven by an on-disk size field.
// Real checkpoints are bounded by RAM — 512 MiB on the largest supported
// machine (the Apple Network Server), whose barely-compressible RAM block
// RLE-encodes to just over its raw size; 1 GiB is roughly 2x worst-case.
// A corrupt header claiming more is rejected rather than driving an OOM
// on 32-bit (WASM) builds.
#define CHECKPOINT_MAX_ALLOC ((size_t)1024 * 1024 * 1024)

// Persistent write buffer for quick checkpoints (allocated once, reused,
// freed by checkpoint_delete).  A quick write borrows it at open and hands
// it back -- grown, if buf_append had to -- at close, so its capacity is
// what the largest save so far needed: a machine larger than 8 MB pays one
// regrow on its first save, not on every save.
static uint8_t *g_quick_write_buf = NULL;
static size_t g_quick_write_cap = 0;

// stdio buffer for a checkpoint FILE.  A consolidated (v2) checkpoint is a
// stream of small records -- a disk is one per 512-byte block -- and with
// stdio's default ~1 KB buffer every couple of records was a filesystem
// call.  Under WasmFS/OPFS each of those is a synchronous access-handle
// round trip, which made saving or opening a Save State of a machine with
// a hard disk take minutes.  The buffer is allocated with the handle (the
// flexible tail of struct checkpoint), so every path that frees the handle
// after fclose frees it too.
#define CHECKPOINT_STDIO_BUFFER_BYTES (1u * 1024 * 1024)

// The quick save's header, laid down at the front of the buffer at close
// so the whole file is one buffer the I/O worker can write and publish.
#define QUICK_HDR_LEN (CHECKPOINT_MAGIC_LEN + BUILD_ID_LEN + 8 + 8)

// The publish the next quick save will end with (checkpoint_publish_next),
// and the one in flight on the worker: while it is, the buffer is the
// worker's and a save that comes due is skipped and counted.
static char g_publish_final[1024];
static char g_publish_tmp[1024];
static bool g_quick_in_flight = false;
static uint32_t g_quick_skipped = 0;

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
        if (in_size - ip < 4)
            return false;
        uint32_t count;
        memcpy(&count, in + ip, 4);
        ip += 4;

        // Every bound below is `count > remaining`, never `cursor + count >
        // size`.  `count` is a uint32_t taken straight from the file and
        // size_t is 32 bits on the shipping wasm build, so the sum form wraps:
        // with op = 16 and count = 0xFFFFFFF8 it evaluates to 8, which passes
        // a 64-byte bound and admits a four-gigabyte memcpy.  The subtraction
        // form cannot wrap because op <= out_size and ip <= in_size are loop
        // invariants.  (Not reproducible on a 64-bit host, where the sum is
        // computed in 64 bits and is correct -- see tests/unit/suites/checkpoint.)
        if (marker == 0x01) {
            // RUN: fill count bytes with next byte value
            if (ip >= in_size)
                return false;
            uint8_t val = in[ip++];
            if (count > out_size - op)
                return false;
            memset(out + op, val, count);
            op += count;
        } else if (marker == 0x00) {
            // LIT: copy count raw bytes
            if (count > in_size - ip)
                return false;
            if (count > out_size - op)
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
    // Consolidated write: the file is written as `tmp_path` and renamed
    // over `final_path` at a clean close (removed otherwise), so a failed
    // save never leaves a truncated file where a good one was.
    char *final_path;
    char *tmp_path;
    // stdio buffer for `file` (CHECKPOINT_STDIO_BUFFER_BYTES; present only
    // when the handle was allocated with a FILE to stream)
    char stdio_buf[];
};

// === v3 buffer helpers ===

// Append raw bytes to the accumulation buffer, return true on success
static bool buf_append(checkpoint_t *cp, const void *data, size_t len) {
    if (cp->buf_used + len > cp->buf_cap) {
        // Grow the buffer to fit (double or exact fit, whichever is larger)
        size_t needed = cp->buf_used + len;
        size_t new_cap = cp->buf_cap * 2;
        if (new_cap < needed)
            new_cap = needed;
        uint8_t *new_buf = (uint8_t *)realloc(cp->buf, new_cap);
        if (!new_buf) {
            LOG(0, "Error: Quick checkpoint buffer realloc failed (%zu bytes)", new_cap);
            cp->error = true;
            return false;
        }
        cp->buf = new_buf;
        cp->buf_cap = new_cap;
        // A borrowed quick buffer (not buf_owned) is handed back, grown, at
        // checkpoint_close.
    }
    memcpy(cp->buf + cp->buf_used, data, len);
    cp->buf_used += len;
    return true;
}

// Read raw bytes from the decompressed buffer, return true on success
static bool buf_read(checkpoint_t *cp, void *data, size_t len) {
    if (cp->buf_pos + len > cp->buf_used) {
        LOG(0, "Error: Quick checkpoint buffer underflow (%zu + %zu > %zu)", cp->buf_pos, len, cp->buf_used);
        cp->error = true;
        return false;
    }
    memcpy(data, cp->buf + cp->buf_pos, len);
    cp->buf_pos += len;
    return true;
}

// === Bounded reads from an untrusted stream =================================
//
// Every length in a checkpoint comes off disk, and a checkpoint is a file the
// user supplies.  The helpers here exist so that adding the next
// variable-length field cannot reintroduce the same three bugs: an unbounded
// allocation driven by an on-disk count, a string used without a terminator
// the writer merely promised, and a loop bound taken from the file.  Read a
// count through checkpoint_read_count(), a string
// through checkpoint_read_string(), and both are correct by construction.

// Longest diagnostic filename a block header may claim.  These are __FILE__
// strings naming the save site; nothing in the tree is close, and the cap is
// what stops a crafted 4 GB request on the 32-bit wasm heap.
#define CP_MAX_FNAME 4096u

// Longest path a restore may claim for an image or its delta directory.
#define CP_MAX_PATH 4096u

// Read the v2 block header's `uint32 length + bytes` filename directly from
// the FILE, below the system_read_checkpoint_data layer.  Returns NULL both
// for "absent" (length 0) and for "refused"; the caller distinguishes by
// checking checkpoint->error, which is set only in the second case.  The
// result is always NUL-terminated.
static char *cp_read_block_fname(checkpoint_t *checkpoint) {
    uint32_t fname_len = 0;
    size_t got = fread(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (got != sizeof(fname_len)) {
        LOG(0, "Error: Failed to read filename length from checkpoint (got %zu)", got);
        checkpoint->error = true;
        return NULL;
    }
    if (fname_len == 0)
        return NULL;
    if (fname_len > CP_MAX_FNAME) {
        LOG(0, "Error: checkpoint block claims a %u-byte filename (cap %u); refusing", fname_len, CP_MAX_FNAME);
        checkpoint->error = true;
        return NULL;
    }
    char *saved_file = (char *)malloc((size_t)fname_len + 1);
    if (!saved_file) {
        LOG(0, "Error: Out of memory reading checkpoint filename");
        checkpoint->error = true;
        return NULL;
    }
    got = fread(saved_file, 1, fname_len, checkpoint->file);
    if (got != fname_len) {
        LOG(0, "Error: Failed to read filename from checkpoint (got %zu, expected %u)", got, fname_len);
        free(saved_file);
        checkpoint->error = true;
        return NULL;
    }
    saved_file[fname_len] = '\0';
    return saved_file;
}

// FNV-1a over the block name.  The hash only has to separate the names a
// single machine uses, and a collision costs a missed diagnostic rather than
// wrong behaviour -- the size check still stands behind it.
static uint32_t cp_tag_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h ? h : 1u; // 0 is reserved for "unchecked"
}

// Compare a stored tag against what the caller expects.  0 on either side
// means "unchecked", so a block can gain a name on the write and read paths
// independently.  Returns false and flags the checkpoint on a real mismatch.
static bool cp_tag_ok(checkpoint_t *checkpoint, uint32_t stored, const char *tag, const char *file, int line) {
    if (!tag || stored == 0)
        return true;
    uint32_t want = cp_tag_hash(tag);
    if (stored == want)
        return true;
    LOG(0,
        "Error: checkpoint block order diverges -- reading '%s' at %s:%d, but the stream holds a different block "
        "here (tag %08x, expected %08x). The save and restore orders disagree.",
        tag, file ? file : "(unknown)", line, stored, want);
    checkpoint_set_error(checkpoint);
    return false;
}

bool checkpoint_read_count(checkpoint_t *checkpoint, uint32_t *out, uint32_t max, const char *what) {
    *out = 0;
    uint32_t v = 0;
    system_read_checkpoint_data(checkpoint, &v, sizeof(v));
    if (checkpoint_has_error(checkpoint))
        return false;
    if (v > max) {
        LOG(0, "Error: checkpoint claims %u %s (cap %u); refusing the restore", v, what ? what : "items", max);
        checkpoint_set_error(checkpoint);
        return false;
    }
    *out = v;
    return true;
}

void checkpoint_write_string(checkpoint_t *checkpoint, const char *s) {
    uint32_t len = (s && *s) ? (uint32_t)strlen(s) + 1 : 0;
    system_write_checkpoint_data(checkpoint, &len, sizeof(len));
    if (len)
        system_write_checkpoint_data(checkpoint, s, len);
}

char *checkpoint_read_string(checkpoint_t *checkpoint, uint32_t max, const char *what) {
    uint32_t len = 0;
    if (!checkpoint_read_count(checkpoint, &len, max, what))
        return NULL;
    if (len == 0)
        return NULL;
    // One byte more than claimed, and terminate unconditionally: the writer
    // includes its own NUL in `len`, but a hostile file need not, and the
    // result is handed to access(), fopen() and gs_outf("%s").
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        LOG(0, "Error: out of memory reading a %u-byte %s from checkpoint", len, what ? what : "string");
        checkpoint_set_error(checkpoint);
        return NULL;
    }
    system_read_checkpoint_data(checkpoint, buf, len);
    if (checkpoint_has_error(checkpoint)) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

// === Block I/O ===

// Read a data block with size validation, source metadata, and RLE decompression
static void read_checkpoint_block(checkpoint_t *checkpoint, void *data, size_t size, const char *tag, const char *file,
                                  int line) {
    if (checkpoint && checkpoint->error && !checkpoint->is_writing) {
        // The stream has already failed and said why; the rest of the
        // restore reads zeros, quietly, rather than one error per field.
        memset(data, 0, size);
        return;
    }
    if (!checkpoint || checkpoint->is_writing) {
        LOG(0, "Error: Invalid checkpoint handle for reading");
        if (checkpoint)
            checkpoint->error = true;
        return;
    }

    // v3 buffered read: pull size header + raw data from decompressed buffer
    if (checkpoint->buf) {
        uint32_t stored_size = 0;
        if (!buf_read(checkpoint, &stored_size, sizeof(stored_size)))
            return;
        uint32_t stored_tag = 0;
        if (!buf_read(checkpoint, &stored_tag, sizeof(stored_tag)))
            return;
        if (!cp_tag_ok(checkpoint, stored_tag, tag, file, line))
            return;
        if ((size_t)stored_size != size) {
            LOG(0, "Error: v3 checkpoint size mismatch: expected %zu but got %u at %s:%d", size, stored_size,
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
        LOG(0, "Error: Failed to read size header from checkpoint (got %zu)", got);
        checkpoint->error = true;
        return;
    }

    uint32_t stored_tag = 0;
    got = fread(&stored_tag, 1, sizeof(stored_tag), checkpoint->file);
    if (got != sizeof(stored_tag)) {
        LOG(0, "Error: Failed to read block tag from checkpoint (got %zu)", got);
        checkpoint->error = true;
        return;
    }
    // Checked before the size, so a divergence is reported by NAME rather than
    // as the size mismatch that only sometimes follows it.
    if (!cp_tag_ok(checkpoint, stored_tag, tag, file, line))
        return;

    // Read filename length and filename (for diagnostics)
    char *saved_file = cp_read_block_fname(checkpoint);
    if (checkpoint->error)
        return;

    int32_t saved_line = 0;
    got = fread(&saved_line, 1, sizeof(saved_line), checkpoint->file);
    if (got != sizeof(saved_line)) {
        LOG(0, "Error: Failed to read saved line from checkpoint (got %zu)", got);
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    // Reject sizes that can't fit in size_t before comparing — otherwise a
    // 32-bit size_t may silently truncate a 5 GB stored_size and pretend
    // it matches a 1 GB `size` request.
    if (stored_size > (uint64_t)SIZE_MAX || (uint64_t)size != stored_size) {
        LOG(0, "Error: Checkpoint size mismatch: expected %zu at %s:%d but file contains %llu at %s:%d", size,
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
        LOG(0, "Error: Failed to read compression flag from checkpoint");
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    if (flag == 0x00) {
        // Raw data: read directly
        size_t nread = fread(data, 1, size, checkpoint->file);
        if (nread != size) {
            LOG(0, "Error: Failed to read %zu bytes from checkpoint (got %zu)", size, nread);
            checkpoint->error = true;
        }
    } else if (flag == 0x01) {
        // RLE-compressed: read compressed size then compressed data, decode
        uint64_t comp_size = 0;
        got = fread(&comp_size, 1, sizeof(comp_size), checkpoint->file);
        if (got != sizeof(comp_size)) {
            LOG(0, "Error: Failed to read RLE compressed size from checkpoint");
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        if (comp_size > CHECKPOINT_MAX_ALLOC) {
            LOG(0, "Error: v2 RLE compressed size %llu exceeds CHECKPOINT_MAX_ALLOC (%zu)",
                (unsigned long long)comp_size, (size_t)CHECKPOINT_MAX_ALLOC);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        uint8_t *comp_buf = (uint8_t *)malloc((size_t)comp_size);
        if (!comp_buf) {
            LOG(0, "Error: Out of memory for RLE decompression (%llu bytes)", (unsigned long long)comp_size);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        got = fread(comp_buf, 1, (size_t)comp_size, checkpoint->file);
        if (got != (size_t)comp_size) {
            LOG(0, "Error: Failed to read %llu RLE bytes from checkpoint (got %zu)", (unsigned long long)comp_size,
                got);
            free(comp_buf);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        // Decode RLE into output buffer
        if (!rle_decode(comp_buf, (size_t)comp_size, (uint8_t *)data, size)) {
            LOG(0, "Error: RLE decompression failed for %zu-byte block at %s:%d", size, file ? file : "(unknown)",
                line);
            free(comp_buf);
            if (saved_file)
                free(saved_file);
            checkpoint->error = true;
            return;
        }
        free(comp_buf);
    } else {
        LOG(0, "Error: Unknown compression flag 0x%02x in checkpoint", flag);
        if (saved_file)
            free(saved_file);
        checkpoint->error = true;
        return;
    }

    if (saved_file)
        free(saved_file);
}

// A failed read leaves `data` zeroed, never untouched.  Callers read into a
// local, test a magic or a field, and apply it: on the early returns above
// the local used to keep whatever the stack held, so a failed or mismatched
// block could be applied as garbage -- the AppleTalk restore, whose state is
// process-wide and so survives into the machine that keeps running when the
// load fails, did exactly that.  One zero-fill here covers every caller.
__attribute__((weak)) void checkpoint_busy_heartbeat(void) {}

void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *tag,
                                     const char *file, int line) {
    checkpoint_busy_heartbeat();
    read_checkpoint_block(checkpoint, data, size, tag, file, line);
    if ((!checkpoint || checkpoint->error) && data && size)
        memset(data, 0, size);
}

// Write a data block with size header, source metadata, and optional RLE compression
void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    checkpoint_busy_heartbeat();
    if (!checkpoint || checkpoint->error || !checkpoint->is_writing) {
        LOG(0, "Error: Invalid checkpoint handle for writing");
        if (checkpoint)
            checkpoint->error = true;
        return;
    }

    // v3 buffered write: append size + tag + raw data to accumulation buffer
    if (checkpoint->buf) {
        uint32_t sz = (uint32_t)size;
        uint32_t tg = tag ? cp_tag_hash(tag) : 0u;
        buf_append(checkpoint, &sz, sizeof(sz));
        buf_append(checkpoint, &tg, sizeof(tg));
        buf_append(checkpoint, data, size);
        return;
    }

    // v2 per-block write path with file/line metadata and per-block RLE

    // Write 64-bit uncompressed size header for validation on read
    uint64_t store_size = (uint64_t)size;
    size_t w = fwrite(&store_size, 1, sizeof(store_size), checkpoint->file);
    if (w != sizeof(store_size)) {
        LOG(0, "Error: Failed to write size header to checkpoint (wrote %zu)", w);
        checkpoint->error = true;
        return;
    }

    // Block tag: 0 when the caller did not name this block.  Always written,
    // so the layout never depends on whether a block happens to be named.
    uint32_t store_tag = tag ? cp_tag_hash(tag) : 0u;
    w = fwrite(&store_tag, 1, sizeof(store_tag), checkpoint->file);
    if (w != sizeof(store_tag)) {
        LOG(0, "Error: Failed to write block tag to checkpoint (wrote %zu)", w);
        checkpoint->error = true;
        return;
    }

    // Write filename length and filename bytes, then the line number
    uint32_t fname_len = file ? (uint32_t)strlen(file) : 0;
    w = fwrite(&fname_len, 1, sizeof(fname_len), checkpoint->file);
    if (w != sizeof(fname_len)) {
        LOG(0, "Error: Failed to write filename length to checkpoint (wrote %zu)", w);
        checkpoint->error = true;
        return;
    }
    if (fname_len > 0) {
        w = fwrite(file, 1, fname_len, checkpoint->file);
        if (w != fname_len) {
            LOG(0, "Error: Failed to write filename to checkpoint (wrote %zu)", w);
            checkpoint->error = true;
            return;
        }
    }

    int32_t iline = (int32_t)line;
    w = fwrite(&iline, 1, sizeof(iline), checkpoint->file);
    if (w != sizeof(iline)) {
        LOG(0, "Error: Failed to write line number to checkpoint (wrote %zu)", w);
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
            LOG(0, "Error: Failed to write %zu bytes to checkpoint (wrote %zu)", size, written);
            checkpoint->error = true;
        }
    } else {
        // Large block: flag=1 then RLE-compressed data
        // First pass: compute exact compressed size (dry run with NULL output)
        size_t comp_size = rle_encode((const uint8_t *)data, size, NULL, 0);
        uint8_t *comp_buf = (uint8_t *)malloc(comp_size);
        if (!comp_buf) {
            LOG(0, "Error: Out of memory for RLE compression (%zu bytes)", comp_size);
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
            LOG(0, "Error: Failed to write %zu RLE bytes to checkpoint (wrote %zu)", comp_size, w);
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
    checkpoint_t *cp = (checkpoint_t *)malloc(sizeof(struct checkpoint) + CHECKPOINT_STDIO_BUFFER_BYTES);
    if (!cp)
        return NULL;

    cp->file = fopen(filename, "rb");
    if (!cp->file) {
        free(cp);
        return NULL;
    }
    setvbuf(cp->file, cp->stdio_buf, _IOFBF, CHECKPOINT_STDIO_BUFFER_BYTES);

    // Initialize buffer fields
    cp->buf = NULL;
    cp->buf_cap = 0;
    cp->buf_used = 0;
    cp->buf_pos = 0;
    cp->buf_owned = false;
    cp->final_path = NULL;
    cp->tmp_path = NULL;

    // Read magic signature to detect format version
    char magic[CHECKPOINT_MAGIC_LEN];
    size_t got = fread(magic, 1, CHECKPOINT_MAGIC_LEN, cp->file);
    if (got != CHECKPOINT_MAGIC_LEN) {
        LOG(0, "Error: %s is not a valid checkpoint (too short)", filename);
        fclose(cp->file);
        free(cp);
        return NULL;
    }

    char version = checkpoint_signature_version(magic);
    if (version == CHECKPOINT_VERSION_V3) {
        // v3 quick format: read build ID, then sizes, decompress entire payload into buffer
        char file_build_id[BUILD_ID_LEN + 1];
        got = fread(file_build_id, 1, BUILD_ID_LEN, cp->file);
        if (got != BUILD_ID_LEN) {
            LOG(0, "Error: Failed to read build ID from %s", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        file_build_id[BUILD_ID_LEN] = '\0';
        // Validate build ID matches the running application
        if (memcmp(file_build_id, build_id_get(), BUILD_ID_LEN) != 0) {
            LOG(0, "Error: Checkpoint build ID mismatch in %s", filename);
            LOG(0, "  checkpoint: %s", file_build_id);
            LOG(0, "  current:    %s", build_id_get());
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        uint64_t uncompressed_size = 0, compressed_size = 0;
        got = fread(&uncompressed_size, 1, sizeof(uncompressed_size), cp->file);
        if (got != sizeof(uncompressed_size)) {
            LOG(0, "Error: Failed to read v3 uncompressed size from %s", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        got = fread(&compressed_size, 1, sizeof(compressed_size), cp->file);
        if (got != sizeof(compressed_size)) {
            LOG(0, "Error: Failed to read v3 compressed size from %s", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        if (compressed_size > CHECKPOINT_MAX_ALLOC || uncompressed_size > CHECKPOINT_MAX_ALLOC) {
            LOG(0, "Error: v3 size header (%llu / %llu) exceeds CHECKPOINT_MAX_ALLOC (%zu)",
                (unsigned long long)uncompressed_size, (unsigned long long)compressed_size,
                (size_t)CHECKPOINT_MAX_ALLOC);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // Allocate and read compressed data
        uint8_t *comp_buf = (uint8_t *)malloc((size_t)compressed_size);
        if (!comp_buf) {
            LOG(0, "Error: Out of memory for v3 decompression (%llu bytes)", (unsigned long long)compressed_size);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        got = fread(comp_buf, 1, (size_t)compressed_size, cp->file);
        if (got != (size_t)compressed_size) {
            LOG(0, "Error: Failed to read v3 compressed data from %s", filename);
            free(comp_buf);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        // Allocate decompressed buffer and decode
        cp->buf = (uint8_t *)malloc((size_t)uncompressed_size);
        if (!cp->buf) {
            LOG(0, "Error: Out of memory for v3 decompressed data (%llu bytes)", (unsigned long long)uncompressed_size);
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
                LOG(0, "Error: v3 RLE decompression failed for %s", filename);
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
    } else if (version == CHECKPOINT_VERSION_V2) {
        // v2 consolidated format: read build ID, model ID, then per-block streaming from file
        char file_build_id[BUILD_ID_LEN + 1];
        got = fread(file_build_id, 1, BUILD_ID_LEN, cp->file);
        if (got != BUILD_ID_LEN) {
            LOG(0, "Error: Failed to read build ID from %s", filename);
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        file_build_id[BUILD_ID_LEN] = '\0';
        // Validate build ID matches the running application
        if (memcmp(file_build_id, build_id_get(), BUILD_ID_LEN) != 0) {
            LOG(0, "Error: Checkpoint build ID mismatch in %s", filename);
            LOG(0, "  checkpoint: %s", file_build_id);
            LOG(0, "  current:    %s", build_id_get());
            fclose(cp->file);
            free(cp);
            return NULL;
        }
        cp->kind = CHECKPOINT_KIND_CONSOLIDATED;
    } else {
        LOG(0, "Error: %s is not a valid Granny Smith checkpoint (bad signature)", filename);
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
    // A quick save is written by the I/O worker at close (or inline by the
    // same code); only the consolidated kind streams to a FILE here, so only
    // it carries a stdio buffer.
    size_t stdio_bytes = kind != CHECKPOINT_KIND_QUICK ? CHECKPOINT_STDIO_BUFFER_BYTES : 0;
    checkpoint_t *cp = (checkpoint_t *)malloc(sizeof(struct checkpoint) + stdio_bytes);
    if (!cp)
        return NULL;

    cp->file = NULL;
    cp->final_path = NULL;
    cp->tmp_path = NULL;
    if (kind != CHECKPOINT_KIND_QUICK) {
        size_t n = strlen(filename);
        cp->final_path = malloc(n + 1);
        cp->tmp_path = malloc(n + 5);
        if (cp->final_path && cp->tmp_path) {
            memcpy(cp->final_path, filename, n + 1);
            memcpy(cp->tmp_path, filename, n);
            memcpy(cp->tmp_path + n, ".tmp", 5);
            cp->file = fopen(cp->tmp_path, "wb");
        }
        if (!cp->file) {
            free(cp->final_path);
            free(cp->tmp_path);
            free(cp);
            return NULL;
        }
        setvbuf(cp->file, cp->stdio_buf, _IOFBF, stdio_bytes);
    } else {
        snprintf(g_publish_tmp, sizeof g_publish_tmp, "%s", filename);
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
                LOG(0, "Error: Failed to allocate quick checkpoint buffer (%d bytes)", QUICK_BUF_CAPACITY);
                free(cp);
                return NULL;
            }
            g_quick_write_cap = QUICK_BUF_CAPACITY;
        }
        cp->buf = g_quick_write_buf;
        cp->buf_cap = g_quick_write_cap;
        cp->buf_used = QUICK_HDR_LEN; // the header is filled in at close
        cp->buf_owned = false; // static buffer, not freed on close
    } else {
        // v2 consolidated: write magic + build ID immediately, data streamed per-block
        char sig[CHECKPOINT_MAGIC_LEN];
        checkpoint_signature(sig, CHECKPOINT_VERSION_V2);
        if (fwrite(sig, 1, CHECKPOINT_MAGIC_LEN, cp->file) != CHECKPOINT_MAGIC_LEN ||
            fwrite(build_id_get(), 1, BUILD_ID_LEN, cp->file) != BUILD_ID_LEN) {
            LOG(0, "Error: Failed to write checkpoint header to %s", filename);
            fclose(cp->file);
            remove(cp->tmp_path);
            free(cp->final_path);
            free(cp->tmp_path);
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

// The system layer's report of a finished publish (system.c); a build
// without it (a unit suite) hears nothing.
__attribute__((weak)) void system_quick_checkpoint_written(bool ok, double ms, const char *error) {
    (void)ok;
    (void)ms;
    (void)error;
}

// The quick save's publish ended (emulator thread): the buffer is ours
// again; the system layer hears about it.
static void quick_written(bool ok, double ms, const char *error, void *ud) {
    (void)ud;
    g_quick_in_flight = false;
    system_quick_checkpoint_written(ok, ms, error);
}

void checkpoint_publish_next(const char *final_path) {
    snprintf(g_publish_final, sizeof g_publish_final, "%s", final_path ? final_path : "");
}

bool checkpoint_quick_in_flight(void) {
    return g_quick_in_flight;
}

void checkpoint_quick_wait(void) {
    while (g_quick_in_flight)
        io_worker_wait_idle();
}

uint32_t checkpoint_quick_skipped(void) {
    return g_quick_skipped;
}

void checkpoint_quick_note_skipped(void) {
    g_quick_skipped++;
}

// Close a checkpoint and free its resources
bool checkpoint_close(checkpoint_t *checkpoint) {
    if (!checkpoint)
        return false;

    // v3 quick write: write buffer directly.  v3 deliberately skips RLE —
    // the quick-save buffer is dominated by uncompressible RAM state, so the
    // pass was never paying for itself.  Set compressed_size == uncompressed_size
    // on disk to mark the payload as raw.  v2 still RLE-encodes per block.
    if (checkpoint->is_writing && checkpoint->buf && !checkpoint->error) {
        size_t raw_size = checkpoint->buf_used - QUICK_HDR_LEN;
        // The v3 header, at the front of the buffer: magic + build ID +
        // uncompressed_size + compressed_size ("compressed" = raw: v3 skips
        // RLE, the buffer being mostly uncompressible RAM).
        uint8_t *h = checkpoint->buf;
        checkpoint_signature((char *)h, CHECKPOINT_VERSION_V3);
        h += CHECKPOINT_MAGIC_LEN;
        memcpy(h, build_id_get(), BUILD_ID_LEN);
        h += BUILD_ID_LEN;
        uint64_t uc = (uint64_t)raw_size, cs = (uint64_t)raw_size;
        memcpy(h, &uc, sizeof uc);
        h += sizeof uc;
        memcpy(h, &cs, sizeof cs);
        // Publish: the whole buffer to the tmp path, renamed over the final
        // one -- on the I/O worker when there is one (the buffer is its
        // until the completion lands), else here.
        const char *final = g_publish_final[0] ? g_publish_final : g_publish_tmp;
        if (g_publish_final[0] &&
            io_submit_publish(checkpoint->buf, checkpoint->buf_used, g_publish_tmp, final, quick_written, NULL)) {
            g_quick_in_flight = true;
        } else {
            char err[160];
            double t0 = io_now_ms();
            int rc = io_write_publish(checkpoint->buf, checkpoint->buf_used, g_publish_tmp, final, err, sizeof err);
            if (rc != 0) {
                LOG(0, "Error: quick checkpoint write failed: %s", err);
                checkpoint->error = true;
            }
            quick_written(rc == 0, io_now_ms() - t0, rc == 0 ? NULL : err, NULL);
        }
        g_publish_final[0] = '\0';
    }

    // Free owned buffer (v3 read mode allocates its own buffer); hand a
    // borrowed quick buffer back, with whatever capacity it grew to.
    if (checkpoint->buf_owned && checkpoint->buf) {
        free(checkpoint->buf);
    } else if (checkpoint->is_writing && checkpoint->buf) {
        g_quick_write_buf = checkpoint->buf;
        g_quick_write_cap = checkpoint->buf_cap;
    }

    bool ok = !checkpoint->error;
    if (checkpoint->file) {
        bool closed = fclose(checkpoint->file) == 0;
        ok = ok && closed;
        if (checkpoint->tmp_path) {
            // Consolidated: publish the finished file, or drop the partial one.
            if (ok && rename(checkpoint->tmp_path, checkpoint->final_path) != 0) {
                LOG(0, "Error: cannot rename %s over %s", checkpoint->tmp_path, checkpoint->final_path);
                ok = false;
            }
            if (!ok)
                remove(checkpoint->tmp_path);
        }
    }
    free(checkpoint->final_path);
    free(checkpoint->tmp_path);
    free(checkpoint);
    return ok;
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

// Write a file to the checkpoint.  A quick (v3) checkpoint references a file
// under /opfs/ and embeds anything else; a consolidated (v2) one always embeds.
void checkpoint_write_file_loc(checkpoint_t *checkpoint, const char *path, const char *file, int line) {
    if (!checkpoint || checkpoint->error || !checkpoint->is_writing) {
        LOG(0, "Error: Invalid checkpoint handle for writing (file block)");
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
        // Paths under /opfs/ are persistent (OPFS-backed). Everything else
        // (including /tmp/) is volatile.
        bool persistent = (path && strncmp(path, "/opfs/", 6) == 0);
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

    // v2 per-block write: a consolidated checkpoint is self-contained, so the
    // file's contents are always embedded.
    uint64_t content_size = 0;
    uint32_t name_len = (path && path[0]) ? (uint32_t)strlen(path) : 0;
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
    uint64_t payload_size = 4 + name_len + 1 + 8 + content_size;

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
    // Content flag (always set in a consolidated checkpoint)
    uint8_t has_content = 1;
    w = fwrite(&has_content, 1, 1, checkpoint->file);
    if (w != 1) {
        checkpoint->error = true;
        return;
    }
    w = fwrite(&content_size, 1, sizeof(content_size), checkpoint->file);
    if (w != sizeof(content_size)) {
        checkpoint->error = true;
        return;
    }
    if (content_size > 0) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            // The source file went away between probe and write — there's
            // no honest payload to emit, and zero-padding the slot would
            // silently corrupt restore. Fail the checkpoint so
            // the user sees the error rather than a half-written file.
            LOG(0, "Error: cannot read '%s' for checkpoint write: source file missing", path);
            checkpoint->error = true;
            return;
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

// Read a file block from the checkpoint into a buffer
size_t checkpoint_read_file_loc(checkpoint_t *checkpoint, uint8_t *dest, size_t capacity, char **out_path,
                                const char *file, int line) {
    if (out_path)
        *out_path = NULL;
    if (checkpoint && checkpoint->error && !checkpoint->is_writing)
        return 0; // the stream has already failed and said why
    if (!checkpoint || checkpoint->is_writing) {
        LOG(0, "Error: Invalid checkpoint handle for reading (file block)");
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
                } else {
                    // The referenced file is gone (user wiped /opfs/images,
                    // or the source image was deleted post-checkpoint).
                    // Surface this rather than silently returning loaded=0,
                    // which a caller can't distinguish from "zero-byte file".
                    LOG(0, "Warning: checkpoint references missing file '%s'", *out_path);
                    checkpoint->error = true;
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
    // Same bounded read as the v2 path; the name is read for stream alignment
    // and discarded, but an unbounded length here drove a 4 GB malloc too.
    char *skipped = cp_read_block_fname(checkpoint);
    if (checkpoint->error)
        return 0;
    free(skipped);
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
        // This build's consolidated writer always embeds, and build-ID gating
        // refuses every file an older build wrote: a reference here is corrupt.
        LOG(0, "Error: consolidated checkpoint file block for '%s' carries no content",
            (out_path && *out_path) ? *out_path : "(unnamed)");
        checkpoint->error = true;
    }
    return loaded;
}

checkpoint_build_t checkpoint_check_build_id(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f)
        return CHECKPOINT_BUILD_UNREADABLE;

    // Signature, then the build ID (immediately follows it in both formats)
    char magic[CHECKPOINT_MAGIC_LEN];
    char file_build_id[BUILD_ID_LEN];
    bool whole = fread(magic, 1, CHECKPOINT_MAGIC_LEN, f) == CHECKPOINT_MAGIC_LEN &&
                 fread(file_build_id, 1, BUILD_ID_LEN, f) == BUILD_ID_LEN;
    fclose(f);
    if (!whole) {
        LOG(1, "%s: too short to be a checkpoint", filename);
        return CHECKPOINT_BUILD_UNREADABLE;
    }
    if (!checkpoint_signature_version(magic)) {
        LOG(1, "%s: not a checkpoint this build reads (bad signature)", filename);
        return CHECKPOINT_BUILD_UNREADABLE;
    }
    if (memcmp(file_build_id, build_id_get(), BUILD_ID_LEN) != 0) {
        LOG(1, "%s: saved by another build", filename);
        return CHECKPOINT_BUILD_MISMATCH;
    }
    return CHECKPOINT_BUILD_MATCH;
}

bool checkpoint_validate_build_id(const char *filename) {
    return checkpoint_check_build_id(filename) == CHECKPOINT_BUILD_MATCH;
}
