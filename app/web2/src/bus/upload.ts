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
// STAGING (see stageUpload): the write runs ON THE WORKER — Module.FS is proxied
// to the runtime thread, whose WasmFS drives OPFS via createSyncAccessHandle,
// the only browser-portable OPFS write path. Writing staging from the MAIN
// thread — the old approach — failed two ways: Safari's OPFS rejects main-thread
// createWritable() with "UnknownError", and on Chromium the worker's WasmFS
// can't see a main-thread OPFS write, so the follow-up storage.cp can't find the
// file and strands it in /opfs/upload. The file is sliced and written chunk by
// chunk, so uploads of any size (including hundreds-of-MB CD-ROMs) never buffer
// the whole file. This mirrors how move/delete route through the worker
// (storage.mv/storage.rm).

import { gsEval, isModuleReady, getModule, applyCapabilities } from './emulator';
import { opfs } from './opfs';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { setMounted, bumpImagesRevision } from '@/state/images.svelte';
import { startActivity, endActivity } from '@/state/activity.svelte';
import { sanitizeName, isZipFile, isMacArchive } from '@/lib/archive';
import { fileHasCheckpointSignature, ROMS_DIR, UPLOAD_DIR } from '@/lib/opfsPaths';
import { MEDIA_TYPES, type MediaTypeId, type MediaTypeDescriptor } from '@/lib/media';

interface RomIdentifyResult {
  recognised: boolean;
  compatible: string[];
  checksum: string;
  name: string;
  size: number;
}

async function romIdentify(path: string): Promise<RomIdentifyResult | null> {
  const r = await gsEval('machine.rom.identify', [path]);
  if (typeof r !== 'string') return null;
  try {
    const parsed = JSON.parse(r) as Partial<RomIdentifyResult>;
    if (parsed && parsed.recognised) return parsed as RomIdentifyResult;
  } catch {
    // Fall through.
  }
  return null;
}

// Bytes copied to the worker per FS.write. Bounds peak memory (one slice in the
// JS heap + one in the WASM heap at a time) so uploads of any size — including
// hundreds-of-MB CD-ROM images — never buffer the whole file anywhere.
const STAGE_CHUNK_BYTES = 4 * 1024 * 1024;

// Write a File to an OPFS path, chunk by chunk, ON THE WORKER: Module.FS is
// proxied to the runtime thread, whose WasmFS drives OPFS via
// createSyncAccessHandle — the only browser-portable OPFS write path.
// Main-thread createWritable() throws "UnknownError" on Safari, and a
// main-thread OPFS write isn't visible to the worker's WasmFS on Chromium
// (stranding the file). Slicing the File and writing chunk-by-chunk means
// nothing buffers the whole file, so any size (incl. large CD-ROMs) works.
// Returns true on success.
async function streamFileToOpfs(opfsPath: string, file: File): Promise<boolean> {
  const mod = getModule();
  if (!mod) return false;
  let stream: unknown;
  try {
    stream = mod.FS.open(opfsPath, 'w');
    for (let pos = 0; pos < file.size; ) {
      const end = Math.min(pos + STAGE_CHUNK_BYTES, file.size);
      const chunk = new Uint8Array(await file.slice(pos, end).arrayBuffer());
      mod.FS.write(stream, chunk, 0, chunk.length, pos);
      pos = end;
    }
    return true;
  } catch (err) {
    console.error('upload: OPFS stream write failed', err);
    return false;
  } finally {
    if (stream !== undefined) {
      try {
        mod.FS.close(stream);
      } catch {
        // Already closed / open failed — nothing to release.
      }
    }
  }
}

// Stage an uploaded file into /opfs/upload for probing/persisting and return the
// staged path (or null on failure). Callers cleanup via discardStaging().
async function stageUpload(file: File): Promise<string | null> {
  const path = `${UPLOAD_DIR}/${sanitizeName(file.name) || 'image.img'}`;
  return (await streamFileToOpfs(path, file)) ? path : null;
}

// Best-effort removal of a staged file / unpacked-archive dir. Everything staging
// touches is written by the worker (FS streaming above, archive.extract), so the
// recursive rm runs worker-side too — keeping its WasmFS view coherent.
async function discardStaging(path: string): Promise<void> {
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
// doesn't match. Single-file flow.
export async function acceptFilesAsCategory(
  files: File[],
  category: MediaTypeId,
): Promise<boolean> {
  if (!files.length) return false;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return false;
  }
  const file = files[0];
  startActivity(file.name);
  try {
    const staging = await stageUpload(file);
    if (!staging) {
      showNotification(`Upload failed: ${file.name}`, 'error');
      return false;
    }
    const descriptor = MEDIA_TYPES[category];
    const result = await descriptor.validate(staging, gsEval);
    if (!result.valid) {
      showNotification(`'${file.name}' is not a valid ${descriptor.label}`, 'error');
      await discardStaging(staging);
      return false;
    }
    const persisted = await persist(staging, file.name, descriptor, result.info);
    if (!persisted) return false;
    if (category === 'rom') await maybeBootFromRom(persisted);
    else await autoMountIfEmpty(persisted, category);
    return true;
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
      const ok = await streamFileToOpfs(finalPath, file);
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
    let sawEmptyDrive = false;
    for (let i = 0; i < 2; i++) {
      const present = await gsEval(`machine.floppy.drive[${i}].present`);
      if (present === true) continue;
      // A failed insert into an empty slot means the image couldn't be
      // opened, not that the drives are full — keep the two apart.
      sawEmptyDrive = true;
      const ok =
        (await gsEval(`machine.floppy.drive[${i}].insert`, [persistedPath, true])) === true;
      if (ok) {
        setMounted(persistedPath, { kind: 'fd', drive: i });
        showNotification(`Inserted into floppy drive ${i + 1}`, 'info');
        return;
      }
    }
    showNotification(
      sawEmptyDrive
        ? 'Image saved, but it could not be inserted (the image could not be opened)'
        : 'Both floppy drives are full — image saved but not mounted',
      'warning',
    );
    return;
  }
  if (category === 'cdrom') {
    const ok = (await gsEval('machine.scsi.attach_cdrom', [persistedPath, 3])) !== null;
    if (ok) {
      setMounted(persistedPath, { kind: 'cd', drive: 3 });
      showNotification('Inserted into CD-ROM drive', 'info');
    } else {
      showNotification('CD-ROM drive is occupied — image saved but not mounted', 'warning');
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
  //   fd     — exact floppy sizes (400/800/1440 KB ± DC42 header)
  //   cdrom  — ISO 9660 / HFS / APM signature inside the file
  //   hd     — permissive fallback
  const ORDER: MediaTypeId[] = ['rom', 'vrom', 'fd', 'cdrom', 'hd'];
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
    | { persistDir?: string; checksum?: string; canonicalName?: string; [k: string]: unknown }
    | undefined,
): Promise<string | null> {
  const finalName = descriptor.nameFn ? descriptor.nameFn(originalName, info) : originalName;
  const targetDir = info?.persistDir ?? descriptor.persistDir;
  const finalPath = `${targetDir}/${finalName}`;
  const ok = (await gsEval('storage.cp', [sourcePath, finalPath])) === true;
  if (!ok) {
    showNotification(`Failed to save ${originalName}`, 'error');
    return null;
  }
  await discardStaging(sourcePath);
  // Notify inventory watchers (e.g. WelcomeConfigSlide's dropdown
  // refresh effect) that the OPFS image catalog has changed.
  bumpImagesRevision();
  // When the file was recognised and stored under a canonical name (VROM →
  // its declaration-ROM canonical filename), surface that in the toast — it
  // tells the user we actually identified the file, not just "it landed in
  // OPFS". The card's human name is shown later in the config dialog (sourced
  // from machine.profile), so we don't duplicate that knowledge here.
  const canonical = info?.canonicalName as string | undefined;
  const shown = canonical && canonical !== originalName ? canonical : originalName;
  showNotification(`${shown} uploaded`, 'info');
  return finalPath;
}

// If the user dropped a ROM and no machine is running yet, boot a default
// configuration so they go straight to a usable Mac without going through
// the Configuration slide. Picks the first compatible model from the ROM
// info; the user can switch later.
async function maybeBootFromRom(romPath: string): Promise<void> {
  if (machine.status === 'running' || machine.status === 'paused') return;
  const info = await romIdentify(romPath);
  if (!info || !info.compatible.length) return;
  const model = info.compatible[0];
  const profile = await gsEval('machine.profile', [model]);
  let ramKb = 4096;
  if (typeof profile === 'string') {
    try {
      const parsed = JSON.parse(profile) as { ram_default?: number };
      if (parsed.ram_default) ramKb = parsed.ram_default;
    } catch {
      /* keep default */
    }
  }
  await gsEval('machine.boot', [model, ramKb]);
  await gsEval('machine.rom.load', [romPath]);
  machine.model = model;
  machine.ram = `${ramKb / 1024} MB`;
  await applyCapabilities(model);
  await gsEval('scheduler.run');
  showNotification(`Booted ${model} from uploaded ROM`, 'info');
}

async function loadCheckpointFile(file: File): Promise<void> {
  const tmpPath = `/tmp/dropped-${Date.now()}-${sanitizeName(file.name)}`;
  // Stage to memory-backed /tmp first (cross-thread safe), then load.
  const buf = new Uint8Array(await file.arrayBuffer());
  const mod = getModule();
  if (!mod) {
    showNotification('Emulator not ready for checkpoint load', 'warning');
    return;
  }
  mod.FS.writeFile(tmpPath, buf);
  showNotification(`Loading checkpoint ${file.name}…`, 'info');
  const ok = (await gsEval('checkpoint.load', [tmpPath])) === true;
  if (ok) {
    showNotification(`Checkpoint loaded (${file.name})`, 'info');
  } else {
    showNotification(`Checkpoint load failed`, 'error');
  }
}

// Programmatic file-picker entry — Welcome's "Upload ROM..." button calls
// this. Wraps an invisible `<input type="file">` click.
export function openFilePicker(accept = ''): Promise<File[]> {
  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    if (accept) input.accept = accept;
    input.multiple = true;
    input.style.display = 'none';
    input.addEventListener(
      'change',
      () => {
        const files = input.files ? Array.from(input.files) : [];
        document.body.removeChild(input);
        resolve(files);
      },
      { once: true },
    );
    document.body.appendChild(input);
    input.click();
  });
}

// Used by Welcome's "Upload ROM..." button — opens the picker, runs the full
// pipeline, then triggers a Recent refresh by re-reading OPFS.
//
// The Welcome button passes { autoBootOnRom: false }: that surface is a
// configuration entry point, not a "boot now" shortcut. The Display
// drag-and-drop path leaves the default on so dropping a ROM there
// still cold-boots straight away.
export async function pickAndUpload(accept = '', opts: AcceptFilesOptions = {}): Promise<void> {
  const files = await openFilePicker(accept);
  if (files.length) await acceptFiles(files, opts);
  // Touch the recent list so consumers re-scan.
  void opfs.scanRoms().catch(() => undefined);
}

// Category-strict picker variant — the New Machine dialog uses this so
// picking "Upload image..." in (say) the floppy slot only accepts a
// floppy. Returns the persistDir-relative path of the persisted file
// (when the upload succeeds), or null when the user cancelled or the
// file was rejected.
export async function pickAndUploadAs(category: MediaTypeId, accept = ''): Promise<boolean> {
  const files = await openFilePicker(accept);
  if (!files.length) return false;
  return acceptFilesAsCategory(files, category);
}

export { ROMS_DIR };
