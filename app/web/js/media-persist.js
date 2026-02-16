// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Ensures mounted disk images are persisted to IDBFS before the C core opens them.
// Intercepts insert-fd / attach-hd commands and copies volatile images
// (those under /tmp/ or /fd/) to /persist/images/<hash>.img, then rewrites
// the command to use the persistent path. Content-addressed naming avoids duplicates.
import { getFS, ensureDir, persistSync } from './fs.js';
import { IMAGES_DIR } from './config.js';
import { registerCommandPreprocessor } from './emulator.js';

// Check if a filesystem path resides in volatile (non-persistent) storage.
export function isVolatile(path) {
  return path.startsWith('/tmp/') || path.startsWith('/fd/');
}

// FNV-1a hash over the first 64 KB of data plus total file size → 8-char hex.
// Not cryptographic — just fast with few collisions for typical disk images.
function hashImage(data) {
  let h = 0x811c9dc5 | 0;
  const len = Math.min(data.length, 65536);
  for (let i = 0; i < len; i++) {
    h ^= data[i];
    h = Math.imul(h, 0x01000193);
  }
  // mix in total file size to distinguish images with identical 64 KB prefixes
  h ^= data.length;
  h = Math.imul(h, 0x01000193);
  return (h >>> 0).toString(16).padStart(8, '0');
}

// Copy a volatile image to /persist/images/<hash>.img.
// Returns the persistent path. Skips the copy if the hash file already exists.
export function persistImage(volatilePath) {
  if (!isVolatile(volatilePath)) return volatilePath;

  const FS = getFS();
  if (!FS) return volatilePath;

  ensureDir(IMAGES_DIR);

  const data = FS.readFile(volatilePath);
  const hash = hashImage(data);
  const destPath = `${IMAGES_DIR}/${hash}.img`;

  // skip copy if identical image already persisted
  try {
    FS.stat(destPath);
    return destPath; // already exists
  } catch (_) { /* does not exist yet */ }

  FS.writeFile(destPath, data);
  console.log(`[media-persist] copied ${volatilePath} → ${destPath}`);
  return destPath;
}

// Intercept insert-fd and attach-hd commands to persist volatile images.
// Returns the (possibly rewritten) command string.
async function preprocessCommand(cmd) {
  const trimmed = (cmd || '').trim();

  // insert-fd [path] [drive] [writable] — skip --probe and other flag variants
  const fdMatch = trimmed.match(/^(insert-fd\s+)(?!"?--)(?:"([^"]+)"|(\S+))(\s+\d+)?(\s+\d+)?$/);
  if (fdMatch) {
    const prefix = fdMatch[1];
    const filePath = fdMatch[2] || fdMatch[3];
    const driveSuffix = fdMatch[4] || '';
    const writableSuffix = fdMatch[5] || '';
    if (isVolatile(filePath)) {
      const newPath = persistImage(filePath);
      await persistSync();
      return `${prefix}${newPath}${driveSuffix}${writableSuffix}`;
    }
    return cmd;
  }

  // attach-hd [path] [scsi-id]
  const hdMatch = trimmed.match(/^(attach-hd\s+)(?:"([^"]+)"|(\S+))\s+(\d+)$/);
  if (hdMatch) {
    const prefix = hdMatch[1];
    const filePath = hdMatch[2] || hdMatch[3];
    const scsiId = hdMatch[4];
    if (isVolatile(filePath)) {
      const newPath = persistImage(filePath);
      await persistSync();
      return `${prefix}${newPath} ${scsiId}`;
    }
    return cmd;
  }

  return cmd;
}

// Register the command preprocessor to intercept mount commands.
export function initMediaPersist() {
  registerCommandPreprocessor(preprocessCommand);
}
