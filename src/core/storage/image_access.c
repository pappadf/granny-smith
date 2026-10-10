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
enum image_type image_get_type(const image_t *image) {
    return image ? image->type : image_other;
}
void image_set_type(image_t *image, enum image_type type) {
    if (image)
        image->type = type;
}
bool image_is_writable(const image_t *image) {
    return image && image->writable;
}
size_t image_get_raw_size(const image_t *image) {
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
void image_set_tags(image_t *image, uint8_t *tags, uint32_t tag_bytes, uint32_t tag_count) {
    if (!image) {
        free(tags); // nobody to own them
        return;
    }
    free(image->tags); // drop the tags the image had
    image->tags = tags;
    image->tag_bytes = tags ? tag_bytes : 0;
    image->tag_count = tags ? tag_count : 0;
}
