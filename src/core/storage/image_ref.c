// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_ref.c
// The checkpoint form of "which image was in this drive": the image's
// position in the checkpoint's image list, written by the media controllers
// (floppy, SCSI, ATA, the Lisa FDC and ProFile) and resolved against the
// machine's restored image list.  A position, not a name: a blank image has
// no name, and two drives can hold images of the same file.  The images part
// is saved before any device that refers to it (it is restored first), and
// it stamps each image with its position as it writes it
// (image_set_checkpoint_index).  A file of its own so a unit suite that
// fakes the rest of the image layer can link the real pair.

#include "checkpoint.h"
#include "image.h"
#include "image_internal.h"
#include "log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("image")

void checkpoint_write_image_ref(checkpoint_t *cp, const image_t *img) {
    uint32_t index = img ? img->checkpoint_index : IMAGE_REF_NONE;
    system_write_checkpoint_data(cp, &index, sizeof index);
}

image_t *checkpoint_read_image_ref(checkpoint_t *cp, const image_list_t *images, const char *what, bool *held) {
    uint32_t index = IMAGE_REF_NONE;
    system_read_checkpoint_data(cp, &index, sizeof index);
    if (held)
        *held = index != IMAGE_REF_NONE;
    if (index == IMAGE_REF_NONE || checkpoint_has_error(cp))
        return NULL;
    image_t *img = (images && index < (uint32_t)images->n) ? images->items[index] : NULL;
    if (!img) {
        // The drive held a medium: a restore without it is not the machine
        // that was saved.
        LOG(0, "%s: the checkpoint's image %u is not among its images", what ? what : "drive", (unsigned)index);
        checkpoint_set_error(cp);
    }
    return img;
}
