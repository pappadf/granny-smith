// Streamed import of disk images: the one way a large hard-disk or CD image
// enters /opfs/images.
//
// The browser charges a file's logical length against the origin's quota, so
// an image stored expanded costs its full size even when most of it is
// zeros, and staging it before storing it costs that twice.  Here the decoded
// bytes of an upload, a URL body, a zip member or a gzip stream go straight
// into the core's UDIF writer through the transfer window (files.udif_open /
// udif_append / udif_finish, bus/xfer.ts): zero runs cost nothing, the rest
// is deflated in 64 KB chunks, and the expanded image never exists -- not in
// OPFS, not in JS memory, not in the wasm heap.  The result is written to
// /opfs/upload/<name>.dmg.part, validated as a CD or hard disk through the
// image layer (which reads UDIF in place), then moved into
// /opfs/images/<category>/<name>.dmg.  A failure anywhere removes the .part.
//
// Small files (ROMs, floppies, tiny disks: at most LARGE_IMPORT_BYTES) keep
// the staged-raw flow, through the caller's `onSmall`: a stream of unknown
// length is gathered in memory up to that size, and only goes to the writer
// once it is bigger.  Mac archives (StuffIt, Compact Pro, BinHex, MacBinary)
// are not unpacked here; the caller stages them as it always has.

import { gsEval, gsErrorText } from './emulator';
import { streamToOpfs } from './upload';
import { xferChunkBytes, xferUdifAppend, xferRead } from './xfer';
import { showNotification } from '@/state/toasts.svelte';
import { bumpImagesRevision } from '@/state/images.svelte';
import { setActivityCancel, setActivityDetail } from '@/state/activity.svelte';
import { sanitizeName } from '@/lib/archive';
import { UPLOAD_DIR } from '@/lib/opfsPaths';
import { MEDIA_TYPES, LARGE_IMPORT_BYTES, type MediaTypeId } from '@/lib/media';
import { findMember } from '@/lib/mediaUrl';
import { zipEntries, sniffContainer, decompressor, ZipStreamError } from '@/lib/zipStream';

export type DiskCategory = Extract<MediaTypeId, 'hd' | 'cdrom'>;

export type ImportSource =
  | { kind: 'blob'; blob: Blob }
  | { kind: 'stream'; stream: ReadableStream<Uint8Array>; total: number | null };

export interface ImportOptions {
  // What the image may be stored as, in the order tried.
  categories: DiskCategory[];
  // A zip member to take (forward-only, by name); null/absent: the first
  // member that is a medium.
  member?: string | null;
  // The small-file flow: `path` is the raw file staged in /opfs/upload,
  // `name` its own name.  Answers the path to use (stored or staged), or
  // null when nothing was kept.  Absent: small files are imported as disks
  // too.
  onSmall?: (path: string, name: string) => Promise<string | null>;
  // Progress (bytes of the source read; its length when known; bytes stored).
  onProgress?: (read: number, total: number | null, stored: number) => void;
}

export interface ImportOutcome {
  // false: the source is a container this pipeline leaves to the caller (a
  // Mac archive), and nothing was done.
  handled: boolean;
  // Where the image went (persisted path, or what onSmall answered); null
  // when nothing was stored (the user has been told why).
  path: string | null;
  category?: DiskCategory;
}

// Names a disk image may carry that the stored .dmg replaces.
const RAW_EXT = /\.(img|image|dsk|hda|hfv|iso|toast|cdr|raw|bin|dmg|smi|udif)$/i;

// The name an image is stored under: its own, sanitised, as .dmg
// ("Disk Tools.img" -> "Disk_Tools.dmg").
export function storedDmgName(original: string): string {
  const base = (original.split('/').pop() || 'disk').replace(/\.(gz|zip)$/i, '');
  const stem = sanitizeName(base.replace(RAW_EXT, '')) || 'disk';
  return `${stem}.dmg`;
}

// Junk a bare zip carries besides its media (macOS resource sidecars,
// Finder state, directories).
function isZipJunk(name: string): boolean {
  const base = name.split('/').pop() ?? '';
  return (
    name.endsWith('/') ||
    name.startsWith('__MACOSX/') ||
    base.startsWith('._') ||
    base === '.DS_Store' ||
    base === ''
  );
}

function sizeText(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

class ImportCancelled extends Error {}

// --- Windows of a source ------------------------------------------------------

// A source read as transfer-window-sized pieces (the last one shorter).
interface WindowReader {
  next(): Promise<Uint8Array | null>;
  cancel(): void;
  read: number; // bytes taken so far
  total: number | null;
}

function windowsOf(src: ImportSource, size: number): WindowReader {
  if (src.kind === 'blob') {
    const blob = src.blob;
    const w: WindowReader = {
      read: 0,
      total: blob.size,
      async next() {
        if (w.read >= blob.size) return null;
        const end = Math.min(w.read + size, blob.size);
        const part = new Uint8Array(await blob.slice(w.read, end).arrayBuffer());
        w.read = end;
        return part;
      },
      cancel() {},
    };
    return w;
  }
  const reader = src.stream.getReader();
  let carry: Uint8Array | null = null;
  let ended = false;
  const w: WindowReader = {
    read: 0,
    total: src.total,
    async next() {
      if (ended && !carry) return null;
      const out = new Uint8Array(size);
      let fill = 0;
      while (fill < size) {
        if (!carry) {
          if (ended) break;
          const { done, value } = await reader.read();
          if (done) {
            ended = true;
            break;
          }
          carry = value && value.length ? value : null;
          continue;
        }
        const n = Math.min(size - fill, carry.length);
        out.set(carry.subarray(0, n), fill);
        fill += n;
        carry = n === carry.length ? null : carry.subarray(n);
      }
      w.read += fill;
      return fill ? out.subarray(0, fill) : null;
    },
    cancel() {
      reader.cancel().catch(() => undefined);
    },
  };
  return w;
}

// The first `n` bytes of a source without consuming it: a Blob is sliced, a
// stream is teed.
async function headOf(
  src: ImportSource,
  n: number,
): Promise<{ head: Uint8Array; src: ImportSource }> {
  if (src.kind === 'blob')
    return { head: new Uint8Array(await src.blob.slice(0, n).arrayBuffer()), src };
  // Read up to n bytes, then hand on a stream that replays them first.
  const reader = src.stream.getReader();
  const got: Uint8Array[] = [];
  let have = 0;
  let ended = false;
  while (have < n) {
    const { done, value } = await reader.read();
    if (done) {
      ended = true;
      break;
    }
    if (value?.length) {
      got.push(value);
      have += value.length;
    }
  }
  const head = new Uint8Array(have);
  let at = 0;
  for (const g of got) {
    head.set(g, at);
    at += g.length;
  }
  const replay = new ReadableStream<Uint8Array>({
    start(ctl) {
      if (head.length) ctl.enqueue(head);
      if (ended) ctl.close();
    },
    async pull(ctl) {
      const { done, value } = await reader.read();
      if (done) ctl.close();
      else if (value?.length) ctl.enqueue(value);
    },
    cancel(reason) {
      return reader.cancel(reason);
    },
  });
  return {
    head: head.subarray(0, Math.min(n, head.length)),
    src: { kind: 'stream', stream: replay, total: src.total },
  };
}

// --- Quota -------------------------------------------------------------------

let persistAsked = false;

// Refuse up front when even the compressed source will not fit, and ask once
// that the origin's storage not be evicted under pressure (nothing else in
// the app asks, and an evicted origin loses every image and checkpoint).
async function preflight(name: string, sourceBytes: number | null): Promise<boolean> {
  const storage = typeof navigator !== 'undefined' ? navigator.storage : undefined;
  if (!storage) return true;
  if (!persistAsked && storage.persist) {
    persistAsked = true;
    storage.persist().catch(() => undefined);
  }
  if (sourceBytes === null || !storage.estimate) return true;
  try {
    const est = await storage.estimate();
    const free = (est.quota ?? Infinity) - (est.usage ?? 0);
    // The stored image is at most about the decoded size, at least what the
    // source compresses to; the source's own size is the cheap bound.
    if (free < Math.min(sourceBytes, 64 * 1024 * 1024)) {
      showNotification(
        `Not enough browser storage for ${name}: ${sizeText(Math.max(0, free))} free`,
        'error',
      );
      return false;
    }
  } catch {
    /* no estimate: try anyway */
  }
  return true;
}

// --- The pipeline ----------------------------------------------------------------

// A unique path for `name` in `dir` (name, name_2, name_3, ...).
async function uniquePath(dir: string, name: string): Promise<string> {
  const dot = name.lastIndexOf('.');
  const stem = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot) : '';
  for (let i = 1; ; i++) {
    const p = `${dir}/${i === 1 ? name : `${stem}_${i}${ext}`}`;
    if ((await gsEval('files.path_exists', [p])) !== true) return p;
  }
}

async function rmQuiet(path: string): Promise<void> {
  try {
    await gsEval('files.rm', [path]);
  } catch {
    /* already gone */
  }
}

// Validate the UDIF at `part` as the first of `categories` it is, and move it
// into that category's directory.  Removes `part` either way.
async function placeUdif(
  part: string,
  name: string,
  categories: DiskCategory[],
): Promise<{ path: string; category: DiskCategory } | null> {
  for (const cat of categories) {
    const d = MEDIA_TYPES[cat];
    const v = await d.validate(part, gsEval);
    if (!v.valid) continue;
    await gsEval('files.mkdir', [d.persistDir]);
    const dest = await uniquePath(d.persistDir, storedDmgName(name));
    const moved = await gsEval('files.mv', [part, dest]);
    if (moved !== true) {
      showNotification(`Could not store ${name}: ${gsErrorText(moved)}`, 'error');
      await rmQuiet(part);
      return null;
    }
    bumpImagesRevision();
    return { path: dest, category: cat };
  }
  await rmQuiet(part);
  const what = categories.map((c) => MEDIA_TYPES[c].label).join(' or ');
  showNotification(`'${name}' is not a valid ${what}`, 'error');
  return null;
}

interface Pump {
  source: WindowReader;
  // Windows already read (a small-path probe's buffer), fed first.
  pending: Uint8Array[];
}

// Write every byte of `pump` into a new UDIF at `part`.  Throws on failure
// (the partial file removed).  The stats files.udif_finish answered.
async function writeUdif(
  part: string,
  name: string,
  pump: Pump,
  opts: ImportOptions,
  cancelled: () => boolean,
): Promise<{ bytes_in: number; stored_bytes: number }> {
  await rmQuiet(part);
  const h = await gsEval('files.udif_open', [part, 64, 1, name]);
  if (typeof h !== 'number' || h < 0) throw new Error(gsErrorText(h));
  let stored = 0;
  try {
    for (;;) {
      if (cancelled()) throw new ImportCancelled();
      const win = pump.pending.shift() ?? (await pump.source.next());
      if (!win) break;
      stored = await xferUdifAppend(h, win);
      opts.onProgress?.(pump.source.read, pump.source.total, stored);
    }
    const st = (await gsEval('files.udif_finish', [h])) as {
      bytes_in?: number;
      stored_bytes?: number;
    } | null;
    if (!st || typeof st !== 'object' || typeof st.stored_bytes !== 'number')
      throw new Error(gsErrorText(st));
    return { bytes_in: st.bytes_in ?? 0, stored_bytes: st.stored_bytes };
  } catch (e) {
    try {
      await gsEval('files.udif_abort', [h]);
    } catch {
      /* finish already released it */
    }
    await rmQuiet(part);
    throw e;
  }
}

// Copy `pump` byte-for-byte to `path` (a .dmg that is already compact).
async function writeExact(path: string, pump: Pump, opts: ImportOptions, cancelled: () => boolean) {
  const all = new ReadableStream<Uint8Array>({
    async pull(ctl) {
      if (cancelled()) {
        ctl.error(new ImportCancelled());
        return;
      }
      const win = pump.pending.shift() ?? (await pump.source.next());
      if (!win) ctl.close();
      else {
        ctl.enqueue(win);
        opts.onProgress?.(pump.source.read, pump.source.total, pump.source.read);
      }
    },
  });
  if (!(await streamToOpfs(path, all))) throw new Error('could not store the image');
}

// A UDIF staged at `part` (stored as uploaded): keep it when the emulator
// reads it in place, else re-chunk it.  The path of the UDIF to place.
async function settleUdif(part: string, name: string): Promise<string> {
  const info = (await gsEval('files.udif_info', [part])) as { in_place?: boolean } | null;
  if (info && typeof info === 'object' && info.in_place) return part;
  const rechunked = `${part}.re`;
  await rmQuiet(rechunked);
  showNotification(`Re-chunking ${name} so it can be read in place...`, 'info');
  const r = await gsEval('files.convert', [part, rechunked]);
  await rmQuiet(part);
  if (!r || typeof r !== 'object' || 'error' in (r as object)) {
    await rmQuiet(rechunked);
    throw new Error(`${name}: ${gsErrorText(r)}`);
  }
  return rechunked;
}

// Import one decoded stream (no container left) as a disk image.
async function importDecoded(
  src: ImportSource,
  name: string,
  opts: ImportOptions,
  cancelled: () => boolean,
  onReader: (w: WindowReader) => void,
): Promise<ImportOutcome> {
  const window = await xferChunkBytes();
  const reader = windowsOf(src, window);
  onReader(reader);
  const pump: Pump = { source: reader, pending: [] };
  const base = sanitizeName(name.split('/').pop() || 'disk') || 'disk';

  // Small: everything fits under the threshold.  A known length decides at
  // once; an unknown one is gathered until it is past it.
  const known = reader.total;
  if (opts.onSmall && (known === null || known <= LARGE_IMPORT_BYTES)) {
    let gathered = 0;
    while (gathered <= LARGE_IMPORT_BYTES) {
      const win = await reader.next();
      if (!win) break;
      pump.pending.push(win);
      gathered += win.length;
    }
    if (gathered <= LARGE_IMPORT_BYTES) {
      const staged = `${UPLOAD_DIR}/${base}`;
      const bytes = new Uint8Array(gathered);
      let at = 0;
      for (const p of pump.pending) {
        bytes.set(p, at);
        at += p.length;
      }
      if (!(await streamToOpfs(staged, bytes))) throw new Error('could not stage the file');
      return { handled: true, path: await opts.onSmall(staged, name) };
    }
  }

  // A UDIF is stored as it is (or re-chunked): never compressed twice.  A
  // Blob's trailer is read directly; a stream's is known by its name.
  let isUdif = /\.(dmg|smi|udif)$/i.test(name);
  if (src.kind === 'blob' && src.blob.size >= 512) {
    const tail = new Uint8Array(await src.blob.slice(src.blob.size - 512).arrayBuffer());
    isUdif = String.fromCharCode(...tail.subarray(0, 4)) === 'koly';
  }
  const part = `${UPLOAD_DIR}/${base}.dmg.part`;
  if (isUdif) {
    await rmQuiet(part);
    await writeExact(part, pump, opts, cancelled);
    const tail = await xferRead(part, Math.max(0, reader.read - 512), 512);
    if (String.fromCharCode(...tail.subarray(0, 4)) !== 'koly') {
      // Not a UDIF after all: it is a raw image, now staged whole.  Store
      // it compressed from the staged copy.
      const dmg = `${part}.re`;
      await rmQuiet(dmg);
      const r = await gsEval('files.convert', [part, dmg]);
      await rmQuiet(part);
      if (!r || typeof r !== 'object' || 'error' in (r as object)) throw new Error(gsErrorText(r));
      const placed = await placeUdif(dmg, name, opts.categories);
      if (placed) showNotification(`${name} stored`, 'info');
      return { handled: true, path: placed?.path ?? null, category: placed?.category };
    }
    const settled = await settleUdif(part, name);
    const placed = await placeUdif(settled, name, opts.categories);
    if (placed) showNotification(`${name} stored`, 'info');
    return { handled: true, path: placed?.path ?? null, category: placed?.category };
  }

  const st = await writeUdif(part, name, pump, opts, cancelled);
  const placed = await placeUdif(part, name, opts.categories);
  if (placed)
    showNotification(
      `${name}: ${sizeText(st.bytes_in)} disk stored in ${sizeText(st.stored_bytes)}`,
      'info',
    );
  return { handled: true, path: placed?.path ?? null, category: placed?.category };
}

// A Mac archive (StuffIt, Compact Pro, BinHex, MacBinary): staged as it is
// -- the compressed side -- then one member decoded by the core straight
// into a UDIF (files.archive.import), nothing extracted.  When that member is
// not a disk the archive is left to the caller's extracting flow.
async function importMacArchive(
  src: ImportSource,
  name: string,
  opts: ImportOptions,
): Promise<ImportOutcome> {
  const base = sanitizeName(name.split('/').pop() || 'archive') || 'archive';
  const staged = `${UPLOAD_DIR}/${base}`;
  const body = src.kind === 'blob' ? src.blob : src.stream;
  if (
    !(await streamToOpfs(staged, body, (n) =>
      opts.onProgress?.(n, src.kind === 'blob' ? src.blob.size : src.total, 0),
    ))
  )
    throw new Error('could not stage the archive');
  const part = `${UPLOAD_DIR}/${base}.dmg.part`;
  await rmQuiet(part);
  try {
    setActivityDetail('decoding the archive...');
    const r = (await gsEval('files.archive.import', [staged, part, opts.member ?? ''])) as {
      member?: string;
      bytes_in?: number;
      stored_bytes?: number;
    } | null;
    if (!r || typeof r !== 'object' || typeof r.member !== 'string') {
      await rmQuiet(part);
      return { handled: false, path: null };
    }
    const member = r.member.split('/').pop() || name;
    for (const cat of opts.categories) {
      if ((await MEDIA_TYPES[cat].validate(part, gsEval)).valid) {
        const placed = await placeUdif(part, member, [cat]);
        if (placed && r.bytes_in && r.stored_bytes)
          showNotification(
            `${member}: ${sizeText(r.bytes_in)} disk stored in ${sizeText(r.stored_bytes)}`,
            'info',
          );
        return { handled: true, path: placed?.path ?? null, category: placed?.category };
      }
    }
    // Not a disk (a floppy, a ROM, an application): the staged flow.
    await rmQuiet(part);
    return { handled: false, path: null };
  } finally {
    await rmQuiet(staged);
  }
}

// Import `src` (named `name`) as a disk image, unpacking a zip or gzip
// container on the way.  See the header.  Never throws: a failure is a toast
// and { handled: true, path: null }.
export async function importImage(
  src: ImportSource,
  name: string,
  opts: ImportOptions,
): Promise<ImportOutcome> {
  let cancel = false;
  const live: { reader: WindowReader | null } = { reader: null };
  let zipCancel: (() => void) | null = null;
  const cancelled = () => cancel;
  setActivityCancel(() => {
    cancel = true;
    live.reader?.cancel();
    zipCancel?.();
  });
  const total = src.kind === 'blob' ? src.blob.size : src.total;
  const report = opts.onProgress;
  opts = {
    ...opts,
    onProgress(read, t, stored) {
      setActivityDetail(
        `${sizeText(read)}${t ? ` of ${sizeText(t)}` : ''} read, ${sizeText(stored)} stored`,
      );
      report?.(read, t, stored);
    },
  };
  try {
    if (!(await preflight(name, total))) return { handled: true, path: null };
    const peeked = await headOf(src, 512);
    src = peeked.src;
    const kind = sniffContainer(peeked.head);
    if (kind === 'mac') return await importMacArchive(src, name, opts);
    if (kind === 'gzip') {
      const stream = (src.kind === 'blob' ? src.blob.stream() : src.stream).pipeThrough(
        decompressor('gzip'),
      );
      const inner = name.replace(/\.gz$/i, '');
      return await importDecoded(
        { kind: 'stream', stream, total: null },
        inner,
        opts,
        cancelled,
        (w) => (live.reader = w),
      );
    }
    if (kind === 'zip') {
      const body = src.kind === 'blob' ? src.blob.stream() : src.stream;
      let zipRead = 0;
      const entries = zipEntries(body, (n) => (zipRead = n));
      zipCancel = () => entries.return(undefined).catch(() => undefined);
      const member = opts.member ?? null;
      const tried: string[] = [];
      for await (const e of entries) {
        if (cancel) throw new ImportCancelled();
        if (member !== null) {
          if (e.isDir || !findMember([e.name], member)) {
            tried.push(e.name);
            continue;
          }
        } else if (isZipJunk(e.name)) continue;
        if (!e.supported) {
          if (member !== null)
            throw new ZipStreamError(`${e.name}: compression method ${e.method} is not supported`);
          continue;
        }
        const memberOpts: ImportOptions = {
          ...opts,
          onProgress: (_r, _t, stored) => opts.onProgress?.(zipRead, total, stored),
        };
        const out = await importDecoded(
          { kind: 'stream', stream: e.open(), total: e.size },
          e.name.split('/').pop() || e.name,
          memberOpts,
          cancelled,
          (w) => (live.reader = w),
        );
        // A named member is the answer whatever it came to; for a bare
        // archive, the first member that stored is (one that did not was
        // removed, and the next is tried from the same stream).
        if (member !== null || out.path) return out;
      }
      if (member !== null) {
        const shown = tried.slice(0, 5).join(', ');
        showNotification(
          `"${member}" is not in ${name}${tried.length ? ` (it has ${shown}${tried.length > 5 ? ', ...' : ''})` : ''}`,
          'error',
        );
      } else showNotification(`No mountable media inside ${name}`, 'warning');
      return { handled: true, path: null };
    }
    return await importDecoded(src, name, opts, cancelled, (w) => (live.reader = w));
  } catch (e) {
    live.reader?.cancel();
    if (e instanceof ImportCancelled || cancel) {
      showNotification(`Import of ${name} cancelled`, 'info');
    } else {
      const msg = e instanceof Error ? e.message : String(e);
      showNotification(`Could not import ${name}: ${msg}`, 'error');
      console.error('[importImage]', e);
    }
    return { handled: true, path: null };
  } finally {
    setActivityCancel(null);
    setActivityDetail('');
  }
}
