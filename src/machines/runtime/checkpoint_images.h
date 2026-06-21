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
#include "system_config.h"

// Save the image list (count prefix + per-image blob via image_checkpoint).
void mac_checkpoint_save_images(config_t *cfg, checkpoint_t *cp);

// Restore the image list and re-attach each image onto cfg->images.  Fails
// loudly via checkpoint_set_error on partial reads / failed opens.
void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_RUNTIME_CHECKPOINT_IMAGES_H
