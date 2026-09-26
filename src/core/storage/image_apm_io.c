// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_apm_io.c
// Image-backed entry point for the APM parser.  Kept separate from
// image_apm.c so unit tests can link the pure parser without needing
// image.c / storage.c.

#include "image.h"
#include "image_apm.h"

#include <stdlib.h>

// Error messages defined in image_apm.c.
extern const char *const image_apm_err_read;
extern const char *const image_apm_err_alloc;
extern const char *const image_apm_err_nil;

apm_table_t *image_apm_parse(image_t *img, const char **errmsg) {
    if (!img) {
        if (errmsg)
            *errmsg = image_apm_err_nil;
        return NULL;
    }

    // A partition map is in 512-byte blocks; an image opened with another
    // geometry (a Lisa ProFile's 532) has none, and disk_read_data would
    // assert on the 512-based read below.
    if (disk_block_size(img) != APM_BLOCK_SIZE) {
        if (errmsg)
            *errmsg = image_apm_err_read;
        return NULL;
    }

    // Read enough blocks to cover any plausible APM.  257 blocks = ~128KB
    // accommodates the largest partition map we will accept (256 entries
    // + block 0).  Cap to the image size so small fixtures work, in whole
    // blocks: disk_read_data reads nothing else.
    size_t scan_bytes = (size_t)(256 + 1) * APM_BLOCK_SIZE;
    size_t img_size = disk_size(img);
    if (scan_bytes > img_size)
        scan_bytes = img_size - img_size % APM_BLOCK_SIZE;
    if (scan_bytes < 2 * APM_BLOCK_SIZE) {
        if (errmsg)
            *errmsg = image_apm_err_read;
        return NULL;
    }

    uint8_t *buf = malloc(scan_bytes);
    if (!buf) {
        if (errmsg)
            *errmsg = image_apm_err_alloc;
        return NULL;
    }
    if (disk_read_data(img, 0, buf, scan_bytes) != scan_bytes) {
        free(buf);
        if (errmsg)
            *errmsg = image_apm_err_read;
        return NULL;
    }

    apm_table_t *table = image_apm_parse_buffer(buf, scan_bytes, errmsg);
    free(buf);
    return table;
}
