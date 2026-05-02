// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Boots the WASM Module and exposes the command API and run-state management.
//
// THREADING — read this before adding any new JS → C call site.
//
// We build with -sPROXY_TO_PTHREAD. main() / shell_init() / system_create()
// / emscripten_set_main_loop() all run on a *worker* pthread; that worker
// owns every piece of emulator state (scheduler, machine, devices, OPFS
// file handles). The JS main thread only handles canvas / DOM / xterm.
//
// `Module.ccall(...)` from this thread does NOT auto-proxy to the worker.
// Unless an export is in Emscripten's `proxiedFunctionTable` (only the
// built-in pointerlock / mouse / key / visibility / webgl callbacks are),
// ccall executes the Wasm code on the main thread, racing the worker.
//
// We learned this the hard way: M10c routed `gsEval` through ccall on
// `_em_gs_eval` and the resulting cross-thread access to scheduler /
// device / OPFS state caused 60–90 s checkpoint save/load latency, the
// post-load `run` failing to advance the emulator, and "browser closed"
// crashes in CI. Probes (pthread_self() inside shell_poll vs. inside
// em_gs_eval) confirmed two distinct thread IDs.
//
// THE RULE: every JS → C request goes through one of the SAB-backed
// queues whose pointers are resolved below. JS writes into the input
// buffer, sets the pending flag, polls the done flag. The worker's
// `shell_poll()` (called from `em_main_tick`) drains both queues. There
// are currently two queues:
//   - cmd (free-form shell line)            — `g_cmd_*`     in em_main.c
//   - gs_eval (typed object-model bridge)   — `g_gs_*`      in em_main.c
// Both are serialized through the same JS-side `cmdInFlight` lock so
// only one is in flight at a time and ordering is well defined.
//
// (`em_tab_complete` is still a direct ccall today — same threading
// hazard, but tab completion is a synchronous read-only operation and
// has not surfaced as a problem. If it ever does, route it through a
// third SAB queue with the same shape.)
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

// Shared-heap gs_eval queue pointers (resolved after Module init)
let gsPathBufPtr = 0;
let gsArgsBufPtr = 0;
let gsPendingPtr = 0;
let gsDonePtr = 0;
let gsResultPtr = 0;
let gsEvalBufPtr = 0;

// Buffer sizes mirror em_main.c. Keep in sync there.
const GS_PATH_BUF_SIZE = 1024;
const GS_ARGS_BUF_SIZE = 8192;

// Serialization: only one command (legacy or gs_eval) in flight at a time.
// shell_poll() drains both queues but the C-side pre-condition is that
// only one is set at a time, so this lock covers both bridges.
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
          const intResult = Module.HEAP32[cmdResultPtr >> 2];
          Module.HEAP32[cmdDonePtr >> 2] = 0;
          resolve(intResult);
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

  gsPathBufPtr = Module._get_gs_path_buffer();
  gsArgsBufPtr = Module._get_gs_args_buffer();
  gsPendingPtr = Module._get_gs_pending_ptr();
  gsDonePtr = Module._get_gs_done_ptr();
  gsResultPtr = Module._get_gs_result_ptr();
  gsEvalBufPtr = Module._get_gs_eval_buffer();

  // Expose Module on window for test shim access to FS
  window.__Module = Module;

  // Expose the typed object-model bridge on window. After M10b/c the JS
  // app callers all use `gsEval`; `window.runCommand` only stays around
  // as the terminal-input bridge and the small set of pre-main-loop
  // boot calls (main.js / checkpoint.js) where ccall ordering is still
  // unsafe — both flagged in their call sites.
  window.runCommand = (cmd) => executeShellCommand(cmd);
  window.queueCommand = window.runCommand;
  window.tabComplete = (line, cursorPos) => tabComplete(line, cursorPos);
  window.gsEval = (path, args) => gsEval(path, args);
  window.gsInspect = (path) => gsInspect(path);

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

// Execute a shell line via the worker-side command queue. Returns the
// integer result. After M10b–M10d this is only used for:
//   - terminal user input (free-form shell text the user types)
//   - the two pre-main-loop boot calls in main.js / checkpoint.js
//     where ccall ordering is still unsafe
// Everything else goes through gsEval.
export function runCommand(cmd) {
  return executeShellCommand(cmd);
}

// (M10e) Removed `runCommandJSON`: the only caller in app/web/js was
// config-dialog.js's "hd models --json" lookup, now backed by the
// typed `hd_models()` root method. If a future need for structured
// results re-emerges, expose it through the gsEval surface — not as a
// runCommand variant.

// Send shell interrupt (Ctrl-C) to the emulator.
// shell_interrupt just sets a flag — safe to call from any thread.
export async function shellInterrupt() {
  if (!Module) return;
  Module._shell_interrupt();
}

// Tab completion: call the WASM completion engine and return an array of matches.
// Uses ccall to marshal the call to the worker thread (PROXY_TO_PTHREAD).
// Note: With PROXY_TO_PTHREAD, ccall with async:false blocks until the worker responds.
let completionBufPtr = 0;

export function tabComplete(line, cursorPos) {
  if (!Module || !moduleReady) return null;

  // Resolve the completion buffer pointer on first use
  if (!completionBufPtr) {
    try { completionBufPtr = Module._get_completion_buffer(); } catch (e) { return null; }
  }
  if (!completionBufPtr) return null;

  try {
    // Use ccall to invoke em_tab_complete on the worker thread
    const count = Module.ccall('em_tab_complete', 'number',
      ['string', 'number'], [line, cursorPos]);

    if (count === 0) return [];

    // Read the JSON result from the completion buffer (shared heap)
    const jsonStr = Module.UTF8ToString(completionBufPtr);
    return JSON.parse(jsonStr);
  } catch (e) {
    return null;
  }
}

// ----------------------------------------------------------------------------
// Object-model bridge (M10a, repaired in M10e to run on the worker pthread).
// ----------------------------------------------------------------------------
//
//   gsEval(path)            → attribute read or zero-arg method call
//   gsEval(path, [v0, v1])  → method call, or attribute write when path
//                             is an attribute and the array has one entry
//   gsInspect(path)         → same JSON shape as gsEval; will diverge into
//                             a recursive subtree shape with M11
//
// args may be undefined / null / an array of primitive values (number,
// string, boolean, null). The C side parses the JSON-encoded array.
//
// IMPORTANT: this routes through the SAB-backed gs_eval queue (g_gs_*),
// not via `Module.ccall('em_gs_eval', ...)`. ccall on _em_gs_eval would
// run the body on this (main) thread instead of the worker pthread that
// owns scheduler / device / OPFS state — see the "Threading" comment at
// the top of this file. The previous ccall-based implementation (M10a)
// was the cause of the M10c CI regression.

function readGsEvalResult() {
  if (!gsEvalBufPtr) return null;
  const jsonStr = Module.UTF8ToString(gsEvalBufPtr);
  if (!jsonStr) return null;
  try { return JSON.parse(jsonStr); } catch (e) { return jsonStr; }
}

// Wait for the worker's shell_init() to complete before issuing the
// first gs_eval request. With PROXY_TO_PTHREAD, createModule resolves
// while the worker is still inside main() / shell_init() — setting
// g_gs_pending during that window would have shell_poll dispatch
// against the empty default root class and return {error: "..."}. The
// shared-memory flag is flipped to 1 at the end of shell_init().
let shellReadyPtr = 0;
async function waitForShellReady() {
  if (!shellReadyPtr) {
    try { shellReadyPtr = Module._get_shell_ready_ptr(); } catch (e) { return; }
  }
  if (!shellReadyPtr) return;
  if (Module.HEAP32[shellReadyPtr >> 2]) return;
  for (let i = 0; i < 200; i++) {
    if (Module.HEAP32[shellReadyPtr >> 2]) return;
    await new Promise((r) => setTimeout(r, 10));
  }
}

// Drive a single gs_eval / gs_inspect request through the SAB queue.
// kind: 1 = gs_eval (uses both path + argsJson), 2 = gs_inspect (path only).
async function executeGsRequest(path, argsJson, kind) {
  // Same in-flight lock as executeShellCommand: shell_poll() drains both
  // queues but the C-side pre-condition is that only one of (cmd_pending,
  // gs_pending) is set at a time, so we serialize JS-side.
  while (cmdInFlight) {
    await new Promise(r => cmdWaiters.push(r));
  }
  cmdInFlight = true;
  try {
    Module.stringToUTF8(String(path || ''), gsPathBufPtr, GS_PATH_BUF_SIZE);
    Module.stringToUTF8(argsJson || '', gsArgsBufPtr, GS_ARGS_BUF_SIZE);

    Module.HEAP32[gsDonePtr >> 2] = 0;
    Module.HEAP32[gsPendingPtr >> 2] = kind;

    await new Promise(resolve => {
      function check() {
        if (Module.HEAP32[gsDonePtr >> 2]) {
          Module.HEAP32[gsDonePtr >> 2] = 0;
          resolve();
        } else {
          setTimeout(check, 1);
        }
      }
      check();
    });

    return readGsEvalResult();
  } finally {
    cmdInFlight = false;
    const waiters = cmdWaiters.splice(0);
    waiters.forEach(r => r());
  }
}

export async function gsEval(path, args) {
  if (!Module || !moduleReady) return null;
  await waitForShellReady();
  const argsJson = (args === undefined || args === null) ? '' : JSON.stringify(args);
  try {
    return await executeGsRequest(String(path || ''), argsJson, 1);
  } catch (e) {
    return null;
  }
}

export async function gsInspect(path) {
  if (!Module || !moduleReady) return null;
  await waitForShellReady();
  try {
    return await executeGsRequest(String(path || ''), '', 2);
  } catch (e) {
    return null;
  }
}
