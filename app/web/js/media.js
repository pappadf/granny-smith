// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// File-type detection, archive extraction, ROM/floppy probing, and classification.
// Media operations are separated from UI concerns; toast calls are kept for user feedback.
import { ensureDir, ensureDirs, getFS } from './fs.js';
import { toast } from './ui.js';

// --- JSZip lazy singleton ---
let _jszip = null;
async function getJSZip() {
  if (!_jszip) {
    const { default: JSZip } = await import('https://cdn.jsdelivr.net/npm/jszip@3.10.1/+esm');
    _jszip = JSZip;
  }
  return _jszip;
}

// --- File type detection ---

const ARCHIVE_EXTENSIONS = /\.(sit|hqx|cpt|bin|sea)$/i;
const ZIP_EXTENSION = /\.zip$/i;

// Check if filename indicates a ZIP archive
export function isZipFile(name) { return ZIP_EXTENSION.test(name || ''); }

// Check if filename indicates a peeler-supported archive
export function isPeelerArchive(name) { return ARCHIVE_EXTENSIONS.test(name || ''); }

// Check if a filename looks like an archive (ZIP or peeler)
export function isArchiveFile(name) { return isZipFile(name) || isPeelerArchive(name); }

// --- Sanitization ---

// Sanitize a file name for use in FS paths
export function sanitizeName(n) { return n.replace(/[^A-Za-z0-9._-]+/g, '_'); }

// Quote a path for shell commands
export function quotePath(p) { return `"${(p || '').replace(/"/g, '\\"')}"`; }

// --- Archive extraction ---

// Extract a ZIP file to a directory, returns list of extracted file paths.
export async function extractZipToDir(data, extractDir) {
  const JSZip = await getJSZip();
  const zip = await JSZip.loadAsync(data);
  const fileNames = Object.keys(zip.files).filter(n => !zip.files[n].dir);
  const extractedPaths = [];
  const FS = getFS();

  ensureDir(extractDir);
  for (const fileName of fileNames) {
    const fileData = await zip.files[fileName].async('uint8array');
    const filePath = `${extractDir}/${sanitizeName(fileName)}`;
    ensureDirs(filePath);
    FS.writeFile(filePath, fileData);
    extractedPaths.push(filePath);
  }
  return extractedPaths;
}

// Extract a peeler-supported archive to a directory.
export async function extractPeelerToDir(archivePath, extractDir) {
  ensureDir(extractDir);
  const result = await window.runCommand(`peeler -o ${extractDir} ${archivePath}`);
  return result === 0;
}

// Probe if a file is a peeler-supported archive.
export async function probePeelerArchive(filePath) {
  try {
    const result = await window.runCommand(`peeler --probe ${filePath}`);
    return result === 0;
  } catch {
    return false;
  }
}

// Try to extract an archive file. Returns extraction info.
export async function tryExtractArchive(filePath, displayName, data) {
  const baseName = sanitizeName(displayName).replace(/\.(zip|sit|hqx|cpt|bin|sea)$/i, '');
  const extractDir = `${filePath.substring(0, filePath.lastIndexOf('/') + 1)}${baseName}_unpacked`;

  // Try ZIP extraction
  if (isZipFile(displayName)) {
    try {
      const paths = await extractZipToDir(data, extractDir);
      if (paths.length > 0) {
        toast(`Extracted ZIP to ${extractDir}`);
        return { extracted: true, extractDir };
      }
    } catch (err) {
      console.error('ZIP extraction failed:', err);
      toast(`Failed to extract ZIP; file kept at ${filePath}`);
    }
    return { extracted: false, extractDir: null };
  }

  // Try peeler extraction (probe first)
  if (await probePeelerArchive(filePath)) {
    toast(`Extracting ${displayName}...`);
    try {
      if (await extractPeelerToDir(filePath, extractDir)) {
        toast(`Extracted ${displayName} to ${extractDir}`);
        return { extracted: true, extractDir };
      }
    } catch (err) {
      console.error('Peeler extract failed:', err);
    }
    toast(`Failed to extract ${displayName}; file kept at ${filePath}`);
  }

  return { extracted: false, extractDir: null };
}

// --- Media probing ---

// Probe if a file is a valid ROM.
export async function probeRom(filePath) {
  try {
    const result = await window.runCommand(`load-rom --probe ${quotePath(filePath)}`);
    return result === 0;
  } catch {
    return false;
  }
}

// Probe if a file is a valid floppy disk image.
export async function probeFloppy(filePath) {
  try {
    const result = await window.runCommand(`insert-fd --probe ${quotePath(filePath)}`);
    return result === 0;
  } catch {
    return false;
  }
}

// Classify a file by probing it. Returns 'rom', 'floppy', or null.
export async function classifyMediaFile(filePath) {
  if (await probeRom(filePath)) return 'rom';
  if (await probeFloppy(filePath)) return 'floppy';
  return null;
}

// Search a directory for recognizable media files (ROM or floppy).
export async function findMediaInDirectory(dirPath) {
  const FS = getFS();
  try {
    const files = FS.readdir(dirPath).filter(n => n !== '.' && n !== '..');
    for (const fileName of files) {
      const fullPath = `${dirPath}/${fileName}`;
      try {
        const stat = FS.stat(fullPath);
        if (!FS.isFile(stat.mode)) continue;
        const kind = await classifyMediaFile(fullPath);
        if (kind) return { path: fullPath, kind };
      } catch {}
    }
  } catch {}
  return null;
}

// --- Checkpoint signature helpers ---

import { CHECKPOINT_MAGIC_BYTES } from './config.js';

// Check whether a Uint8Array begins with a checkpoint signature (v2 or v3)
export function bufferHasCheckpointSignature(buf) {
  if (!buf || buf.length < CHECKPOINT_MAGIC_BYTES.length) return false;
  // Compare the common prefix 'GSCHKPT' (7 chars), then accept '2' or '3'
  for (let i = 0; i < 7; i++) {
    if (buf[i] !== CHECKPOINT_MAGIC_BYTES[i]) return false;
  }
  // Accept version byte '2' (0x32) or '3' (0x33)
  return buf[7] === 0x32 || buf[7] === 0x33;
}

// Light-weight file-level probe for checkpoint signature
export async function fileHasCheckpointSignature(file) {
  if (!file || file.size < CHECKPOINT_MAGIC_BYTES.length) return false;
  const head = new Uint8Array(await file.slice(0, CHECKPOINT_MAGIC_BYTES.length).arrayBuffer());
  return bufferHasCheckpointSignature(head);
}
