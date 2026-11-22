// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_images.h
// Family-agnostic save/restore of the machine's tracked images into a checkpoint stream.
// Previously glue030_checkpoint_{save,restore}_images — but the logic is in
// no way GLUE-specific (it serialises the generic image list), so it lives in
// runtime/ for use by any machine substrate.

#ifndef GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H
#define GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H

#include "checkpoint.h"
#include "image.h"
#include "system_internal.h"

// Save the image list (count prefix + per-image blob via image_checkpoint).
void mac_checkpoint_save_images(config_t *cfg, checkpoint_t *cp);

// Restore the image list and re-attach each image to the tracked list.  Fails
// loudly via checkpoint_set_error on partial reads / failed opens.
void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp);

// Restore ONE image (the metadata + storage blob written by image_checkpoint)
// and reopen it as the kind it was saved as (image_open_kind).  Consumes its
// stream bytes even on failure (returns NULL + marks the checkpoint errored) so
// the caller's stream stays aligned.  The caller owns the returned image — it
// is NOT added to the tracked list here, so a device (the Lisa ProFile) can
// attach it directly.
image_t *mac_checkpoint_restore_one_image(checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H
