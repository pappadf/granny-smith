// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Boots the WASM Module and exposes the command API and run-state management.
//
// With PROXY_TO_PTHREAD, the emulator's main() runs on a worker thread.
// JS ccall runs on the main thread where the OPFS mount is not visible.
// Commands are dispatched via a shared-heap command buffer: JS writes the
// command string into the WASM heap (SharedArrayBuffer), sets a pending flag,
// and the worker's shell_poll() dequeues and executes each tick.
//
// Run-state is also communicated via a shared-heap flag (g_shared_is_running)
// that the worker updates every tick.  JS polls it to keep the UI in sync.
import { CONFIG } from './config.js';

// Module-level state (owned exclusively by this module)
let Module = null;
let moduleReady = false;
let isRunningUI = false;

// Shared-heap command queue pointers (resolved after Module init)
let cmdBufPtr = 0;
let cmdPendingPtr = 0;
let cmdDonePtr = 0;
let cmdResultPtr = 0;
let promptBufPtr = 0;
let isRunningPtr = 0;

// Serialization: only one command in flight at a time
let cmdInFlight = false;
let cmdWaiters = [];

// Run-state polling interval handle
let _runStatePollId = 0;

// Run-state change callbacks (registered by other modules via onRunStateChange)
const _runStateCallbacks = [];

// Install global assertion failure handler (called from C via MAIN_THREAD_EM_ASM).
// Tests can listen for 'gs-assertion-failure' event or check window.__gsAssertionDetected.
window.__gsAssertionFailures = [];
window.__gsAssertionDetected = false;
window.__gsAssertionHandler = (info) => {
  window.__gsAssertionDetected = true;
  window.__gsAssertionFailures.push(info);
  console.error(`[ASSERT FAIL] ${info.expr} at ${info.file}:${info.line} (${info.func})`);
  window.dispatchEvent(new CustomEvent('gs-assertion-failure', { detail: info }));
};

// Poll the shared-heap running flag and update JS state if it changed.
function pollRunState() {
  if (!Module || !isRunningPtr) return;
  const running = Boolean(Module.HEAP32[isRunningPtr >> 2]);
  if (running !== isRunningUI) {
    isRunningUI = running;
    for (const cb of _runStateCallbacks) cb(running);
  }
}

// Execute a single shell command string and return the exit code.
// Commands are serialized (one at a time) and dispatched to the worker
// via the shared-heap command buffer.
async function executeShellCommand(cmd) {
  const line = (cmd ?? '').toString();
  if (!line.trim()) return 0;

  // Wait for any in-flight command to complete
  while (cmdInFlight) {
    await new Promise(r => cmdWaiters.push(r));
  }
  cmdInFlight = true;

  try {
    // Write command string to shared heap buffer
    Module.stringToUTF8(line, cmdBufPtr, 4096);

    // Clear done flag, set pending flag (HEAP32 indices are byte-offset >> 2)
    Module.HEAP32[cmdDonePtr >> 2] = 0;
    Module.HEAP32[cmdPendingPtr >> 2] = 1;

    // Poll for completion (worker executes between main loop ticks)
    const result = await new Promise(resolve => {
      function check() {
        if (Module.HEAP32[cmdDonePtr >> 2]) {
          const r = Module.HEAP32[cmdResultPtr >> 2];
          Module.HEAP32[cmdDonePtr >> 2] = 0;
          resolve(r);
        } else {
          setTimeout(check, 1);
        }
      }
      check();
    });

    return result;
  } finally {
    cmdInFlight = false;
    const waiters = cmdWaiters.splice(0);
    waiters.forEach(r => r());
  }
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
  });

  // Resolve shared-heap pointers into the WASM heap.
  cmdBufPtr = Module._get_cmd_buffer();
  cmdPendingPtr = Module._get_cmd_pending_ptr();
  cmdDonePtr = Module._get_cmd_done_ptr();
  cmdResultPtr = Module._get_cmd_result_ptr();
  promptBufPtr = Module._get_prompt_buffer();
  isRunningPtr = Module._get_is_running_ptr();

  // Expose Module on window for test shim access to FS
  window.__Module = Module;

  // Expose command bridge on window for backward compat and E2E tests.
  window.runCommand = (cmd) => executeShellCommand(cmd);
  window.queueCommand = window.runCommand; // backward-compat alias

  // With PROXY_TO_PTHREAD, shell_init is called from main() on the worker.
  // No need to ccall it from JS.
  moduleReady = true;

  // Start polling the worker's running-state flag so the UI stays in sync.
  // The worker may have already loaded a ROM and started running by now.
  _runStatePollId = setInterval(pollRunState, 100);
  pollRunState(); // sync immediately

  return Module;
}

// --- Exported state accessors ---

export function getModule() { return Module; }
export function isModuleReady() { return moduleReady; }
export function isRunning() { return isRunningUI; }

// Get the current prompt string from the shared heap buffer.
// The worker updates g_prompt_buffer after each command in shell_poll().
export function getRuntimePrompt() {
  if (!Module || !promptBufPtr) return null;
  try {
    const value = Module.UTF8ToString(promptBufPtr);
    return (value && value.length) ? value : null;
  } catch (err) {
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

// Execute a command string via the worker-side command queue.
export function runCommand(cmd) {
  return executeShellCommand(cmd);
}

// Send shell interrupt (Ctrl-C) to the emulator.
// shell_interrupt just sets a flag — safe to call from any thread.
export async function shellInterrupt() {
  if (!Module) return;
  Module._shell_interrupt();
}
