// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Orchestrator: startup sequence and module glue.
// Zero business logic — only wires modules together in the correct order.
import { BOOT_DIR, ROMS_DIR, IMAGES_DIR } from './config.js';
import { initEmulator, runCommand, isModuleReady, getModule, getRuntimePrompt, shellInterrupt, isRunning } from './emulator.js';
import { initTerminal, writeLine, showPrompt, fitTerminal, handleInterrupt } from './terminal.js';
import { initFS, romExists } from './fs.js';
import { initDragDrop } from './drop.js';
import { processUrlMedia, loadRomAndMaybeRun, isRomLoaded } from './url-media.js';
import { initUI, toast, showRomOverlay, hideRomOverlay, enableRunButton, setBackgroundMessage } from './ui.js';
import { maybeOfferBackgroundCheckpoint } from './checkpoint.js';

const params = new URLSearchParams(location.search);

// --- 1. Build WASM arguments from URL parameters ---
// No --model flag: machine type is determined by ROM identification at load time.
// A ?model= URL parameter can override this for testing or future machine support.
const wasmArgs = [];
if (params.has('model')) {
  wasmArgs.push(`--model=${params.get('model')}`);
}
if (params.has('speed')) {
  const sp = params.get('speed');
  if (['max', 'realtime', 'hardware'].includes(sp)) {
    wasmArgs.push(`--speed=${sp}`);
  } else {
    console.warn('[ui] Ignoring invalid speed param:', sp);
  }
}

// --- 2. Terminal (must be created before emulator so writeLine is ready) ---
initTerminal(document.getElementById('terminal'), {
  onSubmit: async (line) => { await runCommand(line); },
  onInterrupt: async () => { await shellInterrupt(); },
  isReady: () => isModuleReady(),
  isRunning: () => isRunning(),
  getPrompt: () => getRuntimePrompt(),
});

// --- 3. Boot emulator ---
// With PROXY_TO_PTHREAD, main() runs on a worker thread and mounts OPFS
// directories (/boot, /images, /checkpoint) before shell_init().
const canvas = document.getElementById('screen');
await initEmulator(canvas, wasmArgs, writeLine);

// Capture FS reference for /tmp/ operations
initFS(getModule());

// Show initial prompt now that the shell is ready
showPrompt(true);

// --- 4. UI chrome ---
initUI({
  canvas,
  panel: document.getElementById('terminal-panel'),
  toggle: document.getElementById('terminal-toggle'),
  termBody: document.querySelector('.terminal-body'),
  canvasWrapper: document.querySelector('.screen-wrapper'),
  screenToolbar: document.getElementById('screen-toolbar'),
});

// --- 5. Drag & drop ---
initDragDrop(canvas);

// --- 6. Load media ---
// OPFS directories are already available (mounted by C-side main()).
// No initial sync needed — OPFS is always up to date.

let romLoaded = false;
const resumedFromCheckpoint = await maybeOfferBackgroundCheckpoint();
if (resumedFromCheckpoint) {
  enableRunButton();
} else {
  if (!params.has('rom') && !romExists()) {
    showRomOverlay();
    setBackgroundMessage('Drag & drop a ROM file to get started');
  }
  await processUrlMedia(params);
  romLoaded = isRomLoaded();
  if (!romLoaded && romExists()) await loadRomAndMaybeRun();
  // Enable run button if a ROM was loaded by any path
  if (romLoaded || isRomLoaded()) {
    enableRunButton();
    setBackgroundMessage('Click \u25b6 to start emulation');
  }
}

// Signal that the full boot sequence is complete.
// Tests wait for this before issuing commands.
window.__gsBootReady = true;
