// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Orchestrator: startup sequence and module glue.
// Zero business logic — only wires modules together in the correct order.
import { initEmulator, runCommand, isModuleReady, getModule, getRuntimePrompt, shellInterrupt, isRunning, tabComplete } from './emulator.js';
import { initTerminal, writeLine, showPrompt, fitTerminal, handleInterrupt } from './terminal.js';
import { initFS } from './fs.js';
import { initDragDrop } from './drop.js';
import { processUrlMedia, isRomLoaded } from './url-media.js';
import { initUI, toast, enableRunButton, setBackgroundMessage } from './ui.js';
import { maybeOfferBackgroundCheckpoint } from './checkpoint.js';
import { scanForPersistedRoms, showRomUploadDialog, showConfigDialog, bootFromConfig } from './config-dialog.js';
import { UPLOAD_DIR } from './config.js';
import { clearOPFSDir } from './fs.js';
import { getOrCreateMachine } from './checkpoint-machine.js';
import { initInspector } from './inspector.js';

const params = new URLSearchParams(location.search);

// --- 1. Build WASM arguments from URL parameters ---
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
  tabComplete: (line, cursorPos) => tabComplete(line, cursorPos),
});

// --- 3. Boot emulator ---
// With PROXY_TO_PTHREAD, main() runs on a worker thread and mounts OPFS
// directories (/rom, /fd, /hd, /checkpoints, etc.) before shell_init().
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

// --- 5b. Object inspector panel (M11) ---
initInspector();

// --- 6. Load media ---
// OPFS directories are already available (mounted by C-side main()).

// Activate the per-machine checkpoint directory before anything that opens
// images runs.  Awaiting here preserves command ordering: every later
// runCommand (starting with `checkpoint --probe` below) sees the machine
// identity already registered.  Done after initUI so the click handlers on
// the terminal toggle / canvas are wired before this awaits.
//
// Uses runCommand instead of gsEval here because this fires during the
// boot window between Module-ready and the worker's main loop becoming
// active, where ccall-based gsEval requests are not yet served — the
// runCommand path naturally waits via the cmd_pending flag the main loop
// polls. M10c re-evaluates this once the e2e helper migration lands.
{
  const machine = getOrCreateMachine();
  await runCommand(`checkpoint --machine ${machine.id} ${machine.created}`);
}

const resumedFromCheckpoint = await maybeOfferBackgroundCheckpoint();
if (resumedFromCheckpoint) {
  enableRunButton();
} else if (params.has('rom')) {
  // URL params specify media — download and auto-boot (skip dialogs)
  await processUrlMedia(params);
  if (isRomLoaded()) {
    enableRunButton();
  }
} else if (params.has('noui')) {
  // Headless/test mode: skip all dialogs, let the test harness drive commands.
} else {
  // Normal startup: clean up stale staging files from previous sessions
  await clearOPFSDir(UPLOAD_DIR);

  // Scan OPFS for persisted ROMs
  let romChecksums = await scanForPersistedRoms();
  let tmpRomPath = null;

  // If no ROMs found, show the ROM upload dialog
  if (romChecksums.length === 0) {
    const result = await showRomUploadDialog();
    if (result) {
      romChecksums = [result.checksum];
      tmpRomPath = result.tmpPath; // fallback if OPFS persist failed
    }
  }

  if (romChecksums.length > 0) {
    // Show machine configuration dialog
    const config = await showConfigDialog(romChecksums);
    await bootFromConfig(config, tmpRomPath);
  }
}

// Signal that the full boot sequence is complete.
// Tests wait for this before issuing commands.
window.__gsBootReady = true;
