// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_access.c
// The image_t field accessors declared in image.h.  A file of their own, with
// no dependency beyond the struct layout (image_internal.h), so a unit suite
// that fakes the rest of the image layer can link the real accessors.

#include "image_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// The accessors below are NULL-safe: a NULL image reads as empty.
image_kind_t image_get_kind(const image_t *image) {
    return image ? image->kind : IMAGE_KIND_NONE;
}
image_layout_t image_layout(const image_t *image) {
    return image ? image_layout_of_size(image->raw_size) : IMAGE_LAYOUT_NONE;
}
void image_set_checkpoint_index(image_t *image, uint32_t index) {
    if (image)
        image->checkpoint_index = index;
}
bool image_is_writable(const image_t *image) {
    return image && image->writable;
}
uint64_t image_get_raw_size(const image_t *image) {
    return image ? image->raw_size : 0;
}
const char *image_get_format(const image_t *image) {
    return image ? image->format : NULL;
}
const char *image_get_source_key(const image_t *image) {
    return image ? image->source_key : NULL;
}
const char *image_get_delta_path(const image_t *image) {
    return image ? image->delta_path : NULL;
}
const char *image_get_journal_path(const image_t *image) {
    return image ? image->journal_path : NULL;
}
storage_t *image_get_storage(const image_t *image) {
    return image ? image->storage : NULL;
}
uint64_t image_get_reads(const image_t *image) {
    return image ? image->reads : 0;
}
uint64_t image_get_writes(const image_t *image) {
    return image ? image->writes : 0;
}

uint64_t image_get_read_errors(const image_t *image) {
    return image ? image->read_errors : 0;
}

uint32_t image_get_tag_bytes(const image_t *image) {
    return image ? image->tag_bytes : 0;
}
