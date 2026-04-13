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
import { MEDIA_TYPES } from './media-types.js';
import { runUploadPipeline } from './upload-pipeline.js';
import { quotePath } from './media.js';

// ---------------------------------------------------------------------------
// OPFS ROM scanning — probe known checksums to find persisted ROMs
// ---------------------------------------------------------------------------

// Scan /rom/ for ROM files by probing all known checksums from ROM_DATABASE.
// Returns an array of checksum strings that exist on disk.
export async function scanForPersistedRoms() {
  const found = [];
  for (const cs of Object.keys(ROM_DATABASE)) {
    const path = romPathForChecksum(cs);
    const rc = await window.runCommand(`rom probe ${path}`);
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
          <p class="modal__message">Upload a Macintosh Plus or SE/30 ROM file to get started.</p>
          <div class="modal__actions">
            <button class="primary-btn" id="rom-upload-select-btn">Select ROM File...</button>
            <input id="rom-upload-file-input" type="file" style="display:none" />
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
      const rc = await window.runCommand(`rom checksum ${tmpPath}`);
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
    // If allowCreate is true, a "Create blank image..." option is also added.
    function mediaOptions(files, dir, allowCreate = false) {
      const opts = [{ value: '', label: '(none)' }];
      for (const name of files) {
        opts.push({ value: `${dir}/${name}`, label: name });
      }
      opts.push({ value: '__divider__', label: '───────────', disabled: true });
      opts.push({ value: '__upload__', label: 'Upload image...' });
      if (allowCreate) {
        opts.push({ value: '__create__', label: 'Create blank image...' });
      }
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
        addRow(`${def.scsiSlots[i]}:`, buildMediaSelect(`config-hd${i}`, mediaOptions(hdFiles, HD_DIR, true), HD_DIR));
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
              buildRows(newIds.includes(sel._prevValue) ? sel._prevValue : newIds[0]).then(updateStartButton);
            } else {
              sel.value = sel._prevValue || modelIds[0];
            }
          });
          return;
        }
        sel._prevValue = sel.value;
        buildRows(sel.value).then(updateStartButton);
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

    // Build a media dropdown with "Upload image..." and optionally "Create blank image..." options.
    // persistDir is the OPFS directory to persist uploaded files to (e.g. /images/fd).
    function buildMediaSelect(id, options, persistDir) {
      const sel = buildSelect(id, options);
      // Insert a newly created/uploaded image option before the divider.
      function addImageOption(path, name) {
        const newOpt = document.createElement('option');
        newOpt.value = path;
        newOpt.textContent = name;
        const divider = sel.querySelector('option[value="__divider__"]');
        if (divider) {
          sel.insertBefore(newOpt, divider);
        } else {
          sel.insertBefore(newOpt, sel.lastChild);
        }
        sel.value = path;
      }
      sel.addEventListener('change', () => {
        if (sel.value === '__upload__') {
          triggerMediaUpload(persistDir).then(result => {
            if (result) {
              addImageOption(result.path, result.name);
            } else {
              sel.value = '';
            }
            updateStartButton();
          });
          return;
        }
        if (sel.value === '__create__') {
          showHdSizeDialog().then(result => {
            if (result) {
              addImageOption(result.path, result.name);
            } else {
              sel.value = '';
            }
            updateStartButton();
          });
          return;
        }
        updateStartButton();
      });
      return sel;
    }

    // Show a dialog prompting the user for a hard disk image size.
    // Queries the emulator for available drive models dynamically.
    // Returns { path, name } on success, or null if cancelled.
    function showHdSizeDialog() {
      return new Promise(async (resolve) => {
        // query available drive models from the emulator
        let models = [];
        try {
          const result = await window.runCommandJSON('hd models --json');
          if (result && result.output) {
            models = JSON.parse(result.output);
          }
        } catch (e) {
          // fallback: empty means dialog will show an error
        }
        if (!models.length) {
          resolve(null);
          return;
        }

        let dlg = document.getElementById('hd-size-dialog');
        // always rebuild the dialog to reflect current models
        if (dlg) dlg.remove();
        dlg = document.createElement('div');
        dlg.id = 'hd-size-dialog';
        dlg.className = 'modal';
        dlg.setAttribute('role', 'dialog');
        dlg.setAttribute('aria-modal', 'true');
        dlg.setAttribute('aria-hidden', 'true');

        // deduplicate models by label, keeping the largest size per label
        const byLabel = new Map();
        for (const m of models) {
          if (!byLabel.has(m.label) || m.size > byLabel.get(m.label).size)
            byLabel.set(m.label, m);
        }
        const uniqueModels = [...byLabel.values()];

        // build radio buttons from model list
        const radios = uniqueModels.map((m, i) => {
          const mb = Math.round(m.size / (1000 * 1000));
          return `<label class="hd-size-option">
              <input type="radio" name="hd-size" value="${m.size}" ${i === 0 ? 'checked' : ''} />
              <span>${mb} MB (${m.label})</span>
            </label>`;
        }).join('\n');

        dlg.innerHTML = `
            <div class="modal__content hd-size-modal">
              <h2 class="modal__title">Create Blank Hard Disk</h2>
              <p class="modal__message">Choose a size for the new hard disk image.</p>
              <div class="hd-size-options">
                ${radios}
              </div>
              <span class="hd-size-error" id="hd-size-error" hidden></span>
              <div class="modal__actions">
                <button class="secondary-btn" id="hd-size-cancel-btn">Cancel</button>
                <button class="primary-btn" id="hd-size-create-btn">Create</button>
              </div>
            </div>
          `;
        document.body.appendChild(dlg);

        const errorSpan = dlg.querySelector('#hd-size-error');
        const createBtn = dlg.querySelector('#hd-size-create-btn');
        const cancelBtn = dlg.querySelector('#hd-size-cancel-btn');
        errorSpan.hidden = true;

        dlg.setAttribute('aria-hidden', 'false');

        let closed = false;
        const close = (result) => {
          if (closed) return;
          closed = true;
          dlg.setAttribute('aria-hidden', 'true');
          resolve(result);
        };

        // Cancel on backdrop click or Escape
        const onBackdrop = (e) => { if (e.target === dlg) close(null); };
        const onKey = (e) => { if (e.key === 'Escape') close(null); };
        dlg.addEventListener('click', onBackdrop);
        document.addEventListener('keydown', onKey);

        cancelBtn.addEventListener('click', () => close(null), { once: true });
        createBtn.addEventListener('click', async () => {
          const selected = dlg.querySelector('input[name="hd-size"]:checked');
          const sizeBytes = parseInt(selected.value, 10);
          errorSpan.hidden = true;

          // Human-readable label for filename
          const sizeMB = Math.round(sizeBytes / (1024 * 1024));

          // Generate a unique filename
          const name = `blank_${sizeMB}MB_${Date.now()}.img`;
          const path = `${HD_DIR}/${name}`;

          const rc = await window.runCommand(`hd create ${quotePath(path)} ${sizeBytes}`);
          if (rc !== 0) {
            errorSpan.textContent = 'Failed to create disk image.';
            errorSpan.hidden = false;
            return;
          }

          dlg.removeEventListener('click', onBackdrop);
          document.removeEventListener('keydown', onKey);
          close({ path, name });
        }, { once: true });
      });
    }

    // Determine the media type descriptor for a given persist directory.
    // FDHD_DIR is a sub-type of fd (high-density floppies), so match it too.
    function mediaTypeForDir(persistDir) {
      if (persistDir === FDHD_DIR) return MEDIA_TYPES.fd;
      return Object.values(MEDIA_TYPES).find(mt => mt.persistDir === persistDir)
        || MEDIA_TYPES.hd;
    }

    // Upload a media file via the unified pipeline.
    // Returns { path, name } on success, null on failure.
    async function triggerMediaUpload(persistDir) {
      const mediaType = mediaTypeForDir(persistDir);
      return new Promise((resolve) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.addEventListener('change', async () => {
          const file = input.files?.[0];
          if (!file) return resolve(null);
          const result = await runUploadPipeline(file, mediaType);
          resolve(result);
        });
        input.click();
      });
    }

    // Upload a ROM via the unified pipeline. Returns checksum or null.
    async function triggerRomUpload() {
      return new Promise((resolve) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.addEventListener('change', async () => {
          const file = input.files?.[0];
          if (!file) return resolve(null);
          const result = await runUploadPipeline(file, MEDIA_TYPES.rom);
          resolve(result ? result.info?.checksum : null);
        });
        input.click();
      });
    }

    // Enable/disable Start based on required selections (e.g. VROM for SE/30).
    function updateStartButton() {
      const startBtn = dlg.querySelector('#config-start-btn');
      if (!startBtn) return;
      const modelSel = dlg.querySelector('#config-model');
      const currentModel = modelSel?.value || initialModel;
      const def = MACHINE_DEFS[currentModel];
      let canStart = true;
      if (def?.hasVrom) {
        const vromSel = dlg.querySelector('#config-vrom');
        const val = vromSel?.value;
        if (!val || val === '__upload__') canStart = false;
      }
      startBtn.disabled = !canStart;
    }

    // Initialize with first available model — await OPFS scans before showing
    const initialModel = modelIds[0] || 'plus';
    await buildRows(initialModel);
    updateStartButton();

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

  // Set VROM path before rom load, because rom load triggers machine
  // creation which needs the VROM during SE/30 init.
  if (vromPath) {
    await window.runCommand(`vrom load ${quotePath(vromPath)}`);
  }

  // Create machine with selected model and RAM before ROM load.
  // rom load will see the correct machine is already active and skip recreation.
  if (model) {
    const ramKB = Math.round((ram || 4) * 1024);
    await window.runCommand(`setup --model ${model} --ram ${ramKB}`);
  }

  // Load ROM — try persisted OPFS path first, fall back to /tmp.
  // rom load identifies the ROM and loads it into the existing machine.
  if (romChecksum) {
    const persistedPath = romPathForChecksum(romChecksum);
    let rc = await window.runCommand(`rom load ${quotePath(persistedPath)}`);
    if (rc !== 0 && tmpRomPath) {
      rc = await window.runCommand(`rom load ${quotePath(tmpRomPath)}`);
    }
    if (rc !== 0) {
      toast('Failed to load ROM');
      return;
    }
  }

  // Mount floppies
  if (floppies) {
    for (let i = 0; i < floppies.length; i++) {
      await window.runCommand(`fd insert ${quotePath(floppies[i])} ${i} true`);
    }
  }

  // Attach SCSI hard disks
  if (hdImages) {
    for (const { path, id } of hdImages) {
      await window.runCommand(`hd attach ${quotePath(path)} ${id}`);
    }
  }

  // Attach CD-ROM
  if (cdImage) {
    await window.runCommand(`cdrom attach ${quotePath(cdImage)}`);
  }

  hideRomOverlay();
  enableRunButton();
  setBackgroundMessage('Click \u25b6 to start emulation');

  await window.runCommand('run');
  setRunning(true);
}
