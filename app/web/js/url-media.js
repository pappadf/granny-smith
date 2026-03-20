// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Handles URL parameter media provisioning: fetches images from URL params,
// stores them in the persistent boot directory, and issues mount commands.
import { BOOT_DIR } from './config.js';
import { ensureDir, writeBinary, romPath, romExists, latestRomPath, latestRomExists, fileExists, hdSlotPath, getFS } from './fs.js';
import { isModuleReady } from './emulator.js';
import { toast, showRomOverlay, hideRomOverlay, enableRunButton } from './ui.js';
import { sanitizeName } from './media.js';

// --- JSZip lazy singleton (shared with media.js but loaded independently) ---
let _jszip = null;
async function getJSZip() {
  if (!_jszip) {
    const { default: JSZip } = await import('https://cdn.jsdelivr.net/npm/jszip@3.10.1/+esm');
    _jszip = JSZip;
  }
  return _jszip;
}

// Allocate the next free HD slot index (0–7).
function allocateHdSlot() {
  for (let i = 0; i < 8; i++) { if (!fileExists(hdSlotPath(i))) return i; }
  return 0;
}

// Auto-attach any hdN files present in the persistent boot directory before run.
async function autoAttachPersistentHDs() {
  try {
    for (let i = 0; i < 8; i++) {
      const p = hdSlotPath(i);
      if (fileExists(p)) {
        await window.runCommand(`attach-hd ${p} ${i}`);
      }
    }
  } catch (e) { console.warn('autoAttachPersistentHDs failed', e); }
}

// Track whether a ROM has already been loaded and run.
let romLoaded = false;

// Returns true if a ROM has already been loaded via loadRomAndMaybeRun.
export function isRomLoaded() { return romLoaded; }

// Load ROM from persistent storage and start the emulator.
// Prefers the new checksum-based latest path, falls back to legacy path.
export async function loadRomAndMaybeRun() {
  if (!romExists()) { showRomOverlay(); return; }
  const path = latestRomExists() ? latestRomPath() : romPath();
  await window.runCommand(`load-rom ${path}`);
  romLoaded = true;
  hideRomOverlay();
  enableRunButton();
  await autoAttachPersistentHDs();
  await window.runCommand('run');
}

// Fetch and store a media file from a URL into the boot directory.
async function fetchAndStore(slot, url) {
  const FS = getFS();
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    const ct = res.headers.get('Content-Type') || '';
    const fileName = url.split('/').pop().split('?')[0] || slot;
    const isZip = /\.zip($|[?#])/i.test(url) || /zip/i.test(ct);
    const path = `${BOOT_DIR}/${slot}`;

    if (isZip) {
      try {
        const JSZip = await getJSZip();
        const zip = await JSZip.loadAsync(buf);
        const fileNames = Object.keys(zip.files).filter(n => !zip.files[n].dir);
        if (!fileNames.length) throw new Error('empty zip (no files)');
        if (fileNames.length > 1) console.warn('[zip] multiple files present, using first', fileNames);
        const primary = fileNames[0];
        const inner = await zip.files[primary].async('uint8array');
        writeBinary(path, inner);
        toast(`${slot} downloaded (zip:${primary})`);
      } catch (zErr) {
        console.error('Download failed (zip extract)', slot, zErr);
        toast(`${slot} failed`);
        return;
      }
    } else {
      // Check if it might be a peeler-supported archive
      const isPeelerArch = /\.(sit|hqx|cpt|bin|sea)(_|$)/i.test(fileName);

      if (isPeelerArch && isModuleReady()) {
        ensureDir('/tmp');
        const tempPath = `/tmp/${slot}_download`;
        writeBinary(tempPath, buf);

        try {
          const probeResult = await window.runCommand(`peeler --probe ${tempPath}`);

          if (probeResult === 0) {
            const extractDir = `/tmp/${slot}_unpacked`;
            ensureDir(extractDir);
            toast(`Extracting ${fileName}...`);

            const extractResult = await window.runCommand(`peeler -o ${extractDir} ${tempPath}`);
            if (extractResult === 0) {
              toast(`Extracted ${fileName}`);

              const files = FS.readdir(extractDir).filter(n => n !== '.' && n !== '..');
              let foundImage = null;
              for (const f of files) {
                const fullPath = `${extractDir}/${f}`;
                try {
                  const stat = FS.stat(fullPath);
                  if (FS.isFile(stat.mode)) {
                    const qp = (p) => `"${p.replace(/"/g, '\\"')}"`;
                    const probeImg = await window.runCommand(`insert-fd --probe ${qp(fullPath)}`);
                    if (probeImg === 0) {
                      foundImage = fullPath;
                      break;
                    }
                  }
                } catch (_) {}
              }

              if (foundImage) {
                const imgData = FS.readFile(foundImage);
                writeBinary(path, imgData);
                toast(`${slot} downloaded (extracted:${foundImage.split('/').pop()})`);
              } else {
                writeBinary(path, buf);
                toast(`${slot} downloaded (archive extracted but no disk image found)`);
              }
            } else {
              writeBinary(path, buf);
              toast(`${slot} downloaded (extraction failed, using original)`);
            }
          } else {
            writeBinary(path, buf);
            toast(`${slot} downloaded`);
          }
        } catch (err) {
          console.warn('Peeler processing failed, storing original', err);
          writeBinary(path, buf);
          toast(`${slot} downloaded`);
        }
      } else {
        writeBinary(path, buf);
        toast(`${slot} downloaded`);
      }
    }
  } catch (e) {
    console.error('Download failed', slot, e);
    toast(`${slot} failed`);
  }
}

// Process all URL-parameter media (rom, fd0..fdN, hd0..hdN).
// Ordering: download all → load ROM (creates machine) → insert floppies → attach HDs → run.
// With deferred boot, the machine doesn't exist until load-rom is called, so floppy
// insertion must happen after ROM loading.
export async function processUrlMedia(params) {
  const downloads = [];
  // ROM first (overwrite precedence rule)
  if (params.has('rom')) downloads.push(fetchAndStore('rom', params.get('rom')));
  // Floppies
  for (const [k, v] of params.entries()) if (/^fd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  // Hard disks
  for (const [k, v] of params.entries()) if (/^hd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  await Promise.all(downloads);

  if (params.has('rom')) {
    // Load ROM first — this creates the machine via rom_identify + system_ensure_machine
    if (!romExists()) { return; }
    const path = latestRomExists() ? latestRomPath() : romPath();
    await window.runCommand(`load-rom ${path}`);
    romLoaded = true;
    hideRomOverlay();
    enableRunButton();

    // Now that the machine exists, insert floppies
    for (const [k] of params.entries()) {
      if (/^fd\d+$/.test(k)) {
        await window.runCommand(`insert-fd ${BOOT_DIR}/${k} 0 1`);
      }
    }

    // Attach persistent HDs and start
    await autoAttachPersistentHDs();
    await window.runCommand('run');
  } else {
    // No ROM in URL params — insert floppies if machine already exists
    for (const [k] of params.entries()) {
      if (/^fd\d+$/.test(k)) {
        await window.runCommand(`insert-fd ${BOOT_DIR}/${k} 0 1`);
      }
    }
  }
}
