// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_images.c
// Image-list checkpoint serialisation — see checkpoint_images.h.

#include "checkpoint_images.h"
#include "out.h"

#include "checkpoint.h"
#include "checkpoint_machine.h"
#include "image.h"
#include "image_kind.h"
#include "image_wrap.h"
#include "source.h"
#include "status.h"
#include "storage.h"
#include "storage_util.h"
#include "system.h" // config_max_images -- the real bound on the restored list

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Save the image list into a checkpoint stream.  Layout:
//   uint32_t count
//   for each image i:
//     uint32_t name_len, name_bytes...
//     uint8_t  flags (IMAGE_CKPT_WRITABLE | IMAGE_CKPT_WRAPPED)
//     uint8_t  kind (image_kind_t: what the drive presents it as)
//     uint64_t raw_size (the storage's, without any wrapper prefix)
//     uint32_t instance_len, instance_bytes...
//     uint32_t key_len, key_bytes... (the base's source key, source.h)
//     <storage-specific blob via image_checkpoint: its blocks, tags in-band>
// A NULL slot is skipped -- and left out of the count, so the count always
// says how many entries follow (image_checkpoint writes nothing for NULL).
void mac_checkpoint_save_images(config_t *cfg, checkpoint_t *cp) {
    uint32_t count = 0;
    int n = config_get_n_images(cfg);
    for (int i = 0; i < n; ++i)
        count += config_get_image(cfg, i) != NULL;
    system_write_checkpoint_data(cp, &count, sizeof(count));
    // Each image's position in this list is what a drive's reference names
    // (checkpoint_write_image_ref); the restore rebuilds the list in this
    // order, so the positions resolve there.
    uint32_t index = 0;
    for (int i = 0; i < n; ++i) {
        image_t *img = config_get_image(cfg, i);
        if (img) {
            image_set_checkpoint_index(img, index++);
            image_checkpoint(img, cp);
        }
    }
}

// Whether the base image at `path` is there to open: through the same path
// opener the image itself is opened with, so a disk inside an archive
// (/opfs/upload/disks.zip/System.dsk) is found.  access() asks the host,
// which has no such file, and the restore then tried to create one inside
// the archive and printed a false "cannot recreate the missing base" over a
// restore that went on to succeed (S1018-32).
static bool base_present(const char *path) {
    int err = 0;
    source_t *s = source_open_path(path, GS_FORK_DATA, &err);
    if (!s)
        return false;
    source_release(s);
    return true;
}

// Restore the image list from a checkpoint stream and attach each image
// back onto the machine's tracked images.  Mirrors save layout; fails loudly via
// checkpoint_set_error on partial reads / failed image opens so the
// caller's restore loop sees a marked-error checkpoint.
image_t *mac_checkpoint_restore_one_image(checkpoint_t *cp) {
    // Bounded and terminated by the reader rather than by the writer's
    // promise: `name` goes on to base_present(), image_open_kind() and
    // out_printf("%s"), so a file that omits the NUL used to read off the end of
    // the allocation, and an unbounded length drove the malloc.
    char *name = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image path");
    uint8_t flags = 0;
    system_read_checkpoint_data(cp, &flags, sizeof(flags));
    bool writable = (flags & IMAGE_CKPT_WRITABLE) != 0;
    uint8_t kind = 0;
    system_read_checkpoint_data(cp, &kind, sizeof(kind));
    if (kind >= IMAGE_KIND_COUNT) {
        out_printf("Error: image kind %u in the checkpoint is not one this build knows\n", (unsigned)kind);
        checkpoint_set_error(cp);
        kind = IMAGE_KIND_NONE;
    }
    uint64_t raw_size = 0;
    system_read_checkpoint_data(cp, &raw_size, sizeof(raw_size));
    char *instance_path = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image instance path");
    char *saved_key = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image source key");

    image_t *img = NULL;
    if (name) {
        // Consolidated checkpoints carry the raw image bytes inline; quick
        // checkpoints reference the on-disk file and just need it reopened.  The
        // base is reopened as the kind it was saved as, so a ProFile restores
        // at its 532-byte block and a CD read-only.
        bool consolidated = checkpoint_get_kind(cp) == CHECKPOINT_KIND_CONSOLIDATED;
        if (raw_size > 0 && consolidated && !base_present(name)) {
            // A consolidated checkpoint carries every block inline and the
            // restore below writes them all into a fresh delta, so the base
            // file's CONTENT is never read through — it only has to exist so
            // the opener can probe the geometry.  Materialise it only when
            // nothing is there: this used to run unconditionally, and
            // because it opens with "wb" it truncated the image at its
            // ORIGINAL path (the "checkpoint.load destroyed my disk image"
            // footgun — a suite that checkpointed with shared test media
            // attached silently zeroed that media on load, while the guest
            // kept working off the restored delta).  An existing file is now
            // left completely alone, whatever its size: a DiskCopy base is
            // legitimately 84 bytes larger than its logical raw_size, and a
            // genuinely mismatched geometry is still caught loudly by
            // storage_restore_from_checkpoint's own check.
            // A .dmg is recreated as UDIF (a few KB of zero run), not as a
            // raw file of the full size under that name.
            // raw_size is the disk's data bytes; a ProFile's file holds its
            // tags in-band too (image_kind_file_bytes).
            uint64_t file_bytes = image_kind_file_bytes((image_kind_t)kind, raw_size);
            int rc = path_has_ext(name, ".dmg") ? image_create_empty_udif(name, file_bytes)
                                                : image_create_empty(name, file_bytes);
            // The open below then fails and flags the checkpoint; this says why.
            if (rc != 0)
                out_printf("Error: cannot recreate the missing base %s while restoring checkpoint\n", name);
        }
        image_open_opts_t o = {.kind = (image_kind_t)kind, .writable = writable};
        if (writable && !consolidated && instance_path && instance_path[0])
            o.instance = instance_path; // the same delta files
        else if (writable)
            o.delta_dir = checkpoint_machine_dir(); // a fresh instance
        img = image_open_kind(name, &o, NULL, 0);
        if (!img) {
            out_printf("Error: image_open failed for %s while restoring checkpoint\n", name);
            checkpoint_set_error(cp);
        } else if (!consolidated && saved_key && saved_key[0] && image_get_source_key(img) &&
                   !source_key_same_source(saved_key, image_get_source_key(img))) {
            // A quick checkpoint's disk is the base plus the saved delta: on
            // other base bytes the delta would apply to the wrong disk.  (A
            // consolidated one carries every block, so its base is not read.)
            out_printf("Error: %s is not the image the checkpoint was saved with (it was %s, it is now %s)\n", name,
                       saved_key, image_get_source_key(img));
            checkpoint_set_error(cp);
        }
    } else if (raw_size > 0 && !checkpoint_has_error(cp)) {
        // A blank image (no base file: a blank ProFile).  A quick checkpoint
        // reopens its delta; a consolidated one refills a fresh blank one.
        // This entry used to be dropped, so the disk vanished on restore.
        bool quick = checkpoint_get_kind(cp) != CHECKPOINT_KIND_CONSOLIDATED;
        if (kind != IMAGE_KIND_NONE && raw_size % 512 == 0)
            img = image_create_blank_kind((image_kind_t)kind, raw_size / 512,
                                          quick && instance_path && instance_path[0] ? instance_path : NULL);
        if (!img) {
            out_printf("Error: cannot recreate a blank %s image while restoring checkpoint\n",
                       image_kind_name((image_kind_t)kind));
            checkpoint_set_error(cp);
        }
    }
    // A NULL storage consumes and discards the blob (an entry with no image),
    // keeping the stream in step.  Once the checkpoint is flagged, though, the
    // restore is over -- the caller stops at the first damaged entry -- and
    // reading on would only repeat the failure under another name.
    if (!checkpoint_has_error(cp) && storage_restore_from_checkpoint(image_get_storage(img), cp) != STATUS_OK) {
        out_printf("Error: storage_restore_from_checkpoint failed for %s\n", name ? name : "<unnamed>");
        checkpoint_set_error(cp);
    }
    // A volume that was attached through the wrapper is re-wrapped, so the
    // SCSI device that re-binds to it by name sees the same disk.  After the
    // blocks are restored, not before: the wrapper is built from what the
    // disk holds, and a consolidated checkpoint whose image file was gone
    // restores onto a zero-filled placeholder, where the sniff found no
    // volume, left the disk unwrapped, and handed the guest every block
    // shifted by the wrapper ("The System file on this startup disk may be
    // damaged").  A volume that was wrapped and cannot be again fails the
    // restore rather than coming back at the wrong offsets.
    if (img && (flags & IMAGE_CKPT_WRAPPED) && !checkpoint_has_error(cp) && image_wrap_volume(img) <= IMAGE_WRAP_NONE) {
        out_printf("Error: cannot re-wrap volume %s while restoring checkpoint\n", name);
        checkpoint_set_error(cp);
    }
    free(name);
    free(instance_path);
    free(saved_key);
    return img;
}

void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp) {
    // The count comes off disk and used to be the loop bound directly: a
    // corrupt 0xFFFFFFFF ran four billion iterations, each one re-entering the
    // restore, calling storage_restore_from_checkpoint and logging a line --
    // an effective hang plus a log flood rather than an error.
    uint32_t count = 0;
    if (!checkpoint_read_count(cp, &count, (uint32_t)config_max_images(), "images"))
        return;
    for (uint32_t i = 0; i < count; ++i) {
        // Stop at the first damaged entry rather than grinding through the
        // remainder against an already-failed stream.
        if (checkpoint_has_error(cp))
            return;
        image_t *img = mac_checkpoint_restore_one_image(cp);
        if (img)
            config_add_image(cfg, img);
    }
}
