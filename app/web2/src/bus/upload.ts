// Upload pipeline — port of app/web/js/drop.js + upload-pipeline.js
// + media.js. Single entry point: `acceptFiles(files)` (called from the
// drag-and-drop overlay, the Upload-ROM button, and the URL-media path).
//
// Flow:
//   1. Check first file for checkpoint signature — short-circuit to load.
//   2. Stage each file into /opfs/upload/ (see stageUpload).
//   3. Probe the staged file (ROM? floppy? archive? something else?).
//   4. If a ROM was uploaded, also boot a default machine from it.
//   5. Persist to the right /opfs/images/<category>/ via gsEval('storage.cp').
//   6. Cleanup the staging copy.
//
// STAGING (see stageUpload): the write goes through the emulator's own
// filesystem, on the emulator thread: the page copies each chunk into the
// core's transfer window and storage.xfer_write puts it in the file
// (bus/xfer.ts).  The page never touches the filesystem itself.  It used to
// call Module.FS, which under WasmFS runs on the page's thread and busy-waits
// for WasmFS's OPFS thread -- jank on Chrome, and a deadlock on Safari, where
// WebKit serves that thread's OPFS requests through the busy page thread.
// Writing staging with the page's own navigator.storage failed too: Safari's
// OPFS rejects main-thread createWritable() with "UnknownError", and on
// Chromium the emulator's WasmFS can't see an out-of-band OPFS write, so the
// follow-up storage.cp strands the file in /opfs/upload.  The file is sliced
// and written chunk by chunk, so uploads of any size (including hundreds-of-MB
// CD-ROMs) never buffer the whole file.  This mirrors how move/delete route
// through the worker (storage.mv/storage.rm).

import { gsEval, gsErrorText, isModuleReady } from './emulator';
import { xferChunkBytes, xferWrite } from './xfer';
import { reconcileUiWithMachine, prepareFreshMachine } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { setMounted, bumpImagesRevision } from '@/state/images.svelte';
import { startActivity, endActivity } from '@/state/activity.svelte';
import { sanitizeName, isZipFile, isMacArchive } from '@/lib/archive';
import { fileHasCheckpointSignature, ROMS_DIR, UPLOAD_DIR } from '@/lib/opfsPaths';
import { MEDIA_TYPES, identifyRom, type MediaTypeId, type MediaTypeDescriptor } from '@/lib/media';
import { attachCdrom, insertFloppy } from './media';

// The one chunked writer (R3): everything the page puts on the emulator's
// filesystem — an upload's File, a URL download's response body, a dropped
// checkpoint — goes through here, to an OPFS path, a transfer window at a time
// (bus/xfer.ts; see STAGING above).  A stream's small network chunks are
// gathered into full windows first, so a download costs one request per
// window, not per packet.  Nothing buffers the whole file, so any size works.
// Returns true on success.
export async function streamToOpfs(
  opfsPath: string,
  source: Blob | ReadableStream<Uint8Array> | Uint8Array,
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
      for (let at = 0; at < source.length; at += size)
        await put(source.subarray(at, Math.min(at + size, source.length)));
    } else if (source instanceof Blob) {
      for (let at = 0; at < source.size; at += size)
        await put(
          new Uint8Array(await source.slice(at, Math.min(at + size, source.size)).arrayBuffer()),
        );
    } else {
      const reader = source.getReader();
      const pending = new Uint8Array(size);
      let fill = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        for (let at = 0; value && at < value.length; ) {
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

// Best-effort removal of a staged file / unpacked-archive dir. Everything staging
// touches is written by the worker (FS streaming above, archive.extract), so the
// recursive rm runs worker-side too — keeping its WasmFS view coherent.
export async function discardStaging(path: string): Promise<void> {
  await gsEval('storage.rm', [path]);
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
    const staging = await stageUpload(file);
    if (!staging) {
      showNotification(`Upload failed: ${file.name}`, 'error');
      return null;
    }
    const descriptor = MEDIA_TYPES[category];
    const result = await descriptor.validate(staging, gsEval);
    if (!result.valid) {
      showNotification(`'${file.name}' is not a valid ${descriptor.label}`, 'error');
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
  for (const file of files) {
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
    // success for any answer that was not null (N-04).
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
  // Try each media type until one validates. Order matters: probe the
  // strict-size / signature matchers first, fall through to HD last
  // (the HD probe accepts anything that just opens and isn't floppy-
  // sized, so it will happily classify a 32 KB VROM as a tiny "hard
  // disk" if VROM hasn't already claimed the file).
  //   rom    — exact size match against the ROM catalog
  //   vrom   — exact 32 KB match
  //   prom   — $55AA + a reachable PCIR + Open Firmware code type
  //   fd     — exact floppy sizes (400/800/1440 KB ± DC42 header)
  //   cdrom  — ISO 9660 / HFS / APM signature inside the file
  //   hd     — permissive fallback
  //
  // vrom and prom cannot claim each other's files even though both are
  // commonly 32 KB: a vROM is identified by a NuBus Format-Block CRC in
  // its trailing bytes, a PROM by a PCI Data Structure near its head, so
  // whichever runs first the other still fails. They sit adjacent anyway,
  // ahead of the permissive hd fallback that would otherwise swallow both.
  const ORDER: MediaTypeId[] = ['rom', 'vrom', 'prom', 'fd', 'cdrom', 'hd'];
  for (const id of ORDER) {
    const descriptor = MEDIA_TYPES[id];
    const result = await descriptor.validate(stagingPath, gsEval);
    if (!result.valid) continue;
    const persisted = await persist(stagingPath, file.name, descriptor, result.info);
    if (persisted) {
      if (id === 'rom') {
        if (opts.autoBootOnRom) await maybeBootFromRom(persisted);
      } else await autoMountIfEmpty(persisted, id);
    }
    return;
  }

  // No raw type matched. If it's an archive extension, ask the C side to
  // extract; the result lands under /opfs/upload/<name>_unpacked/ and a
  // single probe pass classifies the first image we find inside.
  if (isZipFile(file.name) || isMacArchive(file.name)) {
    showNotification(`Extracting ${file.name}...`, 'info');
    const extractDir = `${UPLOAD_DIR}/${sanitizeName(file.name)}_unpacked`;
    const ok = (await gsEval('archive.extract', [stagingPath, extractDir])) === true;
    if (!ok) {
      showNotification(`Failed to extract ${file.name}`, 'error');
      await discardStaging(stagingPath);
      return;
    }
    const innerPath = `${extractDir}/_found_media.img`;
    const found = (await gsEval('storage.find_media', [extractDir, innerPath])) === true;
    if (!found) {
      showNotification(`No mountable media inside ${file.name}`, 'warning');
      await discardStaging(stagingPath);
      await discardStaging(extractDir);
      return;
    }
    // Retry the descriptor probe on the extracted image.
    for (const id of ORDER) {
      const descriptor = MEDIA_TYPES[id];
      const result = await descriptor.validate(innerPath, gsEval);
      if (!result.valid) continue;
      const persisted = await persist(innerPath, file.name, descriptor, result.info);
      if (persisted) {
        if (id === 'rom') {
          if (opts.autoBootOnRom) await maybeBootFromRom(persisted);
        } else await autoMountIfEmpty(persisted, id);
      }
      await discardStaging(stagingPath);
      await discardStaging(extractDir);
      return;
    }
    showNotification(`Extracted ${file.name} but no recognised image inside`, 'warning');
    await discardStaging(stagingPath);
    await discardStaging(extractDir);
    return;
  }

  showNotification(`${file.name} doesn't look like a ROM, floppy, HD, CD, or archive`, 'warning');
  await discardStaging(stagingPath);
}

async function persist(
  sourcePath: string,
  originalName: string,
  descriptor: MediaTypeDescriptor,
  info:
    | { persistDir?: string; checksum?: string; cardId?: string; [k: string]: unknown }
    | undefined,
): Promise<string | null> {
  const finalName = descriptor.nameFn ? descriptor.nameFn(originalName, info) : originalName;
  const targetDir = info?.persistDir ?? descriptor.persistDir;
  const finalPath = `${targetDir}/${finalName}`;
  // storage.cp does not create parent directories, and the category dirs are
  // made once at startup — so a store added after a user's OPFS was first
  // laid down has nowhere to copy to, and every upload of that kind fails
  // with a bare "Failed to save". Create it here; it is a no-op when the
  // directory already exists.
  //
  // Through vfs.mkdir, NOT the main-thread opfs.mkdirP: the copy below runs
  // on the worker, and a main-thread OPFS mutation is not reliably visible
  // to the worker's WasmFS (the same asymmetry that forces staging onto the
  // worker — see stageUpload). Creating it on one side and copying on the
  // other is exactly the bug this is fixing.
  await gsEval('vfs.mkdir', [targetDir]);
  const ok = (await gsEval('storage.cp', [sourcePath, finalPath])) === true;
  if (!ok) {
    showNotification(`Failed to save ${originalName}`, 'error');
    return null;
  }
  await discardStaging(sourcePath);
  // Offer a freshly stored vROM to the core's content-addressed registry.
  // The startup enumeration ran once at page load, so without this an
  // "(auto)" boot after a mid-session upload would not see the file until
  // the next reload.
  if (descriptor.id === 'vrom') {
    await gsEval('machine.vrom.offer', [finalPath]);
  }
  // Same for a PCI expansion ROM, through its own registry.
  if (descriptor.id === 'prom') {
    await gsEval('machine.prom.offer', [finalPath]);
  }
  // Notify inventory watchers (e.g. WelcomeConfigSlide's dropdown
  // refresh effect) that the OPFS image catalog has changed.
  bumpImagesRevision();
  // When the file was recognised as a vROM, surface the card it provides in
  // the toast — it tells the user we actually identified the file, not just
  // "it landed in OPFS". The card's human display name is shown later in the
  // config dialog (sourced from machine.profile), so we don't duplicate that
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
  // and "boot" it (N-08).
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
  // to the memory-backed /tmp, where it stayed for the session (N-22).
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

// Programmatic file-picker entry — Welcome's "Upload ROM..." button calls
// this. Wraps an invisible `<input type="file">` click.  Resolves with the
// chosen files, or [] when the user cancels the dialog: `cancel` fires
// instead of `change` then (every browser that can run the emulator has it),
// and without it the promise stayed pending and the input leaked (F-36).
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
