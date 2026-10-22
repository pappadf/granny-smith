#!/usr/bin/env python3
# Build the zips the archive-expand-compressed row lists:
#   stored.zip   -- the floppy image stored (no compression)
#   deflated.zip -- the same image deflated
#   big.zip      -- the image padded with zeros to 65 MB, deflated: over the
#                   64 MB a listing decodes, so it is not probed
# Usage: make_fixtures.py <floppy.dsk> <out-dir>

import sys
import zipfile

disk_path, out = sys.argv[1], sys.argv[2]
with open(disk_path, "rb") as f:
    disk = f.read()

with zipfile.ZipFile(f"{out}/stored.zip", "w", zipfile.ZIP_STORED) as z:
    z.writestr("System.dsk", disk)
with zipfile.ZipFile(f"{out}/deflated.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("System.dsk", disk)

# Streamed in, so the padded image is never held whole (it deflates to a
# few hundred KB).
big = 65 * 1024 * 1024
with zipfile.ZipFile(f"{out}/big.zip", "w", zipfile.ZIP_DEFLATED) as z:
    with z.open("Big.dsk", "w", force_zip64=True) as m:
        m.write(disk)
        zeros = bytes(1024 * 1024)
        left = big - len(disk)
        while left > 0:
            n = min(left, len(zeros))
            m.write(zeros[:n])
            left -= n
