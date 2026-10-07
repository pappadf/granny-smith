// Upload pipeline — port of app/web/js/drop.js + upload-pipeline.js
// + media.js. Single entry point: `acceptFiles(files)` (called from the
// drag-and-drop overlay, the Upload-ROM button, and the URL-media path).
//
// Flow:
//   1. A checkpoint dropped on its own short-circuits to load; one in a drop
//      with other files is refused.
//   2. Every other file runs the single-file flow on its own (acceptOne):
//      stage it into the scratch area, /opfs/upload/.scratch/ (see
//      stageUpload), under a name no other upload uses; probe it (ROM?
//      floppy? archive? something else?); move it into the right
//      /opfs/images/<category>/ or refuse it; discard the staging copy,
//      whatever came of it.
//   3. Then, as a single drop would: boot a default machine from a ROM that
//      is the drop's only ROM, and put the first file of each category into
//      an empty drive.  A drop of several files ends with one summary.
//
// STAGING (see stageUpload): the write goes through the emulator's own
// filesystem, on the emulator thread: the page copies each chunk into the
// core's transfer window and files.xfer_write puts it in the file
// (bus/xfer.ts).  The page never touches the filesystem itself.  It used to
// call Module.FS, which under WasmFS runs on the page's thread and busy-waits
// for WasmFS's OPFS thread -- jank on Chrome, and a deadlock on Safari, where
// WebKit serves that thread's OPFS requests through the busy page thread.
// Writing staging with the page's own navigator.storage failed too: Safari's
// OPFS rejects main-thread createWritable() with "UnknownError", and on
// Chromium the emulator's WasmFS can't see an out-of-band OPFS write, so the
// follow-up files.cp strands the file in /opfs/upload.  The file is sliced
// and written chunk by chunk, so uploads of any size (including hundreds-of-MB
// CD-ROMs) never buffer the whole file.  This mirrors how move/delete route
// through the worker (files.mv/files.rm).

import { gsEval, gsErrorText, isModuleReady } from './emulator';
import { xferChunkBytes, xferWrite } from './xfer';
import { reconcileUiWithMachine, prepareFreshMachine, recordRecentBoot } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { setMounted, bumpImagesRevision } from '@/state/images.svelte';
import { startActivity, endActivity } from '@/state/activity.svelte';
import { sanitizeName } from '@/lib/archive';
import { fileHasCheckpointSignature, scratchPath, ROMS_DIR, HD_DIR, CD_DIR } from '@/lib/opfsPaths';
import {
  MEDIA_TYPES,
  LARGE_IMPORT_BYTES,
  identifyRom,
  type MediaTypeId,
  type MediaTypeDescriptor,
} from '@/lib/media';
import { mountImage, insertFloppy } from './media';
import { opfsSafeName } from './fsOps';
import { importImage, type DiskCategory } from './importImage';

// The one chunked writer: everything the page puts on the emulator's
// filesystem — an upload's File, a URL download's response body, a dropped
// checkpoint — goes through here, to an OPFS path, a transfer window at a time
// (bus/xfer.ts; see STAGING above).  A stream's small network chunks are
// gathered into full windows first, so a download costs one request per
// window, not per packet.  Nothing buffers the whole file, so any size works.
// `onProgress` hears the bytes taken from the source so far: per network
// chunk for a stream (a download's progress bar), per window otherwise.
// Returns true on success.
export async function streamToOpfs(
  opfsPath: string,
  source: Blob | ReadableStream<Uint8Array> | Uint8Array,
  onProgress?: (bytesWritten: number) => void,
): Promise<boolean> {
  try {
    const size = await xferChunkBytes();
    let pos = 0;
    let wrote = false;
    const put = async (chunk: Uint8Array) => {
      await xferWrite(opfsPath, pos, chunk);
      pos += chunk.length;
      wrote = true;
    };
    if (source instanceof Uint8Array) {
      for (let at = 0; at < source.length; at += size) {
        await put(source.subarray(at, Math.min(at + size, source.length)));
        onProgress?.(pos);
      }
    } else if (source instanceof Blob) {
      for (let at = 0; at < source.size; at += size) {
        await put(
          new Uint8Array(await source.slice(at, Math.min(at + size, source.size)).arrayBuffer()),
        );
        onProgress?.(pos);
      }
    } else {
      const reader = source.getReader();
      const pending = new Uint8Array(size);
      let fill = 0;
      let seen = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        seen += value?.length ?? 0;
        onProgress?.(seen);
        for (let at = 0; value && at < value.length;) {
          const n = Math.min(size - fill, value.length - at);
          pending.set(value.subarray(at, at + n), fill);
          fill += n;
          at += n;
          if (fill === size) {
            await put(pending);
            fill = 0;
          }
        }
      }
      if (fill) await put(pending.subarray(0, fill));
    }
    if (!wrote) await put(new Uint8Array(0)); // an empty file still exists
    return true;
  } catch (err) {
    console.error('upload: staging write failed', err);
    return false;
  }
}

// Stage an uploaded file in the scratch area for probing/persisting and return
// the staged path (or null on failure, with nothing left behind).  Callers
// discard it via discardStaging() on every exit.
async function stageUpload(file: File): Promise<string | null> {
  const path = scratchPath(sanitizeName(file.name) || 'image.img');
  if (await streamToOpfs(path, file)) return path;
  await discardStaging(path); // a partial write
  return null;
}

// Best-effort removal of a staged file or directory. Everything staging touches is written
// by the worker (FS streaming above), so the rm runs worker-side too —
// keeping its WasmFS view coherent.
export async function discardStaging(path: string): Promise<void> {
  await gsEval('files.rm', [path]);
}

// Top-level entry. Caller supplies a flat list of File objects.
export interface AcceptFilesOptions {
  // When true (default) and a freshly-uploaded file validates as a ROM,
  // boot a default machine straight away. The Display drag-and-drop
  // path keeps this on so dropping a ROM lands the user in a running
  // emulator. The Welcome "Upload ROM..." button passes false: the user
  // is on the Welcome screen specifically to configure a machine, and
  // auto-booting strands them mid-setup (no VROM, no disks).
  autoBootOnRom?: boolean;
}

// What one uploaded file came to: stored as `category` at `path`, or not
// stored -- `reason` completes "'<name>' …", and `told` is set when the user
// has already been told why (a streamed import, a failed store say so).
export type FileOutcome =
  | { stored: true; category: MediaTypeId; path: string }
  | { stored: false; reason: string; severity: 'error' | 'warning'; told?: boolean };

// How a drop of several files names what it stored, by category.
const STORED_NOUN: Record<MediaTypeId, [string, string]> = {
  rom: ['ROM', 'ROMs'],
  vrom: ['video ROM', 'video ROMs'],
  prom: ['PCI expansion ROM', 'PCI expansion ROMs'],
  fd: ['floppy', 'floppies'],
  hd: ['hard disk', 'hard disks'],
  cdrom: ['CD-ROM', 'CD-ROMs'],
};

// The one message a drop of several files ends with: how many were stored,
// as what, and each one that was not with its reason -- e.g. "3 stored
// (2 floppies, 1 ROM), 1 rejected: 'notes.txt' doesn't look like …".
export function dropSummary(
  names: string[],
  outcomes: FileOutcome[],
): { msg: string; severity: 'info' | 'warning' | 'error' } {
  const counts = new Map<MediaTypeId, number>();
  const rejected: string[] = [];
  outcomes.forEach((o, i) => {
    if (o.stored) counts.set(o.category, (counts.get(o.category) ?? 0) + 1);
    else rejected.push(`'${names[i]}' ${o.reason}`);
  });
  const stored = outcomes.length - rejected.length;
  const kinds = [...counts].map(([c, n]) => `${n} ${STORED_NOUN[c][n === 1 ? 0 : 1]}`);
  let msg = `${stored} stored${kinds.length ? ` (${kinds.join(', ')})` : ''}`;
  if (rejected.length) msg += `, ${rejected.length} rejected: ${rejected.join('; ')}`;
  const severity = !rejected.length ? 'info' : stored ? 'warning' : 'error';
  return { msg, severity };
}

// The Display drop and the generic Upload button.  Every file runs the
// single-file flow on its own, and a drop of several ends with one summary
// of what each came to.  A checkpoint is loaded when it is dropped alone,
// and refused in a drop with other files: loading it would replace the
// machine they were meant for.
export async function acceptFiles(files: File[], opts: AcceptFilesOptions = {}): Promise<void> {
  if (!files.length) return;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return;
  }
  const single = files.length === 1;
  if (single && (await fileHasCheckpointSignature(files[0]))) {
    await loadCheckpointFile(files[0]);
    return;
  }

  const outcomes: FileOutcome[] = [];
  for (const file of files) {
    startActivity(file.name);
    try {
      outcomes.push(
        (await fileHasCheckpointSignature(file))
          ? {
              stored: false,
              reason: 'is a checkpoint: drop a checkpoint on its own',
              severity: 'error',
            }
          : await acceptOne(file, !single),
      );
    } finally {
      endActivity();
    }
  }

  if (!single) {
    const { msg, severity } = dropSummary(
      files.map((f) => f.name),
      outcomes,
    );
    showNotification(msg, severity);
  } else if (!outcomes[0].stored && !outcomes[0].told) {
    showNotification(`'${files[0].name}' ${outcomes[0].reason}`, outcomes[0].severity);
  }
  await actOnStored(
    outcomes.flatMap((o) => (o.stored ? [o] : [])),
    opts.autoBootOnRom ?? true,
  );
}

// One file of an upload through the single-file flow: a large one streamed
// into a compact disk image, any other staged, probed, stored or refused,
// and discarded.  `quiet` holds the per-file "uploaded" messages back for a
// drop that ends with a summary.
async function acceptOne(file: File, quiet: boolean): Promise<FileOutcome> {
  // A large file is a disk image (or an archive holding one): streamed into
  // a compact UDIF, never staged expanded.  A Mac archive is not unpacked
  // there; it falls through to the staged flow below.
  if (file.size > LARGE_IMPORT_BYTES) {
    let small = null as FileOutcome | null;
    const out = await importImage({ kind: 'blob', blob: file }, file.name, {
      categories: ['cdrom', 'hd'],
      onSmall: async (path, name) => {
        small = await probeStaged(path, name, quiet);
        return small.stored ? small.path : null;
      },
    });
    if (out.handled) {
      if (small) return small;
      if (out.path && out.category) return { stored: true, category: out.category, path: out.path };
      // The import has said why.
      return { stored: false, reason: 'was not stored', severity: 'error', told: true };
    }
  }
  const staging = await stageUpload(file);
  if (!staging) return { stored: false, reason: 'could not be loaded', severity: 'error' };
  try {
    return await probeStaged(staging, file.name, quiet);
  } finally {
    await discardStaging(staging);
  }
}

// What a drop does with what it stored, exactly as single drops would: a ROM
// boots a default machine when it is the drop's only ROM, and the first file
// of each other category goes into an empty drive.  Nothing more is mounted.
async function actOnStored(
  stored: Array<{ category: MediaTypeId; path: string }>,
  autoBootOnRom: boolean,
): Promise<void> {
  const roms = stored.filter((s) => s.category === 'rom');
  if (autoBootOnRom && roms.length === 1) await maybeBootFromRom(roms[0].path);
  else if (autoBootOnRom && roms.length > 1)
    showNotification(`Not booting: the drop holds ${roms.length} ROMs`, 'info');
  const mounted = new Set<MediaTypeId>();
  for (const s of stored) {
    if (s.category === 'rom' || mounted.has(s.category)) continue;
    mounted.add(s.category);
    await autoMountIfEmpty(s.path, s.category);
  }
}

// Category-strict entry — used by the New Machine dialog dropdowns and
// the Images-tab per-category drop targets. Validates the staged file
// AS THIS SPECIFIC category only; rejects (with a toast) anything that
// doesn't match. Single-file flow.  Returns the persisted path, or null
// when nothing was stored.
export async function acceptFilesAsCategory(
  files: File[],
  category: MediaTypeId,
): Promise<string | null> {
  if (!files.length) return null;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return null;
  }
  const file = files[0];
  startActivity(file.name);
  try {
    if ((category === 'hd' || category === 'cdrom') && file.size > LARGE_IMPORT_BYTES) {
      const out = await importDiskFile(file, category);
      if (out !== undefined) return out;
    }
    const staging = await stageUpload(file);
    if (!staging) {
      showNotification(`Could not load ${file.name}`, 'error');
      return null;
    }
    let stored: PersistOutcome;
    try {
      stored = await persistAs(staging, file.name, category);
    } finally {
      await discardStaging(staging);
    }
    if (!stored.ok) {
      showNotification(`'${file.name}' ${stored.reason}`, 'error');
      return null;
    }
    if (category === 'rom') await maybeBootFromRom(stored.path);
    else await autoMountIfEmpty(stored.path, category);
    return stored.path;
  } finally {
    endActivity();
  }
}

// Raw entry — used by the Filesystem-tab external file drop. NO
// validation, NO category routing; just writes the file at the given
// OPFS directory. The Filesystem view exists precisely so the user can
// poke at OPFS freely, including putting random files anywhere.
export async function acceptFilesRaw(files: File[], targetDir: string): Promise<void> {
  if (!files.length) return;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return;
  }
  // The hard-disk and CD stores are the emulator's: what lands there is an
  // image it stores compactly, as an upload of that kind is.  Anywhere else
  // the user gets exactly the file they dropped.
  const dir = targetDir.replace(/\/+$/, '');
  const category: DiskCategory | null = dir === HD_DIR ? 'hd' : dir === CD_DIR ? 'cdrom' : null;
  for (const file of files) {
    if (category && file.size > LARGE_IMPORT_BYTES) {
      startActivity(file.name, 'Importing');
      try {
        const out = await importDiskFile(file, category);
        if (out !== undefined) continue;
      } finally {
        endActivity();
      }
    }
    const safe = sanitizeName(file.name) || 'file.bin';
    const finalPath = `${targetDir}/${safe}`;
    startActivity(file.name);
    try {
      // Write straight to the chosen folder on the worker (Safari-safe, coherent
      // with its WasmFS). No staging/copy — targetDir may itself be /opfs/upload.
      const ok = await streamToOpfs(finalPath, file);
      showNotification(
        ok ? `'${file.name}' saved to ${targetDir}` : `Could not load ${file.name}`,
        ok ? 'info' : 'error',
      );
    } finally {
      endActivity();
    }
  }
}

// A large file dropped or picked as `category`: streamed into a compact
// UDIF.  The persisted path, null when nothing was stored (the user has been
// told), or undefined when the file is a Mac archive the staged flow unpacks.
async function importDiskFile(
  file: File,
  category: DiskCategory,
): Promise<string | null | undefined> {
  const out = await importImage({ kind: 'blob', blob: file }, file.name, {
    categories: [category],
  });
  if (!out.handled) return undefined;
  if (out.path) await autoMountIfEmpty(out.path, category);
  return out.path;
}

// After a floppy / CD image lands in /opfs/images/{fd,cd}/, try to
// auto-insert it into an empty drive. Mirrors the physical metaphor of
// dropping a disk into a Mac — if a slot is free, the disk goes in.
// ROM uses maybeBootFromRom instead (full cold boot); HD / VROM stay
// on disk until explicitly mounted from the Images tab. Toast on the
// outcome either way.
async function autoMountIfEmpty(persistedPath: string, category: MediaTypeId): Promise<void> {
  if (category === 'fd') {
    // The first empty drive of the ones the machine has (bus/media.ts).
    const r = await insertFloppy(persistedPath, true);
    if (r.ok) {
      setMounted(persistedPath, r.mount);
      showNotification(`Inserted into floppy drive ${r.mount.drive + 1}`, 'info');
    } else {
      showNotification(`Image saved but not inserted: ${r.reason}`, 'warning');
    }
    return;
  }
  if (category === 'cdrom') {
    // Into the running machine's first empty CD-ROM drive; refused on a
    // machine with none or with every drive full.
    const r = await mountImage('cd', persistedPath);
    if (r.ok) {
      setMounted(persistedPath, r.mount);
      showNotification('Inserted into CD-ROM drive', 'info');
    } else {
      showNotification(`Image saved but not mounted: ${r.reason}`, 'warning');
    }
  }
}

// Pull the active DataTransfer and feed it into acceptFiles. Used by
// DropOverlay's drop handler.
export async function processDataTransfer(dt: DataTransfer): Promise<void> {
  const files: File[] = [];
  if (dt.items?.length) {
    for (const item of dt.items) {
      if (item.kind !== 'file') continue;
      const f = item.getAsFile();
      if (f) files.push(f);
    }
  } else if (dt.files?.length) {
    for (const f of dt.files) files.push(f);
  }
  await acceptFiles(files);
}

// Probe order.  The strict signature/size matchers come first; `hd` is the
// permissive fallback (it accepts anything that opens and isn't floppy-sized,
// so it would happily classify a 32 KB VROM — or a .zip — as a tiny "hard
// disk"), so it runs last, after archives have had their turn.
//   rom    — machine.rom.identify: content id against the core's ROM table
//   vrom   — catalog.vroms.identify: Format-Block CRC against the catalog
//   prom   — $55AA + a reachable PCIR + Open Firmware code type
//   fd     — exact floppy sizes (400/800/1440 KB ± DC42 header)
//   cdrom  — ISO 9660 / HFS / APM signature inside the file
//   hd     — permissive fallback
//
// vrom and prom cannot claim each other's files even though both are
// commonly 32 KB: a vROM is identified by a NuBus Format-Block CRC in
// its trailing bytes, a PROM by a PCI Data Structure near its head, so
// whichever runs first the other still fails. They sit adjacent anyway.
const STRICT_ORDER: MediaTypeId[] = ['rom', 'vrom', 'prom', 'fd', 'cdrom'];
const ALL_ORDER: MediaTypeId[] = [...STRICT_ORDER, 'hd'];

// Try `order`'s media types on the file at `path` (named `name` for the
// store and the messages) and persist it as the first that validates.
// Null when none of them matched.
async function probeAs(
  path: string,
  name: string,
  order: MediaTypeId[],
  opts: { quiet: boolean; inArchive?: boolean },
): Promise<FileOutcome | null> {
  for (const id of order) {
    const descriptor = MEDIA_TYPES[id];
    const result = await descriptor.validate(path, gsEval);
    // This IS that kind of file, refused: say why and stop, rather than
    // letting the permissive hd probe store it as a disk image.
    if (result.reject) return { stored: false, reason: result.reject, severity: 'error' };
    if (!result.valid) continue;
    const persisted = await persist(path, name, descriptor, result.info, opts);
    if (persisted) return { stored: true, category: id, path: persisted };
    // persist has said so, unless quiet.
    return { stored: false, reason: 'could not be stored', severity: 'error', told: !opts.quiet };
  }
  return null;
}

// Probe a staged upload to figure out what kind of media it is, and store
// it as that.  The staged file is the caller's to discard.
async function probeStaged(path: string, name: string, quiet: boolean): Promise<FileOutcome> {
  // The strict types first: a ROM dump named .bin is a ROM, not MacBinary.
  const strict = await probeAs(path, name, STRICT_ORDER, { quiet });
  if (strict) return strict;

  // Then archives, before the permissive hd probe can claim them.  Whether a
  // file is one -- zip, StuffIt, Compact Pro, BinHex, MacBinary, gzip -- is
  // the core's format registry's call, from its content, not its name.  The
  // archive is a VFS namespace, so its members are probed where they are
  // and only the first medium that validates is copied out: nothing else is
  // unpacked.
  if (await stagedArchiveFormat(path))
    return (
      (await probeArchive(path, quiet)) ?? {
        stored: false,
        reason: 'has no mountable media inside',
        severity: 'warning',
      }
    );

  return (
    (await probeAs(path, name, ['hd'], { quiet })) ?? {
      stored: false,
      reason: "doesn't look like a ROM, floppy, HD, CD, or archive",
      severity: 'warning',
    }
  );
}

// Probe the members of the archive staged at `stagingPath` through its VFS
// path: every file, in listing order, as any kind of medium, until one
// validates.  Dot files (AppleDouble "._" sidecars, which some zips carry)
// are never media.  Null when no member is a medium.
async function probeArchive(stagingPath: string, quiet: boolean): Promise<FileOutcome | null> {
  for (const inner of await listFiles(stagingPath)) {
    const base = inner.split('/').pop() ?? '';
    if (!base || base.startsWith('.')) continue;
    // Stored under its member name, made safe for OPFS (an HFS-derived
    // name can carry ':').
    const outcome = await probeAs(inner, opfsSafeName(base), ALL_ORDER, { quiet, inArchive: true });
    if (!outcome) continue;
    // A refusal names the member it is about.
    if (!outcome.stored) return { ...outcome, reason: `holds '${base}', which ${outcome.reason}` };
    return outcome;
  }
  return null;
}

// Where to store `name` in `dir`: its own name when nothing is there or the
// file there is byte-identical to `source`, else the first "name N.ext"
// free.
async function freeStorePath(dir: string, name: string, source: string): Promise<string> {
  const dot = name.lastIndexOf('.');
  const stem = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot) : '';
  for (let n = 1; ; n++) {
    const path = n === 1 ? `${dir}/${name}` : `${dir}/${stem} ${n}${ext}`;
    if ((await gsEval('files.path_exists', [path])) !== true) return path;
    if ((await gsEval('files.path_compare', [source, path])) === -1) return path;
  }
}

// Every file under `dir` (depth first, in listing order), as full paths.
async function listFiles(dir: string): Promise<string[]> {
  const entries = await gsEval('files.list', [dir]);
  if (!Array.isArray(entries)) {
    // An unreadable or oversized listing is said, not taken for an empty one.
    showNotification(`Cannot list ${dir}: ${gsErrorText(entries)}`, 'error');
    return [];
  }
  const out: string[] = [];
  for (const e of entries as { name: string; kind: string }[]) {
    const path = `${dir}/${e.name}`;
    if (e.kind === 'directory') out.push(...(await listFiles(path)));
    else out.push(path);
  }
  return out;
}

// The archive format of the file at `path` ("zip", "sit", "cpt", "hqx",
// "bin", "gz"), or '' when it is none -- decided by the core from the file's
// content (a bounded read of its head and tail), whatever it is called.
export async function stagedArchiveFormat(path: string): Promise<string> {
  try {
    const fmt = await gsEval('files.archive.identify', [path]);
    return typeof fmt === 'string' ? fmt : '';
  } catch {
    return '';
  }
}

async function persist(
  sourcePath: string,
  originalName: string,
  descriptor: MediaTypeDescriptor,
  info:
    { persistDir?: string; checksum?: string; cardId?: string; [k: string]: unknown } | undefined,
  // inArchive: the source is a member of a (read-only) archive: copy it
  // out, and leave the archive for the caller to discard.  quiet: no
  // per-file message (a drop of several files ends with a summary).
  opts: { inArchive?: boolean; quiet?: boolean } = {},
): Promise<string | null> {
  const { inArchive = false, quiet = false } = opts;
  const finalName = descriptor.nameFn ? descriptor.nameFn(originalName, info) : originalName;
  const targetDir = info?.persistDir ?? descriptor.persistDir;
  // files.cp does not create parent directories, and the category dirs are
  // made once at startup — so a store added after a user's OPFS was first
  // laid down has nowhere to copy to, and every upload of that kind fails
  // with a bare "Failed to save". Create it here; it is a no-op when the
  // directory already exists.
  //
  // Through files.mkdir, NOT the main-thread opfs.mkdirP: the copy below runs
  // on the worker, and a main-thread OPFS mutation is not reliably visible
  // to the worker's WasmFS (the same asymmetry that forces staging onto the
  // worker — see stageUpload). Creating it on one side and copying on the
  // other is exactly the bug this is fixing.
  await gsEval('files.mkdir', [targetDir]);
  // A file of that name already stored is replaced only when it is the same
  // file (a content-named ROM uploaded again): a different one keeps its
  // place and this one is stored beside it ("name 2.ext"), so two media of
  // one name -- two floppies of a URL, two uploads -- never overwrite each
  // other.
  const finalPath = await freeStorePath(targetDir, finalName, sourcePath);
  // Moved, not copied, so storing never holds the file twice.
  const exists = (await gsEval('files.path_exists', [finalPath])) === true;
  const verb = exists || inArchive ? 'files.cp' : 'files.mv';
  const ok = (await gsEval(verb, [sourcePath, finalPath])) === true;
  if (!ok) {
    if (!quiet) showNotification(`Failed to save ${originalName}`, 'error');
    return null;
  }
  if (!inArchive) await discardStaging(sourcePath);
  // Offer a freshly stored vROM to the core's content-addressed registry.
  // The startup enumeration ran once at page load, so without this an
  // "(auto)" boot after a mid-session upload would not see the file until
  // the next reload.
  if (descriptor.id === 'vrom') {
    await gsEval('catalog.vroms.offer', [finalPath]);
  }
  // Same for a PCI expansion ROM, through its own registry.
  if (descriptor.id === 'prom') {
    await gsEval('catalog.proms.offer', [finalPath]);
  }
  // Notify inventory watchers (e.g. WelcomeConfigSlide's dropdown
  // refresh effect) that the OPFS image catalog has changed.
  bumpImagesRevision();
  // When the file was recognised as a vROM, surface the card it provides in
  // the toast — it tells the user we actually identified the file, not just
  // "it landed in OPFS". The card's human display name is shown later in the
  // config dialog (sourced from catalog.profile), so we don't duplicate that
  // knowledge here; the id is the identification signal.
  const cardId = info?.cardId as string | undefined;
  const cardFor = descriptor.id === 'prom' ? 'PCI expansion ROM' : 'Video ROM';
  const shown = cardId ? `${originalName} (${cardFor} for '${cardId}')` : originalName;
  if (!quiet) showNotification(`${shown} added`, 'info');
  return finalPath;
}

// What storing a file as one category came to: where it was stored, or why
// it was not (completing "'<name>' …" in a message).
export type PersistOutcome = { ok: true; path: string } | { ok: false; reason: string };

// Persist a file that is already on the worker's filesystem (in the scratch
// area) as `category`: validate it as that category, then move it into
// /opfs/images/<category>/ exactly as an upload of that kind is stored.  A
// file that is not valid as `category` is refused with the validator's
// reason, and is the caller's to discard, as one that could not be moved is.
// Category-strict uploads and URL media both store through here, so a
// download is accepted or rejected exactly as the same file dropped on its
// category is.
export async function persistAs(
  sourcePath: string,
  originalName: string,
  category: MediaTypeId,
): Promise<PersistOutcome> {
  const descriptor = MEDIA_TYPES[category];
  const result = await descriptor.validate(sourcePath, gsEval);
  // A refusal says why; anything else is simply not this kind of file.
  if (!result.valid)
    return { ok: false, reason: result.reject ?? `is not a valid ${descriptor.label}` };
  const path = await persist(sourcePath, originalName, descriptor, result.info);
  return path ? { ok: true, path } : { ok: false, reason: 'could not be stored' };
}

// If the user dropped a ROM and no machine is running yet, boot a default
// configuration so they go straight to a usable Mac without going through
// the Configuration slide. Picks the first compatible model from the ROM
// info; the user can switch later.
async function maybeBootFromRom(romPath: string): Promise<void> {
  if (machine.status === 'running' || machine.status === 'paused') return;
  const info = await identifyRom(gsEval, romPath);
  if (!info || !info.compatible.length) return;
  const model = info.compatible[0];
  // One boot document — the core validates, installs the ROM itself and
  // boots the model's own default RAM.  A rejected document leaves the
  // previous machine (or none) in place, so stop here rather than configure
  // and "boot" it.
  const booted = await gsEval('machine.boot', { model, rom: romPath });
  if (booted !== true) {
    showNotification(`Could not boot ${model}: ${gsErrorText(booted)}`, 'error');
    return;
  }
  await reconcileUiWithMachine('boot');
  await recordRecentBoot({ model, rom: romPath });
  await prepareFreshMachine();
  showNotification(`Booted ${model} from the loaded ROM`, 'info');
}

async function loadCheckpointFile(file: File): Promise<void> {
  // Staged like any upload, a chunk at a time, in the scratch area, and
  // deleted once loaded.  It used to be read whole into memory and written
  // to the memory-backed /tmp, where it stayed for the session.
  const staged = scratchPath(sanitizeName(file.name) || 'checkpoint');
  let ok: boolean;
  try {
    if (!(await streamToOpfs(staged, file))) {
      showNotification('Emulator not ready for checkpoint load', 'warning');
      return;
    }
    showNotification(`Loading checkpoint ${file.name}…`, 'info');
    ok = (await gsEval('checkpoint.load', [staged])) === true;
  } finally {
    await discardStaging(staged);
  }
  if (ok) {
    await reconcileUiWithMachine('restore');
    showNotification(`Checkpoint loaded (${file.name})`, 'info');
  } else {
    showNotification(`Checkpoint load failed`, 'error');
  }
}

// Welcome's "Open Checkpoint...": pick a saved state from disk and load it
// through the path a dropped one takes.  The file's own signature decides,
// not its name -- Save State downloads a .bin, the store keeps
// state.checkpoint -- so the picker is not filtered by extension.
export async function pickAndLoadCheckpoint(): Promise<void> {
  const [file] = await openFilePicker('', false);
  if (!file) return;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return;
  }
  if (!(await fileHasCheckpointSignature(file))) {
    showNotification(`'${file.name}' is not a Granny Smith checkpoint`, 'error');
    return;
  }
  startActivity(file.name);
  try {
    await loadCheckpointFile(file);
  } finally {
    endActivity();
  }
}

// Programmatic file-picker entry — Welcome's "Upload ROM..." button calls
// this. Wraps an invisible `<input type="file">` click.  Resolves with the
// chosen files, or [] when the user cancels the dialog: `cancel` fires
// instead of `change` then (every browser that can run the emulator has it),
// and without it the promise stayed pending and the input leaked.
// There is deliberately no focus-based fallback: a window refocus can land
// before `change` and would drop a real selection.
export function openFilePicker(accept = '', multiple = true): Promise<File[]> {
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    if (accept) input.accept = accept;
    input.multiple = multiple;
    input.style.display = 'none';
    let done = false;
    const cleanup = (files: File[]) => {
      if (done) return;
      done = true;
      input.removeEventListener('change', onChange);
      input.removeEventListener('cancel', onCancel);
      input.remove();
      resolve(files);
    };
    const onChange = () => cleanup(input.files ? Array.from(input.files) : []);
    const onCancel = () => cleanup([]);
    input.addEventListener('change', onChange);
    input.addEventListener('cancel', onCancel);
    document.body.appendChild(input);
    input.click();
  });
}

// Used by Welcome's "Upload ROM..." button — opens the picker and runs the
// full pipeline (which bumps the image revision the dialogs re-scan on).
//
// The Welcome button passes { autoBootOnRom: false }: that surface is a
// configuration entry point, not a "boot now" shortcut. The Display
// drag-and-drop path leaves the default on so dropping a ROM there
// still cold-boots straight away.
export async function pickAndUpload(accept = '', opts: AcceptFilesOptions = {}): Promise<void> {
  const files = await openFilePicker(accept);
  if (files.length) await acceptFiles(files, opts);
}

// Category-strict picker variant — the New Machine dialog uses this so
// picking "Upload image..." in (say) the floppy slot only accepts a
// floppy. One file.  Returns the path of the persisted file (when the
// upload succeeds), or null when the user cancelled or the file was
// rejected.
export async function pickAndUploadAs(category: MediaTypeId, accept = ''): Promise<string | null> {
  const files = await openFilePicker(accept, false);
  if (!files.length) return null;
  return acceptFilesAsCategory(files, category);
}

export { ROMS_DIR };
