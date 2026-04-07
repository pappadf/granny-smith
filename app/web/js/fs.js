// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Filesystem helpers for the WasmFS + OPFS backend.
//
// With PROXY_TO_PTHREAD, the OPFS-backed mounts (/images, /checkpoints, /upload)
// are only accessible from the worker thread.  JS on the main thread can use
// FS.writeFile/FS.unlink on /tmp/ (memory backend), but mkdir/readFile/stat/
// readdir/rmdir fail cross-thread for OPFS paths.
//
// Persistent file operations should go through C-side shell commands.
import { ROMS_DIR, CHECKPOINT_DIR } from './config.js';

let FS = null;

// Capture the Module.FS reference after the module is ready.
export function initFS(module) {
  FS = module.FS;
}

// Raw FS reference (for callers that need direct API access on /tmp/).
export function getFS() { return FS; }

// Ensure a single directory exists under /tmp/ (memory backend, cross-thread safe).
export function ensureDir(path) {
  if (!FS) return;
  try { FS.mkdir(path); } catch (_) {}
}

// Ensure directory structure exists (POSIX style stepwise mkdir).
// Only reliable for /tmp/ paths (memory backend).
export function ensureDirs(path) {
  if (!FS) return;
  let cur = '';
  for (const part of path.split('/').slice(1, -1)) {
    cur += '/' + part;
    try { FS.mkdir(cur); } catch (_) {}
  }
}

// Write (overwrite) a file (Uint8Array | ArrayBuffer) to FS.
// Reliable for /tmp/ paths.  For OPFS paths, use C-side commands.
export function writeBinary(path, data) {
  if (!FS) return;
  ensureDirs(path);
  try { FS.unlink(path); } catch (_) {}
  FS.writeFile(path, data instanceof Uint8Array ? data : new Uint8Array(data));
}

// Check if a file exists.
// Reliable for /tmp/ paths.  For OPFS paths, use C-side commands.
export function fileExists(p) {
  if (!FS) return false;
  try { FS.stat(p); return true; } catch (_) { return false; }
}

// Remove all files inside a directory (ignores missing or empty directories).
export function clearDirContents(dirPath) {
  if (!FS) return;
  try {
    for (const name of FS.readdir(dirPath)) {
      if (name === '.' || name === '..') continue;
      try { FS.unlink(`${dirPath}/${name}`); } catch (_) {}
    }
  } catch (_) {}
}

// List files in an OPFS directory via the browser's async OPFS API.
// dirPath must be under /opfs/ (e.g. '/opfs/images/fd').
// Returns an array of { name, kind } where kind is 'file' or 'directory'.
export async function listDir(dirPath) {
  try {
    // Strip the /opfs/ prefix to get the OPFS-relative path
    const relPath = dirPath.replace(/^\/opfs\/?/, '');
    let dir = await navigator.storage.getDirectory();
    if (relPath) {
      for (const part of relPath.split('/')) {
        dir = await dir.getDirectoryHandle(part);
      }
    }
    const entries = [];
    for await (const [name, handle] of dir.entries()) {
      entries.push({ name, kind: handle.kind });
    }
    return entries;
  } catch (_) {
    return [];
  }
}

// ROM existence checks — route through C-side commands since /images is OPFS
export async function romExistsAsync() {
  return (await window.runCommand('rom --probe')) === 0;
}
// Synchronous fallback (unreliable for OPFS paths — prefer romExistsAsync)
export function romExists() {
  // This is unreliable for OPFS. Callers should migrate to romExistsAsync.
  return false;
}

// Checksum-based ROM storage paths
export function romsDir() { return ROMS_DIR; }
export function romPathForChecksum(checksum) { return `${ROMS_DIR}/${checksum.toUpperCase()}`; }

// Ensure the checkpoint directory tree exists.
export function ensureCheckpointDir() {
  // /checkpoints is an OPFS mount — it exists from main().
  // Nothing to do here.
}
