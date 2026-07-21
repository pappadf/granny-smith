// Media-type descriptor map. Each entry knows where to persist its kind in
// OPFS, how to validate a candidate file via gsEval, and how to derive its
// final filename. Mirrors app/web/js/media-types.js.

import { ROMS_DIR, VROMS_DIR, FD_DIR, HD_DIR, CD_DIR } from './opfsPaths';

export type MediaTypeId = 'rom' | 'vrom' | 'fd' | 'hd' | 'cdrom';

export interface ValidateResult {
  valid: boolean;
  info?: {
    checksum?: string;
    persistDir?: string;
    [k: string]: unknown;
  };
}

export interface MediaTypeDescriptor {
  id: MediaTypeId;
  label: string;
  persistDir: string;
  validate(path: string, gsEval: GsEval): Promise<ValidateResult>;
  nameFn?(originalName: string, info?: ValidateResult['info']): string;
}

// Function type for the gsEval injection — keeps lib/ free of bus imports.
export type GsEval = (path: string, args?: unknown[]) => Promise<unknown>;

// Common floppy disk sizes (matches detect_diskcopy: 400/800/1440 KB, +84 if
// the image is wrapped with a DiskCopy 4.2 header without sector tags).
const FD_400 = 400 * 1024;
const FD_800 = 800 * 1024;
const FD_HD = 1440 * 1024;
const DC42_HEADER = 0x54;

interface RomIdentifyResult {
  recognised?: boolean;
  compatible?: string[];
  checksum?: string;
  name?: string;
  size?: number;
}

// Shape returned by C-side `machine.vrom.identify`. Identity is keyed off the
// declaration ROM's NuBus Format-Block CRC (the analog of rom.identify's
// checksum); `card_id` is the nubus card-kind the blob provides and
// `compatible` mirrors rom.identify's `compatible:[model_ids]` shape (the card
// ids this vROM can drive, usually length 1). Unrecognised files come back as
// { recognised: false, size?, crc? } — see src/core/memory/vrom.c. The
// human-readable card name is owned by the card kind (machine.profile), not here.
interface VromIdentifyResult {
  recognised: boolean;
  card_id?: string;
  compatible?: string[];
  size?: number;
  crc?: string;
}

async function parseRomIdentify(gsEval: GsEval, path: string): Promise<RomIdentifyResult | null> {
  const r = await gsEval('machine.rom.identify', [path]);
  if (r === null || r === undefined) return null;
  if (typeof r === 'object' && r !== null && 'error' in (r as object)) return null;
  if (typeof r !== 'string') return null;
  try {
    return JSON.parse(r) as RomIdentifyResult;
  } catch {
    return null;
  }
}

export const MEDIA_TYPES: Record<MediaTypeId, MediaTypeDescriptor> = {
  rom: {
    id: 'rom',
    label: 'ROM image',
    persistDir: ROMS_DIR,
    async validate(path, gsEval) {
      const info = await parseRomIdentify(gsEval, path);
      if (!info?.recognised) return { valid: false };
      return { valid: true, info: { checksum: info.checksum } };
    },
    // ROMs are stored by checksum, not original filename.
    nameFn(originalName, info) {
      return (info?.checksum as string) || originalName;
    },
  },

  vrom: {
    id: 'vrom',
    label: 'Video ROM image',
    persistDir: VROMS_DIR,
    async validate(path, gsEval) {
      const r = await gsEval('machine.vrom.identify', [path]);
      if (typeof r !== 'string') return { valid: false };
      try {
        const parsed = JSON.parse(r) as VromIdentifyResult;
        if (!parsed.recognised) return { valid: false };
        return {
          valid: true,
          info: {
            cardId: parsed.card_id,
            compatible: parsed.compatible,
            checksum: parsed.crc,
          },
        };
      } catch {
        return { valid: false };
      }
    },
    // VROMs are stored by content hash (the declaration ROM's Format-Block
    // CRC), mirroring how CPU ROMs are stored by checksum. Discovery is
    // content-based (the core's offer registry), so the on-disk name never
    // matters — and the UI carries no naming grammar of its own. The
    // identify payload's crc is "0x"-prefixed; strip it for the filename.
    nameFn(originalName, info) {
      const crc = info?.checksum as string | undefined;
      return crc ? crc.replace(/^0x/, '') : originalName;
    },
  },

  fd: {
    id: 'fd',
    label: 'Floppy Disk image',
    persistDir: FD_DIR,
    async validate(path, gsEval) {
      // The Configuration slide validates before machine.boot, so the
      // per-machine `floppy` object isn't on the root yet. Use the size-
      // based classifier (same as image.c::classify_image + detect_diskcopy).
      // All floppy densities — 400 KB / 800 KB / 1.44 MB, with or without a
      // DiskCopy 4.2 header — live together under FD_DIR so the Images tab's
      // single "fd" section lists them all. (An earlier revision split HD
      // floppies into a separate /opfs/images/fdhd/ that no category ever
      // scanned, so they became invisible; see BrowserOpfs.scanImages, which
      // still folds any stragglers from that directory back in.)
      const size = (await gsEval('storage.path_size', [path])) as number | null;
      if (typeof size !== 'number') return { valid: false };
      const recognised =
        size === FD_400 ||
        size === FD_400 + DC42_HEADER ||
        size === FD_800 ||
        size === FD_800 + DC42_HEADER ||
        size === FD_HD ||
        size === FD_HD + DC42_HEADER;
      if (!recognised) return { valid: false };
      return { valid: true };
    },
  },

  hd: {
    id: 'hd',
    label: 'Hard Disk image',
    persistDir: HD_DIR,
    async validate(path, gsEval) {
      return { valid: (await gsEval('machine.scsi.identify_hd', [path])) === true };
    },
  },

  cdrom: {
    id: 'cdrom',
    label: 'CD-ROM image',
    persistDir: CD_DIR,
    async validate(path, gsEval) {
      return { valid: (await gsEval('machine.scsi.identify_cdrom', [path])) === true };
    },
  },
};
