// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Machine configuration dialog: lets the user pick model, RAM, media, and boot.
// Also provides the ROM-required dialog for first-time setup.
//
// The dialog is fully driven by core probes — no JS-side machine table:
//   • scanForPersistedRoms() walks /opfs/images/rom/ and calls rom.identify
//     once per file to learn checksum, name, and the compatible-models list.
//   • buildRows() asks machine.profile(id) for the static configuration
//     shape (RAM options, slot layout, CD/VROM flags, …).
import { ROMS_DIR, VROMS_DIR, FD_DIR, FDHD_DIR, HD_DIR, CD_DIR } from './config.js';
import { romPathForChecksum, listDir } from './fs.js';
import { toast, hideRomOverlay, enableRunButton, setBackgroundMessage } from './ui.js';
import { setRunning } from './emulator.js';
import { MEDIA_TYPES } from './media-types.js';
import { runUploadPipeline } from './upload-pipeline.js';
import { quotePath } from './media.js';

// ---------------------------------------------------------------------------
// OPFS ROM scanning — list /opfs/images/rom and probe each entry
// ---------------------------------------------------------------------------

// Walk the ROM directory the frontend owns and call rom.identify per file.
// Returns an array of recognised entries, each:
//   { path, checksum, name, compatible: string[], size }
// Unrecognised files and directories are skipped; unreadable paths are
// silently dropped.
export async function scanForPersistedRoms() {
  const found = [];
  let entries = [];
  try {
    entries = await listDir(ROMS_DIR);
  } catch (e) {
    return found;
  }
  for (const entry of entries) {
    if (entry.kind !== 'file') continue;
    const path = `${ROMS_DIR}/${entry.name}`;
    const info = await window.romIdentify(path);
    if (!info || !info.recognised) continue;
    found.push({
      path,
      checksum: info.checksum,
      name: info.name,
      compatible: Array.isArray(info.compatible) ? info.compatible : [],
      size: info.size,
    });
  }
  return found;
}

// Group scan results by model id.  Returns a Map of model_id → array of
// matching ROM info entries (so the dialog can offer every ROM that lights
// up the model — e.g. both a Universal ROM and a IIcx-specific ROM).
function groupRomsByModel(scanResults) {
  const byModel = new Map();
  for (const r of scanResults) {
    for (const model of r.compatible) {
      if (!byModel.has(model)) byModel.set(model, []);
      byModel.get(model).push(r);
    }
  }
  return byModel;
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

      // Validate via rom.identify: returns null when unreadable, or a map
      // with recognised==false when the bytes don't match a known ROM.
      const info = await window.romIdentify(tmpPath);
      if (!info || !info.recognised || !info.checksum) {
        errorSpan.textContent = 'Not a valid Macintosh ROM image. Please try another file.';
        errorSpan.hidden = false;
        fileInput.value = '';
        return;
      }
      const checksum = info.checksum;

      // Persist to /rom/<checksum> (best-effort)
      await window.gsEval('storage.cp', [tmpPath, romPathForChecksum(checksum)]);

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
// `scanResults` is the array produced by scanForPersistedRoms() above.
// Returns a Promise that resolves with the user's configuration choices.
export async function showConfigDialog(scanResults) {
  return new Promise(async (resolve) => {
    const romsByModel = groupRomsByModel(scanResults);
    const modelIds = [...romsByModel.keys()];

    // machine.profile cache keyed by model id; populated lazily as the user
    // switches between models.  No staleness window (catalog is the running
    // build).
    const profileCache = new Map();
    async function getProfile(id) {
      if (!profileCache.has(id)) profileCache.set(id, await window.machineProfile(id));
      return profileCache.get(id);
    }

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

    // Map a floppy_slots[*].kind to the OPFS directories whose contents
    // should appear for that drive.  Mirrors the rule in proposal §3.3:
    // 800K drives read both 400K and 800K media (single /fd directory),
    // HD drives also read 1.4 MB media (/fd + /fdhd).
    function dirsForFloppyKind(kind) {
      if (kind === 'hd') return [FD_DIR, FDHD_DIR];
      return [FD_DIR];
    }

    async function buildRows(selectedModelId) {
      const profile = await getProfile(selectedModelId);
      if (!profile) return;

      rowsContainer.innerHTML = '';

      // Row 1: Machine Model
      addRow('Machine Model:', await buildModelSelect(selectedModelId));

      // Row 2: Video ROM (when the profile reports needs_vrom)
      if (profile.needs_vrom) {
        const vromFiles = imageFiles(await listDir(VROMS_DIR));
        addRow('Video ROM:', buildMediaSelect('config-vrom',
          mediaOptions(vromFiles, VROMS_DIR), VROMS_DIR));
      }

      // Row 3: RAM Size — driven by ram_options (KB), default ram_default (KB).
      const ramOptions = profile.ram_options.map(kb => ({
        value: String(kb),
        label: kb >= 1024 && kb % 1024 === 0
          ? `${kb / 1024} MB`
          : kb >= 1024
            ? `${(kb / 1024).toFixed(1)} MB`
            : `${kb} KB`,
        selected: kb === profile.ram_default,
      }));
      addRow('RAM Size:', buildSelect('config-ram', ramOptions));

      // Scan OPFS directories for persisted media (via browser OPFS API)
      const fdEntries = await listDir(FD_DIR);
      const fdhdEntries = await listDir(FDHD_DIR);
      const fdFiles = imageFiles(fdEntries);
      const fdhdFiles = imageFiles(fdhdEntries);
      const hdFiles = imageFiles(await listDir(HD_DIR));
      const cdFiles = imageFiles(await listDir(CD_DIR));

      // Floppy rows — one per slot.  Per-slot kind picks the directories.
      for (let i = 0; i < profile.floppy_slots.length; i++) {
        const slot = profile.floppy_slots[i];
        const dirs = dirsForFloppyKind(slot.kind);
        const persistDir = dirs[dirs.length - 1]; // HD drives default uploads to /fdhd
        const opts = [{ value: '', label: '(none)' }];
        for (const dir of dirs) {
          const files = dir === FD_DIR ? fdFiles : fdhdFiles;
          for (const name of files) {
            opts.push({ value: `${dir}/${name}`, label: name });
          }
        }
        opts.push({ value: '__divider__', label: '───────────', disabled: true });
        opts.push({ value: '__upload__', label: 'Upload image...' });
        addRow(`${slot.label}:`, buildMediaSelect(`config-fd${i}`, opts, persistDir));
      }

      // SCSI HD rows
      for (let i = 0; i < profile.scsi_slots.length; i++) {
        const slot = profile.scsi_slots[i];
        addRow(`${slot.label}:`, buildMediaSelect(`config-hd${i}`, mediaOptions(hdFiles, HD_DIR, true), HD_DIR));
      }

      // CD-ROM row
      if (profile.has_cdrom) {
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

    async function buildModelSelect(selectedId) {
      const labels = await Promise.all(modelIds.map(async (id) => {
        const p = await getProfile(id);
        return p?.name || id;
      }));
      const options = modelIds.map((id, i) => ({
        value: id,
        label: labels[i],
        selected: id === selectedId,
      }));
      // Add divider and upload option
      options.push({ value: '__divider__', label: '───────────', disabled: true });
      options.push({ value: '__upload__', label: 'Upload another ROM...' });
      const sel = buildSelect('config-model', options);
      sel.addEventListener('change', () => {
        if (sel.value === '__upload__') {
          triggerRomUpload().then(async (newCs) => {
            if (newCs) {
              // Re-probe the new ROM to get its compatible-models list and
              // refresh the model dropdown.  Probing the file (rather than
              // looking up by checksum) keeps the JS side authoritative-free.
              const info = await window.romIdentify(romPathForChecksum(newCs));
              if (info && info.recognised) {
                scanResults.push({
                  path: romPathForChecksum(newCs),
                  checksum: newCs,
                  name: info.name,
                  compatible: info.compatible,
                  size: info.size,
                });
                const fresh = groupRomsByModel(scanResults);
                modelIds.length = 0;
                for (const id of fresh.keys()) modelIds.push(id);
                romsByModel.clear();
                for (const [k, v] of fresh) romsByModel.set(k, v);
                const next = modelIds.includes(sel._prevValue) ? sel._prevValue : modelIds[0];
                buildRows(next).then(updateStartButton);
                return;
              }
            }
            sel.value = sel._prevValue || modelIds[0];
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
          const json = await window.gsEval('scsi.hd_models');
          if (typeof json === 'string') {
            models = JSON.parse(json);
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
          // extract the nominal MB from the label (e.g. "HD20SC" → 20)
          const match = m.label.match(/(\d+)/);
          const mb = match ? parseInt(match[1], 10) : Math.round(m.size / (1000 * 1000));
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

          if ((await window.gsEval('storage.hd_create', [path, String(sizeBytes)])) !== true) {
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
    async function updateStartButton() {
      const startBtn = dlg.querySelector('#config-start-btn');
      if (!startBtn) return;
      const modelSel = dlg.querySelector('#config-model');
      const currentModel = modelSel?.value || initialModel;
      const profile = await getProfile(currentModel);
      let canStart = true;
      if (profile?.needs_vrom) {
        const vromSel = dlg.querySelector('#config-vrom');
        const val = vromSel?.value;
        if (!val || val === '__upload__') canStart = false;
      }
      startBtn.disabled = !canStart;
    }

    // Initialize with first available model — await OPFS scans before showing
    const initialModel = modelIds[0] || 'plus';
    await buildRows(initialModel);
    await updateStartButton();

    // Start button handler
    const startBtn = dlg.querySelector('#config-start-btn');
    const onStart = async () => {
      startBtn.removeEventListener('click', onStart);
      dlg.setAttribute('aria-hidden', 'true');

      const modelSel = dlg.querySelector('#config-model');
      const ramSel = dlg.querySelector('#config-ram');
      const selectedModel = modelSel?.value || initialModel;
      const profile = await getProfile(selectedModel);
      const ramKB = parseInt(ramSel?.value, 10) || profile?.ram_default || 4096;

      // Pick the first ROM file matching the chosen model.
      const matchingRoms = romsByModel.get(selectedModel) || [];
      const romChecksum = matchingRoms[0]?.checksum;

      // Collect media selections
      const floppies = [];
      if (profile) {
        for (let i = 0; i < profile.floppy_slots.length; i++) {
          const sel = dlg.querySelector(`#config-fd${i}`);
          if (sel && sel.value && sel.value !== '__upload__') floppies.push(sel.value);
        }
      }
      const hdImages = [];
      if (profile) {
        for (let i = 0; i < profile.scsi_slots.length; i++) {
          const sel = dlg.querySelector(`#config-hd${i}`);
          if (sel && sel.value && sel.value !== '__upload__') {
            hdImages.push({ path: sel.value, id: profile.scsi_slots[i].id });
          }
        }
      }
      const cdSel = dlg.querySelector('#config-cd');
      const cdImage = (cdSel && cdSel.value && cdSel.value !== '__upload__') ? cdSel.value : null;
      const cdId = profile?.cdrom_id ?? 3;

      const vromSel = dlg.querySelector('#config-vrom');
      const vromPath = (vromSel && vromSel.value && vromSel.value !== '__upload__') ? vromSel.value : null;

      resolve({
        model: selectedModel,
        romChecksum,
        ramKB,
        vromPath,
        floppies,
        hdImages,
        cdImage,
        cdId,
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
  const { model, romChecksum, ramKB, vromPath, floppies, hdImages, cdImage, cdId } = config;

  // Set VROM path before machine creation, because SE/30 init reads it
  // during the boot sequence.
  if (vromPath) {
    await window.gsEval('vrom.load', [vromPath]);
  }

  // Create the machine with the user-selected model and RAM.  Under the
  // explicit-machine-creation model, this MUST happen before rom.load — and
  // ramKB MUST be one of the model's profile.ram_options values.
  if (model) {
    await window.gsEval('machine.boot', [model, ramKB || 4096]);
  }

  // Load ROM — try persisted OPFS path first, fall back to /tmp.
  if (romChecksum) {
    const persistedPath = romPathForChecksum(romChecksum);
    let ok = (await window.gsEval('rom.load', [persistedPath])) === true;
    if (!ok && tmpRomPath) {
      ok = (await window.gsEval('rom.load', [tmpRomPath])) === true;
    }
    if (!ok) {
      toast('Failed to load ROM');
      return;
    }
  }

  // Mount floppies into successive drive slots.
  if (floppies) {
    for (let i = 0; i < floppies.length; i++) {
      await window.gsEval(`floppy.drives[${i}].insert`, [floppies[i], true]);
    }
  }

  // Attach SCSI hard disks at the bus ids declared by the profile.
  if (hdImages) {
    for (const { path, id } of hdImages) {
      await window.gsEval('scsi.attach_hd', [path, id]);
    }
  }

  // Attach CD-ROM at the profile's cdrom_id (universally 3 today; the
  // value comes from the C side rather than being hardcoded here).
  if (cdImage) {
    await window.gsEval('scsi.attach_cdrom', [cdImage, cdId ?? 3]);
  }

  hideRomOverlay();
  enableRunButton();
  setBackgroundMessage('Click ▶ to start emulation');

  await window.gsEval('scheduler.run');
  setRunning(true);
}
