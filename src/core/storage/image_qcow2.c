// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_qcow2.c
// QCOW2 header parser and cluster decoder -- see image_qcow2.h.

#include "image_qcow2.h"
#include "common.h"

#include "peeler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define QCOW2_V2_HEADER 72u // version 2's whole header; version 3's prefix
#define QCOW2_V3_HEADER 104u // version 3's minimum header_length

// Header field offsets (big-endian).
#define QH_VERSION          4
#define QH_BACKING_OFFSET   8
#define QH_CLUSTER_BITS     20
#define QH_SIZE             24
#define QH_CRYPT_METHOD     32
#define QH_L1_SIZE          36
#define QH_L1_OFFSET        40
#define QH_INCOMPAT         72
#define QH_HEADER_LENGTH    100
#define QH_COMPRESSION_TYPE 104

bool qcow2_detect(const uint8_t *buf, size_t len) {
    if (len < QCOW2_V2_HEADER || RD_BE32(buf) != QCOW2_MAGIC)
        return false;
    uint32_t v = RD_BE32(buf + QH_VERSION);
    return v == 2 || v == 3;
}

int qcow2_parse_header(const uint8_t *buf, size_t len, qcow2_header_t *h, const char **why) {
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = NULL;
    memset(h, 0, sizeof(*h));
    if (!qcow2_detect(buf, len)) {
        *why = "not a QCOW2 header";
        return -EINVAL;
    }
    h->version = RD_BE32(buf + QH_VERSION);
    h->backing_offset = RD_BE64(buf + QH_BACKING_OFFSET);
    h->cluster_bits = RD_BE32(buf + QH_CLUSTER_BITS);
    h->size = RD_BE64(buf + QH_SIZE);
    h->crypt_method = RD_BE32(buf + QH_CRYPT_METHOD);
    h->l1_size = RD_BE32(buf + QH_L1_SIZE);
    h->l1_offset = RD_BE64(buf + QH_L1_OFFSET);
    h->header_length = QCOW2_V2_HEADER;
    if (h->version >= 3) {
        if (len < QCOW2_V3_HEADER) {
            *why = "truncated header";
            return -EINVAL;
        }
        h->incompat = RD_BE64(buf + QH_INCOMPAT);
        h->header_length = RD_BE32(buf + QH_HEADER_LENGTH);
        if (h->header_length < QCOW2_V3_HEADER || (h->header_length % 8) != 0) {
            *why = "bad header length";
            return -EINVAL;
        }
        // An additional field is present only when the header reaches it.
        if (h->header_length > QH_COMPRESSION_TYPE) {
            if (len <= QH_COMPRESSION_TYPE) {
                *why = "truncated header";
                return -EINVAL;
            }
            h->compression_type = buf[QH_COMPRESSION_TYPE];
        }
    }
    if (h->cluster_bits < QCOW2_MIN_CLUSTER_BITS || h->cluster_bits > QCOW2_MAX_CLUSTER_BITS) {
        *why = "cluster size out of range";
        return -EINVAL;
    }
    if (h->size > QCOW2_MAX_SIZE) {
        *why = "virtual size out of range";
        return -EINVAL;
    }
    if ((h->incompat & QCOW2_INCOMPAT_EXTL2) && h->cluster_bits < 14) {
        *why = "extended L2 entries with clusters under 16 KB";
        return -EINVAL;
    }
    if (h->l1_offset & (((uint64_t)1 << h->cluster_bits) - 1)) {
        *why = "L1 table not cluster-aligned";
        return -EINVAL;
    }
    if ((uint64_t)h->l1_size < qcow2_l1_needed(h)) {
        *why = "L1 table too small for the virtual size";
        return -EINVAL;
    }
    if (((h->incompat & QCOW2_INCOMPAT_COMPRESSION) != 0) != (h->compression_type != 0)) {
        *why = "compression type and its feature bit disagree";
        return -EINVAL;
    }
    // Features that change what the guest sees, which this reader lacks.
    if (h->backing_offset) {
        *why = "it is an overlay on a backing file (merge it first: qemu-img convert)";
        return -ENOTSUP;
    }
    if (h->crypt_method) {
        *why = "it is encrypted";
        return -ENOTSUP;
    }
    if (h->incompat & QCOW2_INCOMPAT_DATA_FILE) {
        *why = "its data is in an external file";
        return -ENOTSUP;
    }
    if (h->incompat & ~QCOW2_INCOMPAT_KNOWN) {
        *why = "it uses an incompatible feature this reader does not know";
        return -ENOTSUP;
    }
    if (h->compression_type > QCOW2_COMPRESSION_ZSTD) {
        *why = "unknown compression type";
        return -ENOTSUP;
    }
    return 0;
}

void qcow2_compressed_extent(uint64_t entry, uint32_t cluster_bits, uint64_t *off, uint64_t *len) {
    // x = 62 - (cluster_bits - 8): the offset below, the extra sectors above.
    uint32_t x = 62u - (cluster_bits - 8u);
    uint64_t o = entry & (((uint64_t)1 << x) - 1);
    uint64_t extra = (entry >> x) & (((uint64_t)1 << (62u - x)) - 1);
    *off = o;
    *len = (extra + 1) * 512u - (o & 511u);
}

int qcow2_decode_cluster(const uint8_t *src, size_t src_len, uint32_t cluster_bits, uint8_t *dst, size_t dst_len) {
    size_t cluster = (size_t)1 << cluster_bits;
    if (dst_len > cluster)
        return -EINVAL;
    // A final cluster cut short by the virtual size still decodes whole.
    uint8_t *out = dst_len == cluster ? dst : malloc(cluster);
    if (!out)
        return -ENOMEM;
    int64_t n = peel_inflate(src, src_len, out, cluster);
    int rc = n == (int64_t)cluster ? 0 : -EINVAL;
    if (out != dst) {
        if (rc == 0)
            memcpy(dst, out, dst_len);
        free(out);
    }
    return rc;
}
