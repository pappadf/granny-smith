// Upload pipeline — port of app/web/js/drop.js + upload-pipeline.js
// + media.js. Single entry point: `acceptFiles(files)` (called from the
// drag-and-drop overlay, the Upload-ROM button, and the URL-media path).
//
// Flow:
//   1. Check first file for checkpoint signature — short-circuit to load.
//   2. Stage each file into /opfs/upload/ (see stageUpload).
//   3. Probe the staged file (ROM? floppy? archive? something else?).
//   4. If a ROM was uploaded, also boot a default machine from it.
//   5. Persist to the right /opfs/images/<category>/ via gsEval('files.cp').
//   6. Cleanup the staging copy.
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
import { reconcileUiWithMachine, prepareFreshMachine } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { setMounted, bumpImagesRevision } from '@/state/images.svelte';
import { startActivity, endActivity } from '@/state/activity.svelte';
import { sanitizeName } from '@/lib/archive';
import { fileHasCheckpointSignature, ROMS_DIR, UPLOAD_DIR, HD_DIR, CD_DIR } from '@/lib/opfsPaths';
import {
  MEDIA_TYPES,
  LARGE_IMPORT_BYTES,
  identifyRom,
  type MediaTypeId,
  type MediaTypeDescriptor,
} from '@/lib/media';
import { attachCdrom, insertFloppy } from './media';
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

// Stage an uploaded file into /opfs/upload for probing/persisting and return the
// staged path (or null on failure). Callers cleanup via discardStaging().
async function stageUpload(file: File): Promise<string | null> {
  const path = `${UPLOAD_DIR}/${sanitizeName(file.name) || 'image.img'}`;
  return (await streamToOpfs(path, file)) ? path : null;
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

export async function acceptFiles(files: File[], opts: AcceptFilesOptions = {}): Promise<void> {
  if (!files.length) return;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return;
  }
  const autoBootOnRom = opts.autoBootOnRom ?? true;

  // Checkpoint short-circuit on single-file drop.
  if (files.length === 1 && (await fileHasCheckpointSignature(files[0]))) {
    await loadCheckpointFile(files[0]);
    return;
  }

  startActivity(files[0].name);
  try {
    // A large file is a disk image (or an archive holding one): streamed
    // into a compact UDIF, never staged expanded.  A Mac archive is not
    // unpacked there; it falls through to the staged flow below.
    if (files.length === 1 && files[0].size > LARGE_IMPORT_BYTES) {
      const file = files[0];
      const out = await importImage({ kind: 'blob', blob: file }, file.name, {
        categories: ['cdrom', 'hd'],
        onSmall: async (path, name) => {
          const outcome = await probeAs(path, name, ALL_ORDER, { autoBootOnRom });
          await discardStaging(path);
          return outcome === 'persisted' ? path : null;
        },
      });
      if (out.handled) {
        if (out.path && out.category) await autoMountIfEmpty(out.path, out.category);
        return;
      }
    }
    let firstStagedPath: string | null = null;
    for (const file of files) {
      const staging = await stageUpload(file);
      if (staging) {
        if (!firstStagedPath) firstStagedPath = staging;
      } else {
        showNotification(`Upload failed: ${file.name}`, 'error');
        continue;
      }
    }
    if (!firstStagedPath) return;

    // For now, single-file flow only — multi-file drag is rare and the C side
    // doesn't compose well with multiple images at once.
    if (files.length === 1) {
      await probeAndPersist(firstStagedPath, files[0], { autoBootOnRom });
    } else {
      showNotification(`${files.length} files uploaded`, 'info');
    }
  } finally {
    endActivity();
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
      showNotification(`Upload failed: ${file.name}`, 'error');
      return null;
    }
    const descriptor = MEDIA_TYPES[category];
    const result = await descriptor.validate(staging, gsEval);
    if (!result.valid) {
      // A refusal says why; anything else is simply not this kind of file.
      const why = result.reject ?? `is not a valid ${descriptor.label}`;
      showNotification(`'${file.name}' ${why}`, 'error');
      await discardStaging(staging);
      return null;
    }
    const persisted = await persist(staging, file.name, descriptor, result.info);
    if (!persisted) return null;
    if (category === 'rom') await maybeBootFromRom(persisted);
    else await autoMountIfEmpty(persisted, category);
    return persisted;
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
        ok ? `'${file.name}' saved to ${targetDir}` : `Upload failed: ${file.name}`,
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
    // Into the model's CD bay; the core refuses an occupied bay and a model
    // with none, where this used to attach at id 3 regardless and report
    // success for any answer that was not null.
    const r = await attachCdrom(persistedPath);
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

// What probing one file came to: stored, refused (said why), or no match.
type ProbeOutcome = 'persisted' | 'rejected' | 'none';

// Try `order`'s media types on the file at `path` (named `name` for the
// store and the messages) and persist it as the first that validates.
async function probeAs(
  path: string,
  name: string,
  order: MediaTypeId[],
  opts: { autoBootOnRom: boolean; inArchive?: boolean },
): Promise<ProbeOutcome> {
  for (const id of order) {
    const descriptor = MEDIA_TYPES[id];
    const result = await descriptor.validate(path, gsEval);
    if (result.reject) {
      // This IS that kind of file, refused: say why and stop, rather than
      // letting the permissive hd probe store it as a disk image.
      showNotification(`'${name}' ${result.reject}`, 'error');
      return 'rejected';
    }
    if (!result.valid) continue;
    const persisted = await persist(path, name, descriptor, result.info, opts.inArchive);
    if (persisted) {
      if (id === 'rom') {
        if (opts.autoBootOnRom) await maybeBootFromRom(persisted);
      } else await autoMountIfEmpty(persisted, id);
    }
    return 'persisted';
  }
  return 'none';
}

// Probe a freshly-staged upload to figure out what kind of media it is,
// then persist it appropriately. If it looks like a ROM and no machine is
// running, auto-boot from it (same heuristic the legacy drop.js used)
// — gated by autoBootOnRom so the Welcome "Upload ROM..." button can
// keep the user on the Welcome screen instead of stranding them in a
// VROM-less / disk-less mid-boot.
async function probeAndPersist(
  stagingPath: string,
  file: File,
  opts: { autoBootOnRom: boolean },
): Promise<void> {
  // The strict types first: a ROM dump named .bin is a ROM, not MacBinary.
  if ((await probeAs(stagingPath, file.name, STRICT_ORDER, opts)) !== 'none') {
    await discardStaging(stagingPath);
    return;
  }

  // Then archives, before the permissive hd probe can claim them.  Whether a
  // file is one -- zip, StuffIt, Compact Pro, BinHex, MacBinary, gzip -- is
  // the core's format registry's call, from its content, not its name.  The
  // archive is a VFS namespace, so its members are probed where they are
  // and only the first medium that validates is copied out: nothing else is
  // unpacked.
  if (await stagedArchiveFormat(stagingPath)) {
    const outcome = await probeArchive(stagingPath, opts);
    await discardStaging(stagingPath);
    if (outcome === 'none') showNotification(`No mountable media inside ${file.name}`, 'warning');
    return;
  }

  if ((await probeAs(stagingPath, file.name, ['hd'], opts)) === 'none')
    showNotification(`${file.name} doesn't look like a ROM, floppy, HD, CD, or archive`, 'warning');
  await discardStaging(stagingPath);
}

// Probe the members of the archive staged at `stagingPath` through its VFS
// path: every file, in listing order, as any kind of medium, until one
// validates.  Dot files (AppleDouble "._" sidecars, which some zips carry)
// are never media.
async function probeArchive(
  stagingPath: string,
  opts: { autoBootOnRom: boolean },
): Promise<ProbeOutcome> {
  for (const inner of await listFiles(stagingPath)) {
    const base = inner.split('/').pop() ?? '';
    if (!base || base.startsWith('.')) continue;
    // Stored under its member name, made safe for OPFS (an HFS-derived
    // name can carry ':').
    const outcome = await probeAs(inner, opfsSafeName(base), ALL_ORDER, {
      ...opts,
      inArchive: true,
    });
    if (outcome !== 'none') return outcome;
  }
  return 'none';
}

// Every file under `dir` (depth first, in listing order), as full paths.
async function listFiles(dir: string): Promise<string[]> {
  const entries = await gsEval('files.list', [dir]);
  if (!Array.isArray(entries)) return [];
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
  // The source is a member of a (read-only) archive: copy it out, and leave
  // the archive for the caller to discard.
  inArchive = false,
): Promise<string | null> {
  const finalName = descriptor.nameFn ? descriptor.nameFn(originalName, info) : originalName;
  const targetDir = info?.persistDir ?? descriptor.persistDir;
  const finalPath = `${targetDir}/${finalName}`;
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
  // Moved, not copied, so storing never holds the file twice; an existing
  // file of that name (a content-named ROM uploaded again) is replaced, as
  // the copy always replaced it.
  const exists = (await gsEval('files.path_exists', [finalPath])) === true;
  const verb = exists || inArchive ? 'files.cp' : 'files.mv';
  const ok = (await gsEval(verb, [sourcePath, finalPath])) === true;
  if (!ok) {
    showNotification(`Failed to save ${originalName}`, 'error');
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
  showNotification(`${shown} uploaded`, 'info');
  return finalPath;
}

// Persist a file that is already on the worker's filesystem (staged
// anywhere, e.g. /tmp) as `category`: validate it as that category, then
// copy it into /opfs/images/<category>/ exactly as an upload of that kind
// is stored.  Returns the persisted path, or null if the file is not valid
// as `category` or could not be copied (the source is left in place).  The
// URL-media path uses this so a fetched image is kept the way a dropped one
// is, rather than attached from volatile /tmp.
export async function persistAs(
  sourcePath: string,
  originalName: string,
  category: MediaTypeId,
): Promise<string | null> {
  const descriptor = MEDIA_TYPES[category];
  const result = await descriptor.validate(sourcePath, gsEval);
  if (!result.valid) return null;
  return persist(sourcePath, originalName, descriptor, result.info);
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
  await prepareFreshMachine();
  showNotification(`Booted ${model} from uploaded ROM`, 'info');
}

async function loadCheckpointFile(file: File): Promise<void> {
  // Staged like any upload, a chunk at a time, under /opfs/upload, and
  // deleted once loaded.  It used to be read whole into memory and written
  // to the memory-backed /tmp, where it stayed for the session.
  const staged = `${UPLOAD_DIR}/dropped-${Date.now()}-${sanitizeName(file.name)}`;
  if (!(await streamToOpfs(staged, file))) {
    showNotification('Emulator not ready for checkpoint load', 'warning');
    return;
  }
  showNotification(`Loading checkpoint ${file.name}…`, 'info');
  const ok = (await gsEval('checkpoint.load', [staged])) === true;
  await discardStaging(staged);
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
