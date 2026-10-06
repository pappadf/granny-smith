// URL-media blank disks: `?hd0=blank:HD230SC`, `?fd1=blank:800k`.  A value
// with the reserved prefix `blank:` (a URL never starts with it) asks for a
// new blank disk in that slot instead of a download.  Pure, so it is
// unit-tested directly (tests/unit/blankMedia.test.ts); bus/urlMedia.ts
// creates the disk.
//
// The size or model after `blank:` is the core's to parse: on a SCSI or ATA
// bus it goes unchanged to files.hd_create, whose parser
// (drive_catalog_parse_size, src/core/storage/drive_catalog.c) the shell's
// `hd create` and the New Machine dialog's Create blank image share -- a
// catalog model (`HD20SC` … `HD1000SC`), a decimal size that snaps to the
// nearest model at or above it (`80mb`, `1gb`), or an exact binary size
// (`100m`, `512k`).  A Lisa's ProFile port takes the two sizes the dialog
// offers there (`5mb`, `10mb`; files.profile_create), a floppy drive the two
// the core creates (`800k`, `1440k`; files.fd_create).  A 400K floppy is not
// offered: the core has no blank 400K image.
//
// Reloads reuse: the disk is named for its slot, its spec and a short hash of
// the page URL's media parameters (blank_HD230SC_hd0_<hash>.dmg), and a disk
// of that name already in OPFS is attached instead of a new one, so reloading
// the link does not pile up blank images.  `blank!:` replaces it with a fresh
// one.  The guest's writes go to the machine's delta, not to this image (see
// docs/guide/web.md), so a reload of the link starts from the blank image
// again; what the previous machine installed is kept with that machine.
//
// The disk is plain blank, unpartitioned: the guest formats it, as with a
// real new drive.  (A pre-partitioned HFS option, `blank:HD80SC,hfs`, is not
// implemented; a value with a comma is refused.)

import type { BlankDisk } from '@/bus/profile';

// `blank:` / `blank!:` (any case), then the spec.
const BLANK_PREFIX = /^blank(!?):/i;

// A blank-disk URL value, parsed.
export interface BlankSpec {
  // What follows the prefix, trimmed: a model or size (`HD230SC`, `80mb`).
  spec: string;
  // `blank!:`: replace a disk an earlier load of this URL created.
  fresh: boolean;
}

// The blank-disk request in a URL-media value, or null when the value is a
// URL to fetch.
export function parseBlankValue(value: string): BlankSpec | null {
  const v = value.trim();
  const m = BLANK_PREFIX.exec(v);
  if (!m) return null;
  return { spec: v.slice(m[0].length).trim(), fresh: m[1] === '!' };
}

// A short, stable hash of the page URL's media parameters (`[name, value]`
// pairs, names canonical): the same link names the same blank disks on
// every load.  `blank!:` hashes as `blank:`, so the forced fresh disk
// replaces the one the plain link reuses.  FNV-1a, 32 bits, 8 hex digits.
export function blankUrlHash(media: Array<[string, string]>): string {
  const key = media
    .map(([k, v]) => {
      const b = parseBlankValue(v);
      return `${k}=${b ? `blank:${b.spec}` : v.trim()}`;
    })
    .join('\n');
  let h = 0x811c9dc5;
  for (const byte of new TextEncoder().encode(key)) {
    h ^= byte;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h.toString(16).padStart(8, '0');
}

// The file name of `slot`'s blank disk: `blank_<spec>_<slot>_<hash><ext>`,
// the spec as typed with anything but letters, digits, '.' and '-' made '_'.
export function blankDiskName(slot: string, spec: string, hash: string, ext: string): string {
  const safe = spec.replace(/[^A-Za-z0-9.-]/g, '_') || 'disk';
  return `blank_${safe}_${slot}_${hash}${ext}`;
}

// How to create one blank disk: the files.* method and its size argument
// (after the path), and the new file's extension; or why it cannot be.
export type BlankPlan =
  { ok: true; method: string; arg: string | boolean; ext: string } | { ok: false; reason: string };

// What every spec must be before a bus is asked: non-empty, no options.
function specProblem(spec: string): string | null {
  if (!spec) return 'blank: needs a drive model or size (blank:HD80SC, blank:80mb, blank:100m)';
  if (spec.includes(','))
    return `"${spec}": options after the size are not supported (the disk is created blank, for the guest to format)`;
  return null;
}

// The plan for a blank floppy: 800K or 1440K, as the dialog creates them.
export function floppyBlankPlan(spec: string): BlankPlan {
  const problem = specProblem(spec);
  if (problem) return { ok: false, reason: problem };
  const s = spec.toLowerCase().replace(/\s+/g, '');
  if (s === '800k' || s === '800kb')
    return { ok: true, method: 'files.fd_create', arg: false, ext: '.dsk' };
  if (['1440k', '1440kb', '1.44mb', '1.4mb'].includes(s))
    return { ok: true, method: 'files.fd_create', arg: true, ext: '.dsk' };
  if (s === '400k' || s === '400kb')
    return {
      ok: false,
      reason: 'a blank 400K floppy cannot be created; blank floppies are 800k or 1440k',
    };
  return { ok: false, reason: `"${spec}" is not a floppy size; blank floppies are 800k or 1440k` };
}

// The ProFile size an entry of the bus's blank disks is (its name stem
// ends in _<n>MB: blank_profile_5MB), or null.
function profileMb(disk: BlankDisk): number | null {
  const m = /_(\d+)MB$/i.exec(disk.name);
  return m ? Number(m[1]) : null;
}

// The plan for a blank hard disk on a bus that takes `disks` (the bus's
// blank_disks, bus/profile.ts): the spec as the core's size argument where
// the bus's disks are files.hd_create ones (the core parses and validates
// it), else -- the ProFile port -- the entry whose size the spec names.
export function hardDiskBlankPlan(spec: string, disks: BlankDisk[]): BlankPlan {
  const problem = specProblem(spec);
  if (problem) return { ok: false, reason: problem };
  const hd = disks.find((d) => d.method === 'files.hd_create');
  if (hd) return { ok: true, method: hd.method, arg: spec, ext: hd.ext };
  const sized = disks.filter((d) => profileMb(d) !== null);
  if (!sized.length) return { ok: false, reason: 'this drive takes no blank disk' };
  const m = /^(\d+)\s*(mb?)?$/i.exec(spec);
  const hit = m ? sized.find((d) => profileMb(d) === Number(m[1])) : undefined;
  if (hit) return { ok: true, method: hit.method, arg: hit.arg, ext: hit.ext };
  const offered = sized.map((d) => `${profileMb(d)}mb`).join(' or ');
  return { ok: false, reason: `"${spec}" is not a size this drive takes; it takes ${offered}` };
}

// The reason shown when the core refuses a files.hd_create spec: not a
// catalog model or size, or out of range.
export function hardDiskSpecRefused(spec: string): string {
  return (
    `could not create a blank disk of "${spec}": give a drive model (HD20SC … HD1000SC), ` +
    'a size that snaps to one (80mb, 1gb), or an exact size up to 2 GB (100m)'
  );
}
