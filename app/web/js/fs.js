// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Manages the IDBFS persistent filesystem layer.
// Initialized with the Emscripten Module; all FS access goes through this module.
import { BOOT_DIR, ROMS_DIR, CHECKPOINT_DIR } from './config.js';

let FS = null;
let IDBFS = null;

// Initialize the filesystem: mount IDBFS at /persist, return a sync promise.
export function initFS(module) {
  FS = module.FS;
  IDBFS = FS?.filesystems?.IDBFS;

  // Create /persist mount point
  try { FS.mkdir('/persist'); } catch (_) {}
  if (IDBFS) {
    try { FS.mount(IDBFS, {}, '/persist'); } catch (e) { console.warn('[IDBFS] mount failed', e); }
  } else {
    console.warn('[IDBFS] not available (media persistence disabled)');
  }

  // Promise that resolves after the initial sync-from-IDB is done.
  const initialSync = new Promise(res => {
    if (!IDBFS) return res();
    FS.syncfs(true, (err) => {
      if (err) console.error('[IDBFS] initial sync error', err);
      else console.log('[IDBFS] initial sync complete');
      res();
    });
  });

  // Opportunistically request persistent storage (browser may ignore silently).
  if (navigator.storage?.persist) navigator.storage.persist();

  // Sync back to IndexedDB when we leave.
  addEventListener('pagehide', () => { try { FS.syncfs(false, () => {}); } catch (_) {} });

  return initialSync;
}

// Raw FS reference (for advanced callers that need direct API access).
export function getFS() { return FS; }

// Ensure directory structure exists (POSIX style stepwise mkdir).
export function ensureDirs(path) {
  let cur = '';
  for (const part of path.split('/').slice(1, -1)) {
    cur += '/' + part;
    try { FS.mkdir(cur); } catch (_) {}
  }
}

// Ensure a single directory exists (swallows EEXIST).
export function ensureDir(path) {
  try { FS.mkdir(path); } catch (_) {}
}

// Write (overwrite) a file (Uint8Array | ArrayBuffer) to FS.
export function writeBinary(path, data) {
  ensureDirs(path);
  try { FS.unlink(path); } catch (_) {}
  FS.writeFile(path, data instanceof Uint8Array ? data : new Uint8Array(data));
}

// Check if a file exists.
export function fileExists(p) {
  try { FS.stat(p); return true; } catch (_) { return false; }
}

// Convenience path helpers
export function romPath() { return `${BOOT_DIR}/rom`; }
export function floppySlotPath(idx) { return `${BOOT_DIR}/fd${idx}`; }
export function hdSlotPath(idx) { return `${BOOT_DIR}/hd${idx}`; }
export function romExists() { return latestRomExists() || fileExists(romPath()); }

// Checksum-based ROM storage paths
export function romsDir() { return ROMS_DIR; }
export function romPathForChecksum(checksum) { return `${ROMS_DIR}/${checksum.toUpperCase()}`; }
export function latestRomPath() { return `${ROMS_DIR}/latest`; }
export function latestRomExists() { return fileExists(latestRomPath()); }

// Ensure the checkpoint directory tree exists.
export function ensureCheckpointDir() {
  // ensureDirs creates all parent directories except the final segment, so
  // using a placeholder file path creates the CHECKPOINT_DIR itself.
  ensureDirs(`${CHECKPOINT_DIR}/placeholder`);
}

// Persist sync helpers — delegate to C-side `sync` command (single source of truth)
export function persistSync() { return window.runCommand('sync wait'); }
export function syncFromPersist() { return window.runCommand('sync from-persist wait'); }
