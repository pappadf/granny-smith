// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Handles URL parameter media provisioning: fetches images from URL params,
// stores them in the persistent image directories, and issues mount commands.
//
// With OPFS + pthreads, files written via FS.writeFile to /tmp/ (memory
// backend, cross-thread safe) are then copied to OPFS paths by C-side
// commands.  No explicit sync step needed.
import { ROMS_DIR, VROMS_DIR, FD_DIR, FDHD_DIR, HD_DIR } from './config.js';
import { ensureDir, writeBinary, fileExists, getFS } from './fs.js';
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

// Track whether a ROM has already been loaded and run.
let romLoaded = false;

// Returns true if a ROM has already been loaded via loadRomAndMaybeRun.
export function isRomLoaded() { return romLoaded; }

// Load ROM from persistent storage and start the emulator.
// Lists ROMs in /images/rom/ and loads the first one found.
export async function loadRomAndMaybeRun() {
  // List ROMs via ls command (OPFS accessible only from worker). The ls
  // command currently has no typed-return wrapper — its stdout-only
  // output stays on the legacy bridge until a future `storage.list`
  // method lands.
  const lsResult = await window.runCommand(`ls ${ROMS_DIR}`);
  // If ls returns non-zero or no output, no ROM available
  if (lsResult !== 0) { showRomOverlay(); return; }

  // The ls command printed filenames to stdout; we can't easily capture them.
  // Instead, try rom_probe with no args to check if a ROM is loaded.
  if ((await window.gsEval('rom_probe')) === true) {
    // ROM already loaded (from a previous session or checkpoint)
    romLoaded = true;
    hideRomOverlay();
    enableRunButton();
    await window.gsEval('run');
    return;
  }

  showRomOverlay();
}

// Determine the target directory for a media slot
function targetDir(slot) {
  if (slot === 'rom') return ROMS_DIR;
  if (slot.startsWith('vrom') || slot === 'SE30.vrom') return VROMS_DIR;
  if (slot.startsWith('fd')) return FD_DIR;
  if (slot.startsWith('hd')) return HD_DIR;
  return '/tmp';
}

// Fetch and store a media file from a URL into the appropriate image directory.
// Stage to /tmp/ first (memory backend, cross-thread safe), then
// the C-side command copies to the OPFS-backed path.
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
    const dir = targetDir(slot);
    const destName = slot === 'rom' ? fileName : (slot.startsWith('SE30') ? slot : fileName);
    const path = `${dir}/${sanitizeName(destName)}`;
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
        const tempPath = `/tmp/${slot}_download`;
        writeBinary(tempPath, buf);

        try {
          if ((await window.gsEval('peeler_probe', [tempPath])) === true) {
            const extractDir = `/tmp/${slot}_unpacked`;
            toast(`Extracting ${fileName}...`);

            if ((await window.gsEval('peeler', [tempPath, extractDir])) === true) {
              if ((await window.gsEval('find_media', [extractDir, tmpPath])) === true) {
                toast(`${slot} downloaded (extracted from archive)`);
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

    // Copy from /tmp/ staging to persistent OPFS path.
    await window.gsEval('cp', [tmpPath, path]);
  } catch (e) {
    console.error(`[url-media] Download FAILED for ${slot}:`, e);
    toast(`${slot} failed`);
  }
}

// Process all URL-parameter media (rom, vrom, fd0..fdN, hd0..hdN).
// Ordering: download all → load ROM (creates machine) → insert floppies → attach HDs → run.
export async function processUrlMedia(params) {
  console.log(`[url-media] processUrlMedia called. params:`, [...params.entries()]);
  const downloads = [];
  // ROM first
  if (params.has('rom')) downloads.push(fetchAndStore('rom', params.get('rom')));
  // Video ROM
  if (params.has('vrom')) downloads.push(fetchAndStore('SE30.vrom', params.get('vrom')));
  // Floppies
  for (const [k, v] of params.entries()) if (/^fd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  // Hard disks
  for (const [k, v] of params.entries()) if (/^hd\d+$/.test(k)) downloads.push(fetchAndStore(k, v));
  console.log(`[url-media] waiting for ${downloads.length} downloads...`);
  await Promise.all(downloads);
  console.log(`[url-media] all downloads complete`);

  if (params.has('rom')) {
    // Load ROM — this creates the machine via rom_identify + system_ensure_machine
    // The ROM was stored under /images/rom/<filename>; use the tmp path directly
    const tmpPath = '/tmp/url_rom';
    console.log(`[url-media] loading ROM from: ${tmpPath}`);
    await window.gsEval('rom_load', [tmpPath]);
    romLoaded = true;
    hideRomOverlay();
    enableRunButton();

    // Now that the machine exists, insert floppies
    for (const [k] of params.entries()) {
      if (/^fd\d+$/.test(k)) {
        const fdPath = `/tmp/url_${k}`;
        console.log(`[url-media] inserting floppy: ${fdPath}`);
        const ok = await window.gsEval('fd_insert', [fdPath, 0, true]);
        console.log(`[url-media] fd insert result: ${ok}`);
      }
    }

    // Attach HDs
    for (const [k] of params.entries()) {
      if (/^hd\d+$/.test(k)) {
        const hdPath = `/tmp/url_${k}`;
        const id = parseInt(k.replace('hd', ''), 10);
        await window.gsEval('hd_attach', [hdPath, id]);
      }
    }

    await window.gsEval('run');
  } else {
    // No ROM in URL params — insert floppies if machine already exists
    for (const [k] of params.entries()) {
      if (/^fd\d+$/.test(k)) {
        await window.gsEval('fd_insert', [`/tmp/url_${k}`, 0, true]);
      }
    }
  }
}
