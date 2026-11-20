// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ref.c
// The checkpoint form of "which image was in this drive": the image's name,
// written by the media controllers (floppy, SCSI, ATA, the Lisa FDC) and
// resolved against the machine's restored image list.  A file of its own so
// a unit suite that fakes the rest of the image layer (images_find,
// image_get_filename) can link the real pair.

#include "checkpoint.h"
#include "image.h"
#include "log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("image")

void checkpoint_write_image_ref(checkpoint_t *cp, const image_t *img) {
    // A nameless image cannot be found again, so it writes as "none"
    checkpoint_write_string(cp, img ? image_get_filename(img) : NULL);
}

image_t *checkpoint_read_image_ref(checkpoint_t *cp, const image_list_t *images, const char *what, bool *named) {
    if (named)
        *named = false;
    // Bounded by CHECKPOINT_MAX_PATH: the length is on-disk data
    char *name = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image-name bytes");
    if (!name)
        return NULL;
    if (named)
        *named = true;
    image_t *img = images_find(images, name);
    if (!img) {
        // The drive held a medium: a restore without it is not the machine
        // that was saved.
        LOG(0, "%s: the checkpoint's image '%s' is not among its images", what ? what : "drive", name);
        checkpoint_set_error(cp);
    }
    free(name);
    return img;
}
