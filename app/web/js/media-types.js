// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Media type descriptors for the unified upload pipeline.
// Each descriptor defines how to validate and persist a specific media type.

import { ROMS_DIR, VROMS_DIR, FD_DIR, FDHD_DIR, HD_DIR, CD_DIR } from './config.js';
import { quotePath } from './media.js';

// Media type descriptor registry.
// Each entry provides:
//   id        — unique key (e.g. 'rom', 'fd')
//   label     — human-readable name shown in error messages
//   persistDir — default OPFS target directory
//   validate  — async (path) => { valid, info? }
//   nameFn    — optional: transform filename for persistence
export const MEDIA_TYPES = {
  rom: {
    id: 'rom',
    label: 'ROM image',
    persistDir: ROMS_DIR,
    async validate(path) {
      // rom checksum returns cmd_int (0 = success) + prints checksum to stdout
      const res = await window.runCommandJSON(`rom checksum ${quotePath(path)}`);
      if (res.status !== 'ok' || res.value !== 0) return { valid: false };
      const checksum = (res.output || '').trim();
      if (!checksum || checksum === '0') return { valid: false };
      return { valid: true, info: { checksum } };
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
      // vrom probe returns cmd_int (0 = valid, 1 = invalid)
      const res = await window.runCommandJSON(`vrom probe ${quotePath(path)}`);
      return { valid: res.status === 'ok' && res.value === 0 };
    },
  },

  fd: {
    id: 'fd',
    label: 'Floppy Disk image',
    persistDir: FD_DIR,
    async validate(path) {
      // fd validate returns cmd_bool (true = valid) + prints density to stdout
      const res = await window.runCommandJSON(`fd validate ${quotePath(path)}`);
      if (res.status !== 'ok' || res.value !== true) return { valid: false };
      // Parse density from stdout to choose FD vs FDHD directory
      const output = res.output || '';
      const isHD = output.includes('1.4MB') || output.includes('high-density');
      return { valid: true, info: { persistDir: isHD ? FDHD_DIR : FD_DIR } };
    },
  },

  hd: {
    id: 'hd',
    label: 'Hard Disk image',
    persistDir: HD_DIR,
    async validate(path) {
      // hd validate returns cmd_bool (true = valid)
      const res = await window.runCommandJSON(`hd validate ${quotePath(path)}`);
      return { valid: res.status === 'ok' && res.value === true };
    },
  },

  cdrom: {
    id: 'cdrom',
    label: 'CD-ROM image',
    persistDir: CD_DIR,
    async validate(path) {
      // cdrom validate returns cmd_bool (true = valid)
      const res = await window.runCommandJSON(`cdrom validate ${quotePath(path)}`);
      return { valid: res.status === 'ok' && res.value === true };
    },
  },
};
