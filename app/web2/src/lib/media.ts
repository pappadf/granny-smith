// Media-type descriptor map. Each entry knows where to persist its kind in
// OPFS, how to validate a candidate file via gsEval, and how to derive its
// final filename. Mirrors app/web/js/media-types.js.

import { ROMS_DIR, VROMS_DIR, PROMS_DIR, FD_DIR, HD_DIR, CD_DIR } from './opfsPaths';

export type MediaTypeId = 'rom' | 'vrom' | 'prom' | 'fd' | 'hd' | 'cdrom';

export interface ValidateResult {
  valid: boolean;
  // Set with valid:false when the file IS this media type but is refused
  // (a damaged ROM dump, a ROM of a machine that is not emulated).  The text
  // completes a sentence that starts with the file's name, and the upload
  // stops there instead of trying the next media type.
  reject?: string;
  info?: {
    checksum?: string;
    id?: string;
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

// Shape returned by C-side `machine.rom.identify` (src/core/memory/rom.c).
// `id` is the ROM's own stored checksum, the only thing a ROM file is ever
// named by, and only when `intact`.  `recognised` = the core's ROM table knows
// it; `supported` = it boots at least one emulated model (`compatible`).
interface RomIdentifyResult {
  recognised?: boolean;
  supported?: boolean;
  compatible?: string[];
  name?: string;
  variant?: string;
  size?: number;
  kind?: string;
  id?: string;
  intact?: boolean;
  reason?: string;
}

// Shape returned by C-side `catalog.vroms.identify`. Identity is keyed off the
// declaration ROM's NuBus Format-Block CRC (the analog of rom.identify's
// checksum); `card_id` is the nubus card-kind the blob provides and
// `compatible` mirrors rom.identify's `compatible:[model_ids]` shape (the card
// ids this vROM can drive, usually length 1). Unrecognised files come back as
// { recognised: false, size?, crc? } — see src/core/memory/vrom.c. The
// human-readable card name is owned by the card kind (catalog.profile), not here.
interface VromIdentifyResult {
  recognised: boolean;
  card_id?: string;
  compatible?: string[];
  size?: number;
  crc?: string;
}

// Shape returned by C-side `catalog.proms.identify` — a PCI expansion ROM.
// Deliberately the same shape as VromIdentifyResult, because to the UI the
// two are the same question ("which card does this blob provide?"); the
// identity rules behind them are not (see src/core/memory/prom.c: $55AA, a
// reachable PCIR, code type 1 = Open Firmware, and a catalogued CRC-32 of
// the whole chip image). An unrecognised file may still carry `reason`,
// which is how "that is a PC/x86 option ROM" reaches the user instead of a
// shrug.
interface PromIdentifyResult {
  recognised: boolean;
  card_id?: string;
  compatible?: string[];
  size?: number;
  crc?: string;
  reason?: string;
}

async function parseRomIdentify(gsEval: GsEval, path: string): Promise<RomIdentifyResult | null> {
  // rom.identify returns a native object (V_MAP) — no inner JSON.parse.
  const r = await gsEval('machine.rom.identify', [path]);
  if (!r || typeof r !== 'object' || 'error' in (r as object)) return null;
  return r as RomIdentifyResult;
}

async function parseCardRomIdentify(
  gsEval: GsEval,
  what: 'vrom' | 'prom',
  path: string,
): Promise<PromIdentifyResult | null> {
  // catalog.vroms.identify / catalog.proms.identify return a native object (V_MAP).
  const r = await gsEval(`catalog.${what}s.identify`, [path]); // catalog.vroms / catalog.proms
  if (!r || typeof r !== 'object' || 'error' in (r as object)) return null;
  return r as PromIdentifyResult;
}

// --- The identify wrappers, one each ----------------------------------------
// rom.identify was wrapped four times and vrom/prom.identify twice each, with
// three different result shapes.  These are the only ones.

// A recognised CPU ROM: the models it boots.
export interface RomIdentity {
  path: string;
  name: string;
  // What tells this ROM apart from the other ROMs of its models (core-owned,
  // e.g. "Open Firmware 2.26NT (Windows NT)"); "" when it is the only one.
  variant: string;
  id: string; // content id: the ROM's own stored checksum (rom.identify)
  intact: boolean;
  compatible: string[];
  size: number;
}

// A recognised card ROM (a NuBus vROM or a PCI expansion ROM): the card it
// provides and the cards it can drive.
export interface CardRomIdentity {
  path: string;
  cardId: string;
  compatible: string[];
}

// The ROM at `path`, or null when the core does not recognise it or it boots
// no emulated model (a known ROM of a machine Granny Smith does not emulate).
export async function identifyRom(gsEval: GsEval, path: string): Promise<RomIdentity | null> {
  const r = await parseRomIdentify(gsEval, path);
  if (!r?.recognised || !r.supported || !Array.isArray(r.compatible)) return null;
  return {
    path,
    name: r.name || path.split('/').pop() || path,
    variant: r.variant ?? '',
    id: r.id ?? '',
    intact: r.intact ?? false,
    compatible: r.compatible,
    size: r.size ?? 0,
  };
}

// The vROM (`what` = 'vrom') or PCI expansion ROM ('prom') at `path`, or null.
export async function identifyCardRom(
  gsEval: GsEval,
  what: 'vrom' | 'prom',
  path: string,
): Promise<CardRomIdentity | null> {
  const r = await parseCardRomIdentify(gsEval, what, path);
  if (!r?.recognised || !r.card_id) return null;
  return {
    path,
    cardId: r.card_id,
    compatible: Array.isArray(r.compatible) ? r.compatible : [r.card_id],
  };
}

export const MEDIA_TYPES: Record<MediaTypeId, MediaTypeDescriptor> = {
  rom: {
    id: 'rom',
    label: 'ROM image',
    persistDir: ROMS_DIR,
    async validate(path, gsEval) {
      const info = await parseRomIdentify(gsEval, path);
      if (!info?.recognised) return { valid: false };
      // A known ROM that fails its own checksum is never stored: its id is
      // the good dump's, and a damaged ROM in the picker helps nobody.
      if (!info.intact)
        return {
          valid: false,
          reject: `looks like the ${info.name}, but its ${info.reason} — the dump is probably damaged`,
        };
      if (!info.supported)
        return {
          valid: false,
          reject: `is the ${info.name}; Granny Smith does not emulate that machine`,
        };
      return { valid: true, info: { id: info.id } };
    },
    // ROMs are stored by content id (the ROM's own stored checksum), not
    // original filename.
    nameFn(originalName, info) {
      return (info?.id as string) || originalName;
    },
  },

  vrom: {
    id: 'vrom',
    label: 'Video ROM image',
    persistDir: VROMS_DIR,
    async validate(path, gsEval) {
      const parsed: VromIdentifyResult | null = await parseCardRomIdentify(gsEval, 'vrom', path);
      if (!parsed?.recognised) return { valid: false };
      return {
        valid: true,
        info: {
          cardId: parsed.card_id,
          compatible: parsed.compatible,
          checksum: parsed.crc,
        },
      };
    },
    // VROMs are stored by content hash (the declaration ROM's Format-Block
    // CRC), mirroring how CPU ROMs are stored by content id. Discovery is
    // content-based (the core's offer registry), so the on-disk name never
    // matters — and the UI carries no naming grammar of its own. The
    // identify payload's crc is "0x"-prefixed; strip it for the filename.
    nameFn(originalName, info) {
      const crc = info?.checksum as string | undefined;
      return crc ? crc.replace(/^0x/, '') : originalName;
    },
  },

  prom: {
    id: 'prom',
    label: 'PCI expansion ROM',
    persistDir: PROMS_DIR,
    async validate(path, gsEval) {
      const parsed = await parseCardRomIdentify(gsEval, 'prom', path);
      if (!parsed?.recognised) return { valid: false };
      return {
        valid: true,
        info: {
          cardId: parsed.card_id,
          compatible: parsed.compatible,
          checksum: parsed.crc,
        },
      };
    },
    // Stored by content hash, exactly like a vROM: discovery is the core's
    // offer registry matching on content, so the on-disk name is a handle
    // and never a fact the UI reasons about.
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
      const size = (await gsEval('files.path_size', [path])) as number | null;
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
