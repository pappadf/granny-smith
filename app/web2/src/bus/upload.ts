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
export async function acceptFiles(files: File[]): Promise<void> {
  if (!files.length) return;
  if (!isModuleReady()) {
    showNotification('Emulator still starting; please retry', 'warning');
    return;
  }

  // Checkpoint short-circuit on single-file drop.
  if (files.length === 1 && (await fileHasCheckpointSignature(files[0]))) {
    await loadCheckpointFile(files[0]);
    return;
  }

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
    await probeAndPersist(firstStagedPath, files[0]);
  } else {
    showNotification(`${files.length} files uploaded to ${UPLOAD_DIR}`, 'info');
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
// running, auto-boot from it (same heuristic the legacy drop.js used).
async function probeAndPersist(stagingPath: string, file: File): Promise<void> {
  // Try each media type until one validates. The order matters: ROM is
  // probed first so a fresh download lands the user in a runnable state.
  const ORDER: MediaTypeId[] = ['rom', 'fd', 'hd', 'cdrom', 'vrom'];
  for (const id of ORDER) {
    const descriptor = MEDIA_TYPES[id];
    const result = await descriptor.validate(stagingPath, gsEval);
    if (!result.valid) continue;
    const persisted = await persist(stagingPath, file.name, descriptor, result.info);
    if (persisted && id === 'rom') await maybeBootFromRom(persisted);
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
      if (persisted && id === 'rom') await maybeBootFromRom(persisted);
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
  info: { persistDir?: string; checksum?: string } | undefined,
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
  showNotification(`${originalName} uploaded`, 'info');
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
export async function pickAndUpload(accept = ''): Promise<void> {
  const files = await openFilePicker(accept);
  if (files.length) await acceptFiles(files);
  // Touch the recent list so consumers re-scan.
  void opfs.scanRoms().catch(() => undefined);
}

export { ROMS_DIR };
