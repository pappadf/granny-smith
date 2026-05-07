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
      // fd_validate returns the density tag ("400K" / "800K" / "1.4MB")
      // for valid floppy images, empty otherwise. Pick FDHD vs FD based
      // on the tag.
      const density = await window.gsEval('floppy.identify', [path]);
      if (typeof density !== 'string' || !density) return { valid: false };
      const isHD = density === '1.4MB';
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
