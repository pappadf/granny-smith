// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Boots the WASM Module and exposes the command API and run-state management.
import { CONFIG } from './config.js';

// Module-level state (owned exclusively by this module)
let Module = null;
let moduleReady = false;
let isRunningUI = false;
let emitPromptFromRuntime = null;

// Mutual-exclusion flag that prevents overlapping Module.ccall() invocations.
// Emscripten's Asyncify cannot handle concurrent async ccalls; a second ccall
// while one is in-flight will corrupt the Asyncify state and crash the WASM module.
let _ccallActive = false;
let _ccallWaiters = [];

// Command preprocessor hook — allows media-persist.js to rewrite commands
// (e.g., persist volatile disk images) before they reach the C side.
let _commandPreprocessor = null;

// Run-state change callbacks (registered by other modules via onRunStateChange)
const _runStateCallbacks = [];

// Install global assertion failure handler (called from C via EM_JS).
// Tests can listen for 'gs-assertion-failure' event or check window.__gsAssertionDetected.
window.__gsAssertionFailures = [];
window.__gsAssertionDetected = false;
window.__gsAssertionHandler = (info) => {
  window.__gsAssertionDetected = true;
  window.__gsAssertionFailures.push(info);
  console.error(`[ASSERT FAIL] ${info.expr} at ${info.file}:${info.line} (${info.func})`);
  window.dispatchEvent(new CustomEvent('gs-assertion-failure', { detail: info }));
};

// Execute a single shell command string and return the exit code.
// Sets the _ccallActive flag to prevent overlapping ccalls.
async function executeShellCommand(cmd) {
  const line = (cmd ?? '').toString();
  if (!line.trim()) return 0;
  _ccallActive = true;
  try {
    const result = await Module.ccall('em_handle_command', 'number', ['string'], [line], { async: true });
    // Emscripten returns BigInt for uint64_t; convert to Number (safe for values up to 2^53)
    return typeof result === 'bigint' ? Number(result) : result;
  } finally {
    _ccallActive = false;
    const waiters = _ccallWaiters.splice(0);
    waiters.forEach(r => r());
  }
}

// Execute a command with mutual exclusion.  Waits for any currently-active
// ccall to finish, then runs immediately.  Prevents concurrent Asyncify ccalls.
async function directExecShellCommand(cmd) {
  // Allow registered preprocessor to rewrite commands before dispatch.
  // Runs before the mutex so the preprocessor can issue its own commands
  // (e.g., 'sync wait') without deadlocking.
  if (_commandPreprocessor) {
    cmd = await _commandPreprocessor(cmd);
  }
  while (_ccallActive) {
    await new Promise(r => _ccallWaiters.push(r));
  }
  return executeShellCommand(cmd);
}

// Initialize the WASM module (called once from main.js).
// printFn is used for both print and printErr on the Module.
export async function initEmulator(canvas, wasmArgs, printFn) {
  const bust = Date.now();
  const { default: createModule } = await import(`../main.mjs?v=${bust}`);

  Module = await createModule({
    canvas,
    arguments: wasmArgs,
    locateFile: (p) => p.endsWith('.wasm') ? `${p}?v=${bust}` : p,
    print: printFn,
    printErr: printFn,
    onRunStateChange: null
  });

  // Bind the runtime prompt function (used by terminal)
  try {
    if (Module && typeof Module.cwrap === 'function') {
      emitPromptFromRuntime = Module.cwrap('shell_emit_prompt', 'string', []);
    }
  } catch (e) {
    console.warn('wasm binding setup failed', e);
  }

  // Expose Module on window for test shim access to FS
  window.__Module = Module;

  // Expose command bridge on window for backward compat and E2E tests.
  // The _ccallActive flag prevents concurrent Asyncify ccalls.
  window.runCommand = (cmd) => directExecShellCommand(cmd);
  window.queueCommand = window.runCommand; // backward-compat alias

  // Install the run-state callback so C can notify JS of state changes.
  try {
    Module.onRunStateChange = (running) => {
      try { setRunning(Boolean(running)); } catch (_) {}
    };
  } catch (e) { console.warn('failed to set Module.onRunStateChange', e); }

  // Run shell_init to prepare the command interface.
  try {
    await Module.ccall('shell_init', 'number', [], [], { async: true });
  } catch (err) {
    printFn(`shell_init failed: ${err?.message || err}`);
  }
  moduleReady = true;

  return Module;
}

// --- Exported state accessors ---

export function getModule() { return Module; }
export function isModuleReady() { return moduleReady; }
export function isRunning() { return isRunningUI; }

// Get the current prompt string from the runtime (or null).
export function getRuntimePrompt() {
  if (typeof emitPromptFromRuntime !== 'function') return null;
  try {
    const value = emitPromptFromRuntime();
    return (value && value.length) ? value : null;
  } catch (err) {
    console.warn('emitPromptFromRuntime failed', err);
    return null;
  }
}

// --- Run-state management ---

// Register a callback for run-state changes: cb(running: boolean).
export function onRunStateChange(cb) { _runStateCallbacks.push(cb); }

// Update the run state and notify all registered listeners.
export function setRunning(running) {
  running = Boolean(running);
  if (isRunningUI === running) return;
  isRunningUI = running;
  for (const cb of _runStateCallbacks) cb(running);
}

// --- Command API (string-based) ---

// Execute a command string (waits only for active ccall, no FIFO queue).
export function runCommand(cmd) {
  return directExecShellCommand(cmd);
}

// Register a function to preprocess commands before C-side dispatch.
// The preprocessor receives the command string and returns a (possibly
// rewritten) command string.  Used by media-persist.js to copy volatile
// images to /persist/images/ before mount commands reach the C core.
export function registerCommandPreprocessor(fn) {
  _commandPreprocessor = fn;
}

// Send shell interrupt (Ctrl-C) to the emulator.
export async function shellInterrupt() {
  if (!Module) return;
  await Module.ccall('shell_interrupt', 'void', [], []);
}
