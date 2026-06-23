// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_images.h
// Family-agnostic save/restore of cfg->images[] into a checkpoint stream.
// Previously glue030_checkpoint_{save,restore}_images — but the logic is in
// no way GLUE-specific (it serialises the generic image list), so it lives in
// runtime/ for use by any machine substrate (proposal §4.2.1 / §4.3).

#ifndef GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H
#define GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H

#include "checkpoint.h"
#include "image.h"
#include "system_config.h"

// Save the image list (count prefix + per-image blob via image_checkpoint).
void mac_checkpoint_save_images(config_t *cfg, checkpoint_t *cp);

// Restore the image list and re-attach each image onto cfg->images.  Fails
// loudly via checkpoint_set_error on partial reads / failed opens.
void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp);

// Restore ONE image (the metadata + storage blob written by image_checkpoint)
// and reopen it with `geom` (0 block_size ⇒ 512).  Consumes its stream bytes
// even on failure (returns NULL + marks the checkpoint errored) so the caller's
// stream stays aligned.  The caller owns the returned image — it is NOT added to
// cfg->images here, so a device (e.g. the Lisa ProFile, block_size 532) can
// reopen its own image with the right geometry and attach it directly.
image_t *mac_checkpoint_restore_one_image(checkpoint_t *cp, image_geometry_t geom);

#endif // GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H
