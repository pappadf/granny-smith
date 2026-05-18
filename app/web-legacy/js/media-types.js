// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Media type descriptors for the unified upload pipeline.
// Each descriptor defines how to validate and persist a specific media type.

import { ROMS_DIR, VROMS_DIR, FD_DIR, FDHD_DIR, HD_DIR, CD_DIR } from './config.js';

// Media type descriptor registry.
// Each entry provides:
//   id        — unique key (e.g. 'rom', 'fd')
//   label     — human-readable name shown in error messages
//   persistDir — default OPFS target directory
//   validate  — async (path) => { valid, info? }
//   nameFn    — optional: transform filename for persistence
//
// All validate() implementations route through gsEval. The C-side root
// methods return typed values (V_BOOL for the validate family, V_STRING
// for rom_checksum / fd_validate density), removing the previous
// runCommandJSON + stdout-parsing detour.
export const MEDIA_TYPES = {
  rom: {
    id: 'rom',
    label: 'ROM image',
    persistDir: ROMS_DIR,
    async validate(path) {
      // rom.identify returns the file's full info map; recognised==true means
      // the checksum matches an entry in the C-side ROM_TABLE.
      const info = await window.romIdentify(path);
      if (!info || !info.recognised) return { valid: false };
      return { valid: true, info: { checksum: info.checksum } };
    },
    // ROMs are stored by checksum, not original filename
    nameFn(originalName, info) {
      return info?.checksum || originalName;
    },
  },

  vrom: {
    id: 'vrom',
    label: 'Video ROM image',
    persistDir: VROMS_DIR,
    async validate(path) {
      return { valid: (await window.gsEval('vrom.identify', [path])) === true };
    },
  },

  fd: {
    id: 'fd',
    label: 'Floppy Disk image',
    persistDir: FD_DIR,
    async validate(path) {
      // The config dialog runs *before* machine.boot — at that point the
      // per-machine `floppy` object has not been registered on the
      // object root, so `floppy.identify` would fail with "did not
      // resolve".  Match the C-side classifier (image.c::classify_image
      // + detect_diskcopy) using only the file size, which is the
      // single discriminator for raw and DiskCopy 4.2 wrapped floppies
      // without sector tags:
      //   400 KB / 800 KB / 1.4 MB raw, or +84 for the DC42 header.
      // Floppy disk sizes have been frozen since the 1980s, so this
      // duplicated table is stable.  Tagged DC42 images (common only
      // for very old MFS disks) are not handled here — they would need
      // a magic-byte read.
      const size = await window.gsEval('storage.path_size', [path]);
      const FD_400 = 400 * 1024, FD_800 = 800 * 1024, FD_HD = 1440 * 1024;
      const DC42 = 0x54;
      let isHD = false, recognised = false;
      if (size === FD_400 || size === FD_400 + DC42) recognised = true;
      else if (size === FD_800 || size === FD_800 + DC42) recognised = true;
      else if (size === FD_HD || size === FD_HD + DC42) { recognised = true; isHD = true; }
      if (!recognised) return { valid: false };
      return { valid: true, info: { persistDir: isHD ? FDHD_DIR : FD_DIR } };
    },
  },

  hd: {
    id: 'hd',
    label: 'Hard Disk image',
    persistDir: HD_DIR,
    async validate(path) {
      return { valid: (await window.gsEval('scsi.identify_hd', [path])) === true };
    },
  },

  cdrom: {
    id: 'cdrom',
    label: 'CD-ROM image',
    persistDir: CD_DIR,
    async validate(path) {
      return { valid: (await window.gsEval('scsi.identify_cdrom', [path])) === true };
    },
  },
};
