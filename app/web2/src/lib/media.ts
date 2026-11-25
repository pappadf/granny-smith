// Media-type descriptor map. Each entry knows where to persist its kind in
// OPFS, how to validate a candidate file via gsEval, and how to derive its
// final filename. Mirrors app/web/js/media-types.js.

import { ROMS_DIR, VROMS_DIR, PROMS_DIR, FD_DIR, HD_DIR, CD_DIR } from './opfsPaths';
import { isGsObject } from './gsResult';

export type MediaTypeId = 'rom' | 'vrom' | 'prom' | 'fd' | 'hd' | 'cdrom';

// Above this, a hard-disk or CD image is imported streamed into a compact
// UDIF (bus/importImage.ts) instead of staged raw and copied.  It only has
// to exceed the largest ROM and floppy, which keep the staged flow.
export const LARGE_IMPORT_BYTES = 16 * 1024 * 1024;

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

// What the core makes of a file (`files.identify`, src/core/storage/
// storage_class.c): whether the image layer opens it and why not, the
// archive it is, the floppy layout its size implies, which kinds of drive
// take it (`fits`: floppy, hd, cd, profile) and the one an upload of no
// stated kind is filed as (`kind`), and its Mac OS volumes.  The one
// identify for every drive; it answers before a machine exists.
export interface FileIdentity {
  opens: boolean;
  refusal: string;
  archive: string;
  floppy: string;
  fits: string[];
  kind: string;
  volumes: unknown[];
}

// The file at `path` as the core identifies it, or null when it cannot say.
export async function identifyFile(gsEval: GsEval, path: string): Promise<FileIdentity | null> {
  const r = await gsEval('files.identify', [path]);
  if (!isGsObject(r)) return null;
  const o = r as Record<string, unknown>;
  return {
    opens: o.opens === true,
    refusal: typeof o.refusal === 'string' ? o.refusal : '',
    archive: typeof o.archive === 'string' ? o.archive : '',
    floppy: typeof o.floppy === 'string' ? o.floppy : '',
    fits: Array.isArray(o.fits) ? o.fits.filter((k): k is string => typeof k === 'string') : [],
    kind: typeof o.kind === 'string' ? o.kind : '',
    volumes: Array.isArray(o.volumes) ? o.volumes : [],
  };
}

// The validate of a drive kind: valid when a drive of `kind` takes the file,
// else the core's reason when the file is a container it cannot read (a
// UDIF, NDIF, QCOW2 or MOOF): "is a QCOW2 image that cannot be read in place
// (it uses ...)".  The upload said only "is not a valid Hard Disk image".
async function validateFits(path: string, gsEval: GsEval, kind: string): Promise<ValidateResult> {
  const id = await identifyFile(gsEval, path);
  if (id?.fits.includes(kind)) return { valid: true };
  return { valid: false, reject: id?.refusal ? `is ${id.refusal}` : undefined };
}

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
  // The known ROM whose id the file carries at another size (a truncated
  // dump); "" / 0 otherwise.
  expected_name?: string;
  expected_size?: number;
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
// reachable PCIR, code type 1 = Open Firmware, and a catalogued identity made
// of the PCIR vendor/device ids and the FCode header's own checksum, which
// must verify). A vROM answers `crc` (its Format-Block CRC), a PROM `id`
// ("vvvv-dddd-cccc"). An unrecognised file may still carry `reason`, which is
// how "that is a PC/x86 option ROM" reaches the user instead of a shrug.
interface PromIdentifyResult {
  recognised: boolean;
  card_id?: string;
  compatible?: string[];
  size?: number;
  crc?: string;
  id?: string;
  intact?: boolean;
  reason?: string;
}

async function parseRomIdentify(gsEval: GsEval, path: string): Promise<RomIdentifyResult | null> {
  // rom.identify returns a native object (VK_MAP) — no inner JSON.parse.
  const r = await gsEval('machine.rom.identify', [path]);
  if (!isGsObject(r)) return null;
  return r as RomIdentifyResult;
}

async function parseCardRomIdentify(
  gsEval: GsEval,
  what: 'vrom' | 'prom',
  path: string,
): Promise<PromIdentifyResult | null> {
  // catalog.vroms.identify / catalog.proms.identify return a native object (VK_MAP).
  const r = await gsEval(`catalog.${what}s.identify`, [path]); // catalog.vroms / catalog.proms
  if (!isGsObject(r)) return null;
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
      // A known ROM's id at the wrong size: a dump cut short.  It used to fall
      // through to the permissive hard-disk probe and be stored as a disk.
      if (!info?.recognised && info?.expected_size && info.size)
        return {
          valid: false,
          reject: `looks like the ${info.expected_name}, but it is ${info.size / 1024} KB where that ROM is ${info.expected_size / 1024} KB — the dump is probably damaged`,
        };
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
      if (!parsed?.recognised) {
        // A real PCI expansion ROM the emulator cannot use -- a card it does
        // not emulate, a damaged dump, a PC/x86 option ROM -- is refused with
        // the reason.  It used to fall through to the permissive hard-disk
        // probe and be stored as a 128 KB "hard disk".
        if (parsed?.id || parsed?.reason?.startsWith('an expansion ROM'))
          return {
            valid: false,
            reject: parsed.reason?.includes('no catalog row')
              ? `is a PCI expansion ROM (${parsed.id}) for a card Granny Smith does not emulate`
              : `is ${parsed.reason}`,
          };
        return { valid: false };
      }
      return {
        valid: true,
        info: {
          cardId: parsed.card_id,
          compatible: parsed.compatible,
          id: parsed.id,
        },
      };
    },
    // Stored by content identity ("vvvv-dddd-cccc"), like a vROM by its CRC:
    // discovery is the core's offer registry matching on content, so the
    // on-disk name is a handle and never a fact the UI reasons about.
    nameFn(originalName, info) {
      return (info?.id as string | undefined) || originalName;
    },
  },

  fd: {
    id: 'fd',
    label: 'Floppy Disk image',
    persistDir: FD_DIR,
    async validate(path, gsEval) {
      // Whatever wraps it -- a DiskCopy 4.2 header and tags, a MOOF, a
      // floppy inside a .dmg -- the image layer decides from the disk it
      // opens.  All floppy densities (400 KB / 800 KB / 720 KB / 1.44 MB)
      // live together under FD_DIR so the Images tab's single "fd" section
      // lists them all.
      return validateFits(path, gsEval, 'floppy');
    },
  },

  hd: {
    id: 'hd',
    label: 'Hard Disk image',
    persistDir: HD_DIR,
    async validate(path, gsEval) {
      // A disk smaller than the smallest floppy is refused (a ROM, a card's
      // ROM or a cut-short dump, S1018-26).  The core measures the disk, not
      // the file: a UDIF or QCOW2 of a mostly empty disk is far smaller than
      // the disk it holds, so the file's size says nothing here.
      return validateFits(path, gsEval, 'hd');
    },
  },

  cdrom: {
    id: 'cdrom',
    label: 'CD-ROM image',
    persistDir: CD_DIR,
    async validate(path, gsEval) {
      return validateFits(path, gsEval, 'cd');
    },
  },
};

// Validate the file at `path` as `id` for an upload of no stated kind (a
// drop, Home's Load ROM): as MEDIA_TYPES[id] does, except that a CD-ROM is
// only an image holding ISO 9660.  A bare HFS or partitioned image is a hard
// disk then (S1018-06): the CD-ROM section's own Add, which validates with
// MEDIA_TYPES.cdrom, still takes one as a disc.
export async function validateUnstated(
  id: MediaTypeId,
  path: string,
  gsEval: GsEval,
): Promise<ValidateResult> {
  const result = await MEDIA_TYPES[id].validate(path, gsEval);
  if (id !== 'cdrom' || !result.valid) return result;
  if ((await identifyFile(gsEval, path))?.kind === 'cd') return result;
  return { valid: false };
}
