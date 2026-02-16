// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Handles drag-and-drop UI cue and the processDrop pipeline.
import { isModuleReady } from './emulator.js';
import { getFS, ensureDir, ensureDirs, writeBinary, romPath, persistSync } from './fs.js';
import {
  sanitizeName, quotePath, tryExtractArchive, classifyMediaFile, findMediaInDirectory,
  bufferHasCheckpointSignature, fileHasCheckpointSignature
} from './media.js';
import { showUploadDialog } from './dialogs.js';
import { toast, hideRomOverlay, enableRunButton, setBackgroundMessage } from './ui.js';

// --- Checkpoint drop helpers ---

// Build a unique /tmp path for checkpoint uploads
function makeCheckpointTmpPath(displayName) {
  const safe = sanitizeName(displayName || 'checkpoint.bin') || 'checkpoint.bin';
  return `/tmp/${Date.now()}-${safe}`;
}

// Load checkpoint file into the emulator, pausing first if needed
async function loadCheckpointFromPath(path, displayName) {
  const label = displayName || path;
  if (!isModuleReady()) {
    toast(`Checkpoint stored at ${path}; emulator still starting (run load-state manually once ready)`);
    return false;
  }
  // Import setRunning late to avoid circular init issues
  const { setRunning } = await import('./emulator.js');

  try {
    // load-state replaces the entire emulator state (including scheduler);
    // no need to pause first.  The restored scheduler's running flag
    // determines whether execution continues or stays paused.
    toast(`Loading ${label}…`);
    await window.runCommand(`load-state ${path}`);

    // Sync JS-side running flag with the restored scheduler state
    const running = (await window.runCommand('status')) === 1;
    setRunning(running);
    toast(`Checkpoint loaded (${label})`);
    enableRunButton();
    return true;
  } catch (err) {
    console.error('Checkpoint load failed', err);
    toast('Checkpoint load failed; see terminal');
    return false;
  }
}

// Persist a checkpoint buffer to /tmp and trigger load-state
async function importCheckpointBuffer(buf, displayName) {
  if (!bufferHasCheckpointSignature(buf)) return false;
  ensureDir('/tmp');
  const dest = makeCheckpointTmpPath(displayName);
  writeBinary(dest, buf);
  const label = displayName || dest;
  showUploadDialog(`Checkpoint restored from ${label}`);
  await loadCheckpointFromPath(dest, label);
  return true;
}

// Short-circuit when a checkpoint file is detected
async function maybeHandleCheckpointDrop(file) {
  if (!file) return false;
  if (!(await fileHasCheckpointSignature(file))) return false;
  const buf = new Uint8Array(await file.arrayBuffer());
  await importCheckpointBuffer(buf, file.name);
  return true;
}

// --- File extraction from DataTransfer ---

async function extractAllDroppedFiles(dt) {
  const out = [];
  // Prefer DataTransferItem API for directory support
  if (dt.items && dt.items.length) {
    const pending = [];
    for (const item of dt.items) {
      if (item.kind !== 'file') continue;
      const entry = item.webkitGetAsEntry ? item.webkitGetAsEntry() : null;
      if (entry) {
        pending.push(traverseEntry(entry, ''));
      } else {
        const f = item.getAsFile();
        if (f) out.push({ file: f, relPath: f.name });
      }
    }
    await Promise.all(pending);
    return out;

    async function traverseEntry(entry, prefix) {
      if (entry.isFile) {
        await new Promise(res => entry.file(f => { out.push({ file: f, relPath: prefix + f.name }); res(); }, () => res()));
      } else if (entry.isDirectory) {
        const reader = entry.createReader();
        await new Promise(resDir => {
          const iter = () => {
            reader.readEntries(async entries => {
              if (!entries.length) return resDir();
              for (const ent of entries) { await traverseEntry(ent, prefix + entry.name + '/'); }
              iter();
            }, () => resDir());
          };
          iter();
        });
      }
    }
  }
  // Fallback to file list (no directory structure)
  const list = dt.files || [];
  if (list.length && !out.length) {
    for (const f of list) { out.push({ file: f, relPath: f.webkitRelativePath || f.name }); }
  }
  return out;
}

// --- Upload and probe pipeline ---

// Store files to /tmp/upload and auto-process archives and disk images
async function storeInTmpAndProbe(files) {
  if (!isModuleReady()) {
    toast('Emulator still starting; please wait');
    return;
  }
  const FS = getFS();
  ensureDir('/tmp');
  ensureDir('/tmp/upload');

  // Upload all files to /tmp/upload first
  const uploadedPaths = [];
  for (const { file, relPath } of files) {
    const cleanPath = relPath.split('/').map(sanitizeName).join('/');
    const dest = '/tmp/upload/' + cleanPath;
    const data = new Uint8Array(await file.arrayBuffer());
    ensureDirs(dest);
    try { FS.unlink(dest); } catch (_) {}
    FS.writeFile(dest, data);
    uploadedPaths.push({ dest, displayName: file.name, data });
  }

  const msg = files.length === 1
    ? `${files[0].file.name} uploaded to /tmp/upload`
    : `${files.length} files uploaded to /tmp/upload`;
  toast(msg);

  // For single files, check if it's an archive or disk image
  if (uploadedPaths.length === 1) {
    const { dest, displayName, data } = uploadedPaths[0];
    let finalPath = dest;
    let isDirectory = false;

    // Try to extract if it's an archive
    const extraction = await tryExtractArchive(dest, displayName, data);
    if (extraction.extracted) {
      finalPath = extraction.extractDir;
      isDirectory = true;
    }

    // Now probe for disk images and auto-mount if found
    await probeAndMountDiskImage(finalPath, isDirectory, displayName);
  }
}

// Probe a path for disk images and auto-mount if valid
async function probeAndMountDiskImage(path, isDirectory, displayName) {
  const FS = getFS();
  try {
    let media = null;
    if (isDirectory) {
      media = await findMediaInDirectory(path);
    } else {
      const kind = await classifyMediaFile(path);
      if (kind) media = { path, kind };
    }

    if (!media) {
      showUploadDialog(`${displayName} uploaded to ${path} (no mountable disk image or ROM found)`);
      return;
    }

    const { path: imagePath, kind } = media;

    if (kind === 'rom') {
      toast(`Loading ROM: ${imagePath.split('/').pop()}`);
      try {
        const romData = FS.readFile(imagePath);
        writeBinary(romPath(), romData);
        await persistSync();
        // Import loadRomAndMaybeRun lazily to avoid circular deps
        const { loadRomAndMaybeRun } = await import('./url-media.js');
        await loadRomAndMaybeRun();
        toast(`ROM loaded successfully`);
        setBackgroundMessage('Click \u25b6 to start emulation');
      } catch (err) {
        console.error('Failed to load ROM:', err);
        toast(`Failed to load ROM`);
      }
    } else if (kind === 'floppy') {
      toast(`Mounting disk image: ${imagePath.split('/').pop()}`);
      const mountResult = await window.runCommand(`insert-fd ${quotePath(imagePath)} 0 1`);
      if (mountResult === 0) {
        toast(`Disk image mounted successfully`);
      } else {
        toast(`Failed to mount disk image (error ${mountResult})`);
        showUploadDialog(`${displayName} uploaded to ${imagePath} (mount failed)`);
      }
    }
  } catch (err) {
    console.error('Probe and mount failed:', err);
    showUploadDialog(`${displayName} uploaded to ${path} (disk probe failed)`);
  }
}

// --- Main drop processor ---

async function processDrop(dt) {
  if (!dt) return;
  const files = await extractAllDroppedFiles(dt);
  if (!files.length) return;
  // Single file - check if it's a checkpoint first
  if (files.length === 1) {
    const f = files[0].file;
    if (await maybeHandleCheckpointDrop(f)) {
      return;
    }
  }
  // Default: upload to /tmp/upload and probe for ROMs, archives, and disks
  await storeInTmpAndProbe(files);
}

// --- Drag & drop DOM wiring ---

// Initialize drag-and-drop on the canvas.
export function initDragDrop(canvasEl) {
  const dropHint = document.getElementById('drop-hint');
  let dragDepth = 0;
  function addCue() { canvasEl.classList.add('dragover'); dropHint.classList.add('active'); }
  function rmCue() { canvasEl.classList.remove('dragover'); dropHint.classList.remove('active'); dragDepth = 0; }

  // Document-level listeners provide stable behavior when moving across child nodes
  document.addEventListener('dragenter', (e) => { e.preventDefault(); dragDepth++; addCue(); });
  document.addEventListener('dragover', (e) => { e.preventDefault(); });
  document.addEventListener('dragleave', (e) => { e.preventDefault(); if (--dragDepth <= 0) rmCue(); });
  document.addEventListener('drop', (e) => {
    e.preventDefault();
    rmCue();
    // Only process drops that land on the canvas (or its descendants)
    const target = e.target;
    if (target === canvasEl || (target instanceof Node && canvasEl.contains(target))) {
      processDrop(e.dataTransfer);
    }
  });
  // Some UAs won't fire dragleave when the pointer exits the viewport
  window.addEventListener('mousemove', (e) => {
    if (dragDepth > 0) {
      const out = e.clientX <= 0 || e.clientY <= 0 || e.clientX >= window.innerWidth || e.clientY >= window.innerHeight;
      if (out) rmCue();
    }
  });
  // Cleanup on dragend (e.g., ESC cancel)
  document.addEventListener('dragend', () => rmCue());
}
