// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// medium_export.h
// The body of a drive's medium `export(path, [codec])` method, shared by the
// floppy disk node (machine.floppy.drive[N].disk) and the SCSI image node
// (machine.scsi.device[N].image), which carried it twice with only the
// "disk.export" / "image.export" prefix apart.
//
// The export is an I/O job (io_leaf_export_image, io_leaf.h): the disk is
// snapshotted and write-locked here, streamed to the file on the I/O worker,
// and the answer comes when it is done.  The declaration is weak: a unit
// suite of a controller alone links no I/O leaves, and exports here and now.

#ifndef MEDIUM_EXPORT_H
#define MEDIUM_EXPORT_H

#include "image.h"
#include "value.h"

value_t io_leaf_export_image(struct image *img, const char *dest, const char *codec, const char *what)
    __attribute__((weak));

// Writes `img` as it is now (base + delta) to the NEW file `dest` (`codec`:
// a .dmg's compression, NULL for zlib); `what` prefixes the errors.  Never
// overwrites, never touches the source.
static inline value_t medium_export_image(image_t *img, const char *dest, const char *codec, const char *what) {
    if (io_leaf_export_image)
        return io_leaf_export_image(img, dest, codec, what);
    if (image_export_to_codec(img, dest, codec) != 0)
        return val_err("%s: failed to write '%s' (refuses to overwrite an existing file)", what, dest);
    return val_bool(true);
}

#endif // MEDIUM_EXPORT_H
