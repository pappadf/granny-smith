// Upload pipeline — port of app/web/js/drop.js + upload-pipeline.js
// + media.js. Single entry point: `acceptFiles(files)` (called from the
// drag-and-drop overlay, the Upload-ROM button, and the URL-media path).
//
// Flow:
//   1. Check first file for checkpoint signature — short-circuit to load.
//   2. Stage each file into /opfs/upload/ via direct OPFS write.
//   3. Probe the staged file (ROM? floppy? archive? something else?).
//   4. If a ROM was uploaded, also boot a default machine from it.
//   5. Persist to the right /opfs/images/<category>/ via gsEval('storage.cp').
//   6. Cleanup /opfs/upload/.

import { gsEval, isModuleReady, getModule } from './emulator';
import { writeToOPFS, removeFromOPFS, opfs } from './opfs';
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
  const r = await gsEval('rom.identify', [path]);
  if (typeof r !== 'string') return null;
  try {
    const parsed = JSON.parse(r) as Partial<RomIdentifyResult>;
    if (parsed && parsed.recognised) return parsed as RomIdentifyResult;
  } catch {
    // Fall through.
  }
  return null;
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
      const safe = sanitizeName(file.name) || 'image.img';
      const staging = `${UPLOAD_DIR}/${safe}`;
      try {
        await writeToOPFS(staging, file);
        if (!firstStagedPath) firstStagedPath = staging;
      } catch (err) {
        console.error('upload: OPFS write failed', err);
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
      showNotification(`${files.length} files uploaded to ${UPLOAD_DIR}`, 'info');
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
  const safe = sanitizeName(file.name) || 'image.img';
  const staging = `${UPLOAD_DIR}/${safe}`;
  startActivity(file.name);
  try {
    try {
      await writeToOPFS(staging, file);
    } catch (err) {
      console.error('upload: OPFS write failed', err);
      showNotification(`Upload failed: ${file.name}`, 'error');
      return false;
    }
    const descriptor = MEDIA_TYPES[category];
    const result = await descriptor.validate(staging, gsEval);
    if (!result.valid) {
      showNotification(`'${file.name}' is not a valid ${descriptor.label}`, 'error');
      await removeFromOPFS(staging);
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
      await writeToOPFS(finalPath, file);
      showNotification(`'${file.name}' saved to ${targetDir}`, 'info');
    } catch (err) {
      console.error('upload: OPFS write failed', err);
      showNotification(`Upload failed: ${file.name}`, 'error');
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
    for (let i = 0; i < 2; i++) {
      const present = await gsEval(`floppy.drives[${i}].present`);
      if (present === true) continue;
      const ok = (await gsEval(`floppy.drives[${i}].insert`, [persistedPath, true])) !== null;
      if (ok) {
        setMounted(persistedPath, true);
        showNotification(`Inserted into floppy drive ${i}`, 'info');
        return;
      }
    }
    showNotification('Both floppy drives are full — image saved but not mounted', 'warning');
    return;
  }
  if (category === 'cdrom') {
    const ok = (await gsEval('scsi.attach_cdrom', [persistedPath, 3])) !== null;
    if (ok) {
      setMounted(persistedPath, true);
      showNotification('Mounted into CD-ROM drive', 'info');
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
      await removeFromOPFS(stagingPath);
      return;
    }
    const innerPath = `${extractDir}/_found_media.img`;
    const found = (await gsEval('storage.find_media', [extractDir, innerPath])) === true;
    if (!found) {
      showNotification(`No mountable media inside ${file.name}`, 'warning');
      await removeFromOPFS(stagingPath);
      await removeFromOPFS(extractDir);
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
      await removeFromOPFS(stagingPath);
      await removeFromOPFS(extractDir);
      return;
    }
    showNotification(`Extracted ${file.name} but no recognised image inside`, 'warning');
    await removeFromOPFS(stagingPath);
    await removeFromOPFS(extractDir);
    return;
  }

  showNotification(`${file.name} doesn't look like a ROM, floppy, HD, CD, or archive`, 'warning');
  await removeFromOPFS(stagingPath);
}

async function persist(
  sourcePath: string,
  originalName: string,
  descriptor: MediaTypeDescriptor,
  info:
    | { persistDir?: string; checksum?: string; cardName?: string; [k: string]: unknown }
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
  await removeFromOPFS(sourcePath);
  // Notify inventory watchers (e.g. WelcomeConfigSlide's dropdown
  // refresh effect) that the OPFS image catalog has changed.
  bumpImagesRevision();
  // For media that carries a human-readable label (VROM → card_name)
  // surface it in the toast — gives the user feedback that we actually
  // recognised the file, not just "it landed in OPFS".
  const label = (info?.cardName as string | undefined) ?? originalName;
  showNotification(info?.cardName ? `${label} VROM uploaded` : `${originalName} uploaded`, 'info');
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
  await gsEval('rom.load', [romPath]);
  machine.model = model;
  machine.ram = `${ramKb / 1024} MB`;
  machine.mmuEnabled = /SE\/30|II/i.test(model);
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
