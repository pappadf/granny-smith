// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Handles URL parameter media provisioning: fetches images from URL params,
// stores them in the persistent boot directory, and issues mount commands.
//
// With OPFS + pthreads, files written via FS.writeFile to /tmp/ (memory
// backend, cross-thread safe) are then copied to OPFS paths by C-side
// commands.  No explicit sync step needed.
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
// attach-hd gracefully fails for non-existent files, so just try each slot.
async function autoAttachPersistentHDs() {
  try {
    for (let i = 0; i < 8; i++) {
      const p = hdSlotPath(i);
      await window.runCommand(`attach-hd ${p} ${i}`);
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
// Stage to /tmp/ first (memory backend, cross-thread safe), then
// the C-side command copies to the OPFS-backed boot path.
async function fetchAndStore(slot, url) {
  const FS = getFS();
  try {
    console.log(`[url-media] fetchAndStore START: slot=${slot} url=${url}`);
    const res = await fetch(url);
    console.log(`[url-media] fetch response: slot=${slot} status=${res.status} ok=${res.ok}`);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    console.log(`[url-media] fetched ${slot}: ${buf.length} bytes, first4=[${buf[0]},${buf[1]},${buf[2]},${buf[3]}]`);
    const ct = res.headers.get('Content-Type') || '';
    const fileName = url.split('/').pop().split('?')[0] || slot;
    const isZip = /\.zip($|[?#])/i.test(url) || /zip/i.test(ct);
    const path = `${BOOT_DIR}/${slot}`;
    console.log(`[url-media] ${slot}: fileName=${fileName} isZip=${isZip} destPath=${path}`);

    // Stage to /tmp/ first, then write to persistent OPFS path via C command
    const tmpPath = `/tmp/url_${slot}`;

    if (isZip) {
      try {
        const JSZip = await getJSZip();
        const zip = await JSZip.loadAsync(buf);
        const fileNames = Object.keys(zip.files).filter(n => !zip.files[n].dir);
        if (!fileNames.length) throw new Error('empty zip (no files)');
        if (fileNames.length > 1) console.warn('[zip] multiple files present, using first', fileNames);
        const primary = fileNames[0];
        const inner = await zip.files[primary].async('uint8array');
        writeBinary(tmpPath, inner);
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
                writeBinary(tmpPath, imgData);
                toast(`${slot} downloaded (extracted:${foundImage.split('/').pop()})`);
              } else {
                writeBinary(tmpPath, buf);
                toast(`${slot} downloaded (archive extracted but no disk image found)`);
              }
            } else {
              writeBinary(tmpPath, buf);
              toast(`${slot} downloaded (extraction failed, using original)`);
            }
          } else {
            writeBinary(tmpPath, buf);
            toast(`${slot} downloaded`);
          }
        } catch (err) {
          console.warn('Peeler processing failed, storing original', err);
          writeBinary(tmpPath, buf);
          toast(`${slot} downloaded`);
        }
      } else {
        console.log(`[url-media] ${slot}: writing ${buf.length} bytes to ${tmpPath} (plain binary)`);
        writeBinary(tmpPath, buf);
        toast(`${slot} downloaded`);
      }
    }

    // Copy from /tmp/ staging to persistent OPFS path via C-side file copy.
    // The worker thread can access both /tmp/ (memory) and /boot/ (OPFS).
    await window.runCommand(`file-copy ${tmpPath} ${path}`);
  } catch (e) {
    console.error(`[url-media] Download FAILED for ${slot}:`, e);
    toast(`${slot} failed`);
  }
}

// Process all URL-parameter media (rom, vrom, fd0..fdN, hd0..hdN).
// Ordering: download all → load ROM (creates machine) → insert floppies → attach HDs → run.
// With deferred boot, the machine doesn't exist until load-rom is called, so floppy
// insertion must happen after ROM loading.
export async function processUrlMedia(params) {
  console.log(`[url-media] processUrlMedia called. params:`, [...params.entries()]);
  const downloads = [];
  // ROM first (overwrite precedence rule)
  if (params.has('rom')) downloads.push(fetchAndStore('rom', params.get('rom')));
  // Video ROM (stored as SE30.vrom next to ROM so se30_load_vrom finds it)
  if (params.has('vrom')) downloads.push(fetchAndStore('SE30.vrom', params.get('vrom')));
  // Floppies
  for (const [k, v] of params.entries()) if (/^fd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  // Hard disks
  for (const [k, v] of params.entries()) if (/^hd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  console.log(`[url-media] waiting for ${downloads.length} downloads...`);
  await Promise.all(downloads);
  console.log(`[url-media] all downloads complete`);

  if (params.has('rom')) {
    // Load ROM first — this creates the machine via rom_identify + system_ensure_machine
    const path = `${BOOT_DIR}/rom`;
    console.log(`[url-media] loading ROM from: ${path}`);
    await window.runCommand(`load-rom ${path}`);
    romLoaded = true;
    hideRomOverlay();
    enableRunButton();

    // Now that the machine exists, insert floppies
    for (const [k] of params.entries()) {
      if (/^fd\d+$/.test(k)) {
        const fdPath = `${BOOT_DIR}/${k}`;
        console.log(`[url-media] inserting floppy: ${fdPath}`);
        const rc = await window.runCommand(`insert-fd ${fdPath} 0 1`);
        console.log(`[url-media] insert-fd result: ${rc}`);
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
