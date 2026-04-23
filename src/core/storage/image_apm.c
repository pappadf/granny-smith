// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_apm.c
// Apple Partition Map parser.  Reads block 1 for the "PM" signature and
// map length, then walks entries 1..N at 512-byte offsets.  All multi-byte
// fields on disk are big-endian; we do the byte swaps here rather than
// rely on host-specific htonl macros.

// This file contains the pure parser (no image_t dependency).  The
// image-backed entry point image_apm_parse lives in image_apm_io.c so
// unit tests can link against image_apm.c without the storage stack.

#include "image_apm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// On-disk APM entry field offsets.  From the Apple Partition Map spec.
#define APM_OFF_SIG         0x00 // uint16 "PM" (0x504D)
#define APM_OFF_SIG_PAD     0x02 // uint16 reserved, should be 0
#define APM_OFF_MAP_BLKCNT  0x04 // uint32 blocks in partition map
#define APM_OFF_PY_START    0x08 // uint32 partition start in 512-byte blocks
#define APM_OFF_PART_BLKCNT 0x0C // uint32 partition size in 512-byte blocks
#define APM_OFF_NAME        0x10 // char[32]
#define APM_OFF_TYPE        0x30 // char[32]
#define APM_OFF_STATUS      0x58 // uint32

#define APM_SIG_PM 0x504D

// Upper bound on the partition map length we will accept.  Real Apple
// media never exceeds ~64 entries; capping at 256 protects against a
// corrupt pmMapBlkCnt that would otherwise drive a huge allocation.
#define APM_MAX_PARTITIONS 256

// Read a big-endian uint16 from a byte buffer.
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// Read a big-endian uint32 from a byte buffer.
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Copy a 32-byte APM name/type field into a 33-byte NUL-terminated buffer,
// stripping trailing NULs and spaces for display.
static void copy_apm_str(char *dst, const uint8_t *src) {
    memcpy(dst, src, 32);
    dst[32] = '\0';
    for (int i = 31; i >= 0 && (dst[i] == '\0' || dst[i] == ' '); i--)
        dst[i] = '\0';
}

bool image_apm_probe_magic(const uint8_t *block1) {
    if (!block1)
        return false;
    // Signature "PM" must be present.
    if (be16(block1 + APM_OFF_SIG) != APM_SIG_PM)
        return false;
    // pmMapBlkCnt must be plausible: non-zero and not absurdly large.
    uint32_t map_blocks = be32(block1 + APM_OFF_MAP_BLKCNT);
    if (map_blocks == 0 || map_blocks > APM_MAX_PARTITIONS)
        return false;
    return true;
}

enum apm_fs_kind image_apm_classify_type(const char *type_string) {
    if (!type_string)
        return APM_FS_UNKNOWN;
    // Apple's strings vary in case over time (old tools emit Apple_HFS,
    // new ones sometimes differ); compare case-insensitively.
    if (strcasecmp(type_string, "Apple_HFS") == 0 || strcasecmp(type_string, "Apple_HFSX") == 0)
        return APM_FS_HFS;
    if (strcasecmp(type_string, "Apple_UNIX_SVR2") == 0)
        return APM_FS_UFS;
    if (strcasecmp(type_string, "Apple_partition_map") == 0)
        return APM_FS_PARTITION_MAP;
    // Apple_Driver, Apple_Driver43, Apple_Driver_ATA, Apple_Driver_IOKit...
    if (strncasecmp(type_string, "Apple_Driver", 12) == 0)
        return APM_FS_DRIVER;
    if (strcasecmp(type_string, "Apple_Free") == 0)
        return APM_FS_FREE;
    if (strcasecmp(type_string, "Apple_Patches") == 0)
        return APM_FS_PATCHES;
    return APM_FS_UNKNOWN;
}

// Static error strings (also referenced by image_apm_io.c via extern).
const char *const image_apm_err_nil = "image is NULL";
const char *const image_apm_err_read = "short read on APM block";
const char *const image_apm_err_sig = "APM signature missing on block 1";
const char *const image_apm_err_count = "pmMapBlkCnt out of range (1..256)";
const char *const image_apm_err_alloc = "out of memory";

apm_table_t *image_apm_parse_buffer(const uint8_t *buf, size_t buf_size, const char **errmsg) {
    if (!buf || buf_size < 2 * APM_BLOCK_SIZE) {
        if (errmsg)
            *errmsg = image_apm_err_read;
        return NULL;
    }

    const uint8_t *entry1 = buf + APM_BLOCK_SIZE;
    if (!image_apm_probe_magic(entry1)) {
        if (errmsg)
            *errmsg = image_apm_err_sig;
        return NULL;
    }

    uint32_t n = be32(entry1 + APM_OFF_MAP_BLKCNT);
    if (n == 0 || n > APM_MAX_PARTITIONS) {
        if (errmsg)
            *errmsg = image_apm_err_count;
        return NULL;
    }

    apm_table_t *table = calloc(1, sizeof(*table));
    if (!table) {
        if (errmsg)
            *errmsg = image_apm_err_alloc;
        return NULL;
    }
    table->map_block_count = n;
    table->partitions = calloc(n, sizeof(apm_partition_t));
    if (!table->partitions) {
        free(table);
        if (errmsg)
            *errmsg = image_apm_err_alloc;
        return NULL;
    }

    // Parse each entry.  On per-entry sig mismatch or buffer exhaustion
    // we stop and return the partial table — real Apple media is always
    // well-formed but surfacing what we have beats rejecting the whole
    // map on a single bad trailing entry.
    for (uint32_t i = 0; i < n; i++) {
        size_t offset = (size_t)(i + 1) * APM_BLOCK_SIZE;
        if (offset + APM_BLOCK_SIZE > buf_size)
            break;
        const uint8_t *block = buf + offset;
        if (be16(block + APM_OFF_SIG) != APM_SIG_PM)
            break;
        apm_partition_t *p = &table->partitions[table->n_partitions++];
        p->index = i + 1;
        p->start_block = be32(block + APM_OFF_PY_START);
        p->size_blocks = be32(block + APM_OFF_PART_BLKCNT);
        p->status = be32(block + APM_OFF_STATUS);
        copy_apm_str(p->name, block + APM_OFF_NAME);
        copy_apm_str(p->type, block + APM_OFF_TYPE);
        p->fs_kind = image_apm_classify_type(p->type);
    }

    if (table->n_partitions == 0) {
        free(table->partitions);
        free(table);
        if (errmsg)
            *errmsg = image_apm_err_sig;
        return NULL;
    }

    return table;
}

void image_apm_free(apm_table_t *table) {
    if (!table)
        return;
    free(table->partitions);
    free(table);
}
