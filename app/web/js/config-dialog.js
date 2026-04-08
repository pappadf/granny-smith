// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Machine configuration dialog: lets the user pick model, RAM, media, and boot.
// Also provides the ROM-required dialog for first-time setup.
import { VROMS_DIR, FD_DIR, FDHD_DIR, HD_DIR, CD_DIR } from './config.js';
import { ROM_DATABASE, getAvailableModels } from './rom-db.js';
import { MACHINE_DEFS } from './machine-defs.js';
import { romPathForChecksum, listDir } from './fs.js';
import { toast, hideRomOverlay, enableRunButton, setBackgroundMessage } from './ui.js';
import { setRunning } from './emulator.js';

// ---------------------------------------------------------------------------
// OPFS ROM scanning — probe known checksums to find persisted ROMs
// ---------------------------------------------------------------------------

// Scan /rom/ for ROM files by probing all known checksums from ROM_DATABASE.
// Returns an array of checksum strings that exist on disk.
export async function scanForPersistedRoms() {
  const found = [];
  for (const cs of Object.keys(ROM_DATABASE)) {
    const path = romPathForChecksum(cs);
    const rc = await window.runCommand(`rom --probe ${path}`);
    if (rc === 0) {
      found.push(cs);
    }
  }
  return found;
}

// ---------------------------------------------------------------------------
// ROM Upload Dialog (first-time experience)
// ---------------------------------------------------------------------------

// Show a modal dialog asking the user to upload a ROM file.
// Returns { checksum, tmpPath } on success, or null if cancelled.
export function showRomUploadDialog() {
  return new Promise((resolve) => {
    let dlg = document.getElementById('rom-upload-dialog');
    if (!dlg) {
      dlg = document.createElement('div');
      dlg.id = 'rom-upload-dialog';
      dlg.className = 'modal';
      dlg.setAttribute('role', 'dialog');
      dlg.setAttribute('aria-modal', 'true');
      dlg.setAttribute('aria-hidden', 'true');
      dlg.innerHTML = `
        <div class="modal__content rom-upload-modal">
          <h2 class="modal__title">Welcome to Granny Smith</h2>
          <p class="modal__message">Upload a Macintosh ROM image to get started.</p>
          <div class="modal__actions" style="flex-direction:column; align-items:center; gap:0.5rem;">
            <button class="primary-btn" id="rom-upload-select-btn">Select ROM File...</button>
            <input id="rom-upload-file-input" type="file" style="display:none" />
            <span class="rom-upload-hint">or drag & drop a ROM file onto the screen</span>
            <span class="rom-upload-error" id="rom-upload-error" hidden></span>
          </div>
        </div>
      `;
      document.body.appendChild(dlg);
    }

    const selectBtn = dlg.querySelector('#rom-upload-select-btn');
    const fileInput = dlg.querySelector('#rom-upload-file-input');
    const errorSpan = dlg.querySelector('#rom-upload-error');

    errorSpan.hidden = true;

    const onFile = async () => {
      const file = fileInput.files?.[0];
      if (!file) return;

      const data = new Uint8Array(await file.arrayBuffer());
      const { writeBinary } = await import('./fs.js');
      const tmpPath = `/tmp/upload_rom_${Date.now()}`;
      writeBinary(tmpPath, data);

      // Validate
      const rc = await window.runCommand(`rom --checksum ${tmpPath}`);
      if (rc !== 0) {
        errorSpan.textContent = 'Not a valid Macintosh ROM image. Please try another file.';
        errorSpan.hidden = false;
        fileInput.value = '';
        return;
      }

      // Extract checksum from header
      const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
      const checksum = dv.getUint32(0, false).toString(16).toUpperCase().padStart(8, '0');

      // Persist to /rom/<checksum> (best-effort)
      await window.runCommand(`file-copy ${tmpPath} ${romPathForChecksum(checksum)}`);

      dlg.setAttribute('aria-hidden', 'true');
      fileInput.value = '';
      resolve({ checksum, tmpPath });
    };

    selectBtn.addEventListener('click', () => fileInput.click());
    fileInput.addEventListener('change', onFile);

    dlg.setAttribute('aria-hidden', 'false');
  });
}

// ---------------------------------------------------------------------------
// Machine Configuration Dialog
// ---------------------------------------------------------------------------

// Build the configuration dialog UI.
// Returns a Promise that resolves with the user's configuration choices.
export async function showConfigDialog(romChecksums) {
  return new Promise(async (resolve) => {
    const models = getAvailableModels(romChecksums);
    const modelIds = [...models.keys()];

    let dlg = document.getElementById('config-dialog');
    if (!dlg) {
      dlg = document.createElement('div');
      dlg.id = 'config-dialog';
      dlg.className = 'modal';
      dlg.setAttribute('role', 'dialog');
      dlg.setAttribute('aria-modal', 'true');
      dlg.setAttribute('aria-hidden', 'true');
      dlg.innerHTML = `
        <div class="modal__content config-modal">
          <h2 class="modal__title">Machine Configuration</h2>
          <div class="config-rows" id="config-rows"></div>
          <div class="modal__actions">
            <button class="primary-btn" id="config-start-btn">Start</button>
          </div>
        </div>
      `;
      document.body.appendChild(dlg);
    }

    const rowsContainer = dlg.querySelector('#config-rows');

    // Filter listDir results to actual image files (skip .delta, .journal, directories).
    function imageFiles(entries) {
      return entries
        .filter(e => e.kind === 'file' && !e.name.endsWith('.delta') && !e.name.endsWith('.journal'))
        .map(e => e.name);
    }

    // Build options list from persisted files in a directory + upload option.
    function mediaOptions(files, dir) {
      const opts = [{ value: '', label: '(none)' }];
      for (const name of files) {
        opts.push({ value: `${dir}/${name}`, label: name });
      }
      opts.push({ value: '__divider__', label: '───────────', disabled: true });
      opts.push({ value: '__upload__', label: 'Upload image...' });
      return opts;
    }

    async function buildRows(selectedModelId) {
      const def = MACHINE_DEFS[selectedModelId];
      if (!def) return;

      rowsContainer.innerHTML = '';

      // Row 1: Machine Model
      addRow('Machine Model:', buildModelSelect(selectedModelId));

      // Row 2: Video ROM (SE/30 only)
      if (def.hasVrom) {
        const vromFiles = imageFiles(await listDir(VROMS_DIR));
        addRow('Video ROM:', buildMediaSelect('config-vrom',
          mediaOptions(vromFiles, VROMS_DIR), VROMS_DIR));
      }

      // Row 3: RAM Size
      const ramOptions = def.ramOptions.map(mb => ({
        value: String(mb),
        label: `${mb} MB`,
        selected: mb === def.defaultRam,
      }));
      addRow('RAM Size:', buildSelect('config-ram', ramOptions));

      // Scan OPFS directories for persisted media (via browser OPFS API)
      const fdFiles = imageFiles(await listDir(FD_DIR));
      const fdhdFiles = imageFiles(await listDir(FDHD_DIR));
      const hdFiles = imageFiles(await listDir(HD_DIR));
      const cdFiles = imageFiles(await listDir(CD_DIR));

      // Combine floppy dirs based on machine capabilities
      const floppyFiles = def.floppyDirs.includes('fdhd')
        ? [...fdFiles.map(f => ({ name: f, dir: FD_DIR })),
           ...fdhdFiles.map(f => ({ name: f, dir: FDHD_DIR }))]
        : fdFiles.map(f => ({ name: f, dir: FD_DIR }));

      // Floppy rows
      const defaultFloppyDir = def.floppyDirs.includes('fdhd') ? FDHD_DIR : FD_DIR;
      for (let i = 0; i < def.floppySlots.length; i++) {
        const opts = [{ value: '', label: '(none)' }];
        for (const { name, dir } of floppyFiles) {
          opts.push({ value: `${dir}/${name}`, label: name });
        }
        opts.push({ value: '__divider__', label: '───────────', disabled: true });
        opts.push({ value: '__upload__', label: 'Upload image...' });
        addRow(`${def.floppySlots[i]}:`, buildMediaSelect(`config-fd${i}`, opts, defaultFloppyDir));
      }

      // SCSI HD rows
      for (let i = 0; i < def.scsiSlots.length; i++) {
        addRow(`${def.scsiSlots[i]}:`, buildMediaSelect(`config-hd${i}`, mediaOptions(hdFiles, HD_DIR), HD_DIR));
      }

      // CD-ROM row
      if (def.hasCdrom) {
        addRow('SCSI CD:', buildMediaSelect('config-cd', mediaOptions(cdFiles, CD_DIR), CD_DIR));
      }
    }

    function addRow(label, selectEl) {
      const row = document.createElement('div');
      row.className = 'config-row';
      const lbl = document.createElement('label');
      lbl.className = 'config-label';
      lbl.textContent = label;
      row.appendChild(lbl);
      row.appendChild(selectEl);
      rowsContainer.appendChild(row);
    }

    function buildModelSelect(selectedId) {
      const options = modelIds.map(id => ({
        value: id,
        label: MACHINE_DEFS[id]?.displayName || id,
        selected: id === selectedId,
      }));
      // Add divider and upload option
      options.push({ value: '__divider__', label: '───────────', disabled: true });
      options.push({ value: '__upload__', label: 'Upload another ROM...' });
      const sel = buildSelect('config-model', options);
      sel.addEventListener('change', () => {
        if (sel.value === '__upload__') {
          triggerRomUpload().then(newCs => {
            if (newCs) {
              romChecksums.push(newCs);
              const newModels = getAvailableModels(romChecksums);
              const newIds = [...newModels.keys()];
              modelIds.length = 0;
              modelIds.push(...newIds);
              buildRows(newIds.includes(sel._prevValue) ? sel._prevValue : newIds[0]);
            } else {
              sel.value = sel._prevValue || modelIds[0];
            }
          });
          return;
        }
        sel._prevValue = sel.value;
        buildRows(sel.value);
      });
      sel._prevValue = selectedId;
      return sel;
    }

    function buildSelect(id, options) {
      const sel = document.createElement('select');
      sel.id = id;
      sel.className = 'config-select';
      for (const opt of options) {
        const o = document.createElement('option');
        o.value = opt.value;
        o.textContent = opt.label;
        if (opt.selected) o.selected = true;
        if (opt.disabled) o.disabled = true;
        sel.appendChild(o);
      }
      return sel;
    }

    // Build a media dropdown with an "Upload image..." option.
    // persistDir is the OPFS directory to persist uploaded files to (e.g. /images/fd).
    function buildMediaSelect(id, options, persistDir) {
      const sel = buildSelect(id, options);
      sel.addEventListener('change', () => {
        if (sel.value !== '__upload__') return;
        triggerMediaUpload(persistDir).then(result => {
          if (result) {
            const newOpt = document.createElement('option');
            newOpt.value = result.path;
            newOpt.textContent = result.name;
            const divider = sel.querySelector('option[value="__divider__"]');
            if (divider) {
              sel.insertBefore(newOpt, divider);
            } else {
              sel.insertBefore(newOpt, sel.lastChild);
            }
            sel.value = result.path;
          } else {
            sel.value = '';
          }
        });
      });
      return sel;
    }

    // Upload a media file and persist it to the given OPFS directory.
    // Returns { path, name } where path is the persisted OPFS path.
    async function triggerMediaUpload(persistDir) {
      return new Promise((res) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.addEventListener('change', async () => {
          const file = input.files?.[0];
          if (!file) return res(null);
          const data = new Uint8Array(await file.arrayBuffer());
          const { writeBinary, ensureDir } = await import('./fs.js');
          const { sanitizeName } = await import('./media.js');
          const safeName = sanitizeName(file.name) || 'image.img';

          // Stage to /tmp first (memory backend, cross-thread safe)
          const tmpPath = `/tmp/${safeName}`;
          ensureDir('/tmp');
          writeBinary(tmpPath, data);

          // Persist to OPFS via file-copy (runs on worker where OPFS is accessible)
          const persistPath = `${persistDir}/${safeName}`;
          const rc = await window.runCommand(`file-copy ${tmpPath} ${persistPath}`);
          const finalPath = (rc === 0) ? persistPath : tmpPath;

          toast(`${file.name} uploaded`);
          res({ path: finalPath, name: file.name });
        });
        input.click();
      });
    }

    async function triggerRomUpload() {
      return new Promise((res) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.addEventListener('change', async () => {
          const file = input.files?.[0];
          if (!file) return res(null);

          const data = new Uint8Array(await file.arrayBuffer());
          const { writeBinary } = await import('./fs.js');
          const tmpPath = `/tmp/upload_rom_${Date.now()}`;
          writeBinary(tmpPath, data);

          const rc = await window.runCommand(`rom --checksum ${tmpPath}`);
          if (rc !== 0) {
            toast('Not a valid ROM image');
            return res(null);
          }

          const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
          const checksum = dv.getUint32(0, false).toString(16).toUpperCase().padStart(8, '0');

          await window.runCommand(`file-copy ${tmpPath} ${romPathForChecksum(checksum)}`);
          toast(`ROM uploaded: ${checksum}`);
          res(checksum);
        });
        input.click();
      });
    }

    // Initialize with first available model — await OPFS scans before showing
    const initialModel = modelIds[0] || 'plus';
    await buildRows(initialModel);

    // Start button handler
    const startBtn = dlg.querySelector('#config-start-btn');
    const onStart = async () => {
      startBtn.removeEventListener('click', onStart);
      dlg.setAttribute('aria-hidden', 'true');

      const modelSel = dlg.querySelector('#config-model');
      const ramSel = dlg.querySelector('#config-ram');
      const selectedModel = modelSel?.value || initialModel;
      const selectedRam = ramSel?.value || '4';
      const modelInfo = models.get(selectedModel);
      const def = MACHINE_DEFS[selectedModel];

      // Collect media selections
      const floppies = [];
      if (def) {
        for (let i = 0; i < def.floppySlots.length; i++) {
          const sel = dlg.querySelector(`#config-fd${i}`);
          if (sel && sel.value && sel.value !== '__upload__') floppies.push(sel.value);
        }
      }
      const hdImages = [];
      if (def) {
        for (let i = 0; i < def.scsiSlots.length; i++) {
          const sel = dlg.querySelector(`#config-hd${i}`);
          if (sel && sel.value && sel.value !== '__upload__') hdImages.push({ path: sel.value, id: i });
        }
      }
      const cdSel = dlg.querySelector('#config-cd');
      const cdImage = (cdSel && cdSel.value && cdSel.value !== '__upload__') ? cdSel.value : null;

      const vromSel = dlg.querySelector('#config-vrom');
      const vromPath = (vromSel && vromSel.value && vromSel.value !== '__upload__') ? vromSel.value : null;

      resolve({
        model: selectedModel,
        romChecksum: modelInfo?.romChecksum,
        ram: parseFloat(selectedRam),
        vromPath,
        floppies,
        hdImages,
        cdImage,
      });
    };
    startBtn.addEventListener('click', onStart);

    dlg.setAttribute('aria-hidden', 'false');
  });
}

// ---------------------------------------------------------------------------
// Boot sequence
// ---------------------------------------------------------------------------

// Execute the boot sequence from config dialog selections.
// tmpRomPath is a fallback if the persisted ROM can't be found.
export async function bootFromConfig(config, tmpRomPath) {
  const { model, romChecksum, ram, vromPath, floppies, hdImages, cdImage } = config;

  // Set VROM path before rom --load, because rom --load triggers machine
  // creation which needs the VROM during SE/30 init.
  if (vromPath) {
    await window.runCommand(`vrom --load ${vromPath}`);
  }

  // Create machine with selected model and RAM before ROM load.
  // rom --load will see the correct machine is already active and skip recreation.
  if (model) {
    const ramKB = Math.round((ram || 4) * 1024);
    await window.runCommand(`setup --model ${model} --ram ${ramKB}`);
  }

  // Load ROM — try persisted OPFS path first, fall back to /tmp.
  // rom --load identifies the ROM and loads it into the existing machine.
  if (romChecksum) {
    const persistedPath = romPathForChecksum(romChecksum);
    let rc = await window.runCommand(`rom --load ${persistedPath}`);
    if (rc !== 0 && tmpRomPath) {
      rc = await window.runCommand(`rom --load ${tmpRomPath}`);
    }
    if (rc !== 0) {
      toast('Failed to load ROM');
      return;
    }
  }

  // Mount floppies
  if (floppies) {
    for (let i = 0; i < floppies.length; i++) {
      await window.runCommand(`insert-fd ${floppies[i]} ${i} 1`);
    }
  }

  // Attach SCSI hard disks
  if (hdImages) {
    for (const { path, id } of hdImages) {
      await window.runCommand(`attach-hd ${path} ${id}`);
    }
  }

  // Attach CD-ROM
  if (cdImage) {
    await window.runCommand(`attach-hd ${cdImage} 3`);
  }

  hideRomOverlay();
  enableRunButton();
  setBackgroundMessage('Click \u25b6 to start emulation');

  await window.runCommand('run');
  setRunning(true);
}
