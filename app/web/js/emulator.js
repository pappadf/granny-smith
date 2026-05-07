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
// THE RULE: every JS → C request goes through the SAB-backed queue
// whose pointers are resolved below. JS writes into the input buffer,
// sets the pending flag, polls the done flag. The worker's
// `shell_poll()` drains the queue and writes the result. The single
// queue (`g_gs_*`) carries every request — `g_gs_pending` selects the
// dispatch by kind (1=gs_eval, 2=gs_inspect, 3=tab_complete,
// 4=free-form shell line). The JS-side `cmdInFlight` lock ensures
// only one request is in flight at a time.
import { CONFIG } from './config.js';

// Module-level state (owned exclusively by this module)
let Module = null;
let moduleReady = false;
let isRunningUI = false;

// Base pointer to the single shared-memory `js_bridge_t` region exposed
// by the worker. Resolved once at init via Module._get_js_bridge().
// Layout MUST mirror src/platform/wasm/em.h — bump BRIDGE_VERSION and
// the offsets below whenever the C struct changes.
let bridgePtr = 0;

const BRIDGE_VERSION = 3;
const OFF_VERSION     = 0;
const OFF_READY = 4;
const OFF_PENDING     = 8;
const OFF_DONE        = 12;
const OFF_RESULT      = 16;
const OFF_PATH        = 20;
const OFF_ARGS        = 1044;
const OFF_OUTPUT      = 9236;
const PATH_SIZE       = 1024;
const ARGS_SIZE       = 8192;

// Serialization: only one request in flight on the SAB queue at a time.
let cmdInFlight = false;
let cmdWaiters = [];

// Run-state change callbacks (registered by other modules via onRunStateChange).
// Fired by the C side via Module.onRunStateChange whenever the scheduler
// flips state — no JS-side polling.
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

// Park until the worker stores `done = 1` and notifies. The C side
// pairs its store with `emscripten_atomic_notify(&g_bridge.done, 1)`,
// so this resolves on the next event-loop turn after the worker tick
// — no setTimeout polling, no main-thread CPU spin. Atomics.waitAsync
// returns synchronously with `not-equal` if the worker beat us to it.
async function waitForBridgeDone() {
  const doneIdx = (bridgePtr + OFF_DONE) >> 2;
  const w = Atomics.waitAsync(Module.HEAP32, doneIdx, 0);
  if (w.async) await w.value;
  Atomics.store(Module.HEAP32, doneIdx, 0);
}

// Receives push notifications from the worker on every scheduler
// state transition (including the first tick). Mirrors what the old
// pollRunState() did, minus the timer.
function handleRunStateChange(running) {
  running = Boolean(running);
  if (running === isRunningUI) return;
  isRunningUI = running;
  for (const cb of _runStateCallbacks) cb(running);
}

// Cache for the current shell prompt. The worker pushes updates via
// Module.onPromptChange after each free-form line in shell_poll; the
// terminal reads this through getRuntimePrompt() when it needs to
// display the next prompt.
let cachedPrompt = null;
function handlePromptChange(text) {
  cachedPrompt = (typeof text === 'string' && text.length) ? text : null;
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
    onRunStateChange: handleRunStateChange,
    onPromptChange: handlePromptChange,
  });

  // Resolve the single bridge pointer and verify the layout version
  // matches what we were compiled against.
  bridgePtr = Module._get_js_bridge();
  const v = Module.HEAP32[(bridgePtr + OFF_VERSION) >> 2];
  if (v !== BRIDGE_VERSION) {
    throw new Error(`js_bridge version mismatch: C=${v}, JS=${BRIDGE_VERSION}`);
  }

  // Expose Module on window for test shim access to FS
  window.__Module = Module;

  // Expose object-model bridges. The terminal's onSubmit (main.js)
  // imports gsEvalLine for free-form lines; everything else uses gsEval
  // / gsInspect for typed object-model access.
  window.tabComplete = (line, cursorPos) => tabComplete(line, cursorPos);
  window.gsEval = (path, args) => gsEval(path, args);
  window.gsInspect = (path) => gsInspect(path);
  window.romIdentify = (path) => romIdentify(path);
  window.machineProfile = (id) => machineProfile(id);

  // With PROXY_TO_PTHREAD, shell_init is called from main() on the worker.
  // No need to ccall it from JS. The worker pushes the initial run-state
  // (and every transition) via Module.onRunStateChange — see em_main_tick.
  moduleReady = true;
  return Module;
}

// --- Exported state accessors ---

export function getModule() { return Module; }
export function isModuleReady() { return moduleReady; }
export function isRunning() { return isRunningUI; }

// Get the current shell prompt. The worker pushes updates via
// Module.onPromptChange after each free-form command, which is fired
// synchronously before gsEvalLine resolves — so this read always
// reflects the prompt the worker just produced.
export function getRuntimePrompt() {
  return cachedPrompt;
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

// --- Command API ---

// Execute a free-form shell line via the SAB queue (kind=4). Returns
// the integer result. Used by the terminal input handler — every other
// caller should use gsEval / gsInspect for typed object-model access.
export async function gsEvalLine(line) {
  if (!Module || !moduleReady) return -1;
  const text = (line ?? '').toString();
  if (!text.trim()) return 0;

  await waitForBridgeReady();

  while (cmdInFlight) {
    await new Promise(r => cmdWaiters.push(r));
  }
  cmdInFlight = true;
  try {
    Module.stringToUTF8(text, bridgePtr + OFF_PATH, PATH_SIZE);
    Module.HEAPU8[bridgePtr + OFF_ARGS] = 0; // clear stale args
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_DONE) >> 2, 0);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_PENDING) >> 2, 4); // kind=4 → free-form line

    await waitForBridgeDone();
    return Module.HEAP32[(bridgePtr + OFF_RESULT) >> 2];
  } finally {
    cmdInFlight = false;
    const waiters = cmdWaiters.splice(0);
    waiters.forEach(r => r());
  }
}

// Stop the running scheduler (Ctrl-C / Pause). Routes through the
// object-model channel like every other JS→C call; shell_poll drains
// the SAB queue every tick regardless of CPU run-state, so this
// reaches scheduler_stop even mid-emulation.
export async function shellInterrupt() {
  if (!Module || !moduleReady) return;
  await gsEval('scheduler.stop');
}

// Tab completion: kind=3 on the bridge. Cursor pos goes in `args` as
// decimal text; JSON match list comes back in `output`.
export async function tabComplete(line, cursorPos) {
  if (!Module || !moduleReady) return null;

  await waitForBridgeReady();

  while (cmdInFlight) {
    await new Promise(r => cmdWaiters.push(r));
  }
  cmdInFlight = true;
  try {
    Module.stringToUTF8(String(line || ''), bridgePtr + OFF_PATH, PATH_SIZE);
    Module.stringToUTF8(String(cursorPos | 0), bridgePtr + OFF_ARGS, ARGS_SIZE);

    Atomics.store(Module.HEAP32, (bridgePtr + OFF_DONE) >> 2, 0);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_PENDING) >> 2, 3); // kind=3 → tab complete

    await waitForBridgeDone();

    const count = Module.HEAP32[(bridgePtr + OFF_RESULT) >> 2];
    if (count === 0) return [];

    const jsonStr = Module.UTF8ToString(bridgePtr + OFF_OUTPUT);
    try { return JSON.parse(jsonStr); } catch (e) { return null; }
  } catch (e) {
    return null;
  } finally {
    cmdInFlight = false;
    const waiters = cmdWaiters.splice(0);
    waiters.forEach(r => r());
  }
}

// ----------------------------------------------------------------------------
// Object-model bridge.
// ----------------------------------------------------------------------------
//
//   gsEval(path)            → attribute read or zero-arg method call
//   gsEval(path, [v0, v1])  → method call, or attribute write when path
//                             is an attribute and the array has one entry
//   gsInspect(path)         → same JSON shape as gsEval; recursive
//                             subtree expansion is in progress (M11+)
//
// args may be undefined / null / an array of primitive values. The C
// side parses the JSON-encoded array.
//
// Every JS→C call routes through the bridge so the body runs on the
// worker pthread that owns scheduler / device / OPFS state. Do NOT add
// a direct ccall — see the threading-model comment at the top of this
// file for why.

function readBridgeOutput() {
  if (!bridgePtr) return null;
  const jsonStr = Module.UTF8ToString(bridgePtr + OFF_OUTPUT);
  if (!jsonStr) return null;
  try { return JSON.parse(jsonStr); } catch (e) { return jsonStr; }
}

// Wait for the worker to be ready to dispatch bridge requests. With
// PROXY_TO_PTHREAD, createModule resolves while the worker is still
// inside main() — sending a request during that window would dispatch
// against the empty default root class. main() stores 1 into the
// `ready` field and calls emscripten_atomic_notify; we park in
// Atomics.waitAsync until then (or return immediately if the worker
// beat us to it).
async function waitForBridgeReady() {
  if (!bridgePtr) return;
  const idx = (bridgePtr + OFF_READY) >> 2;
  const w = Atomics.waitAsync(Module.HEAP32, idx, 0);
  if (w.async) await w.value;
}

// Drive a single gs_eval / gs_inspect request through the bridge.
// kind: 1 = gs_eval (uses both path + argsJson), 2 = gs_inspect (path only).
async function executeGsRequest(path, argsJson, kind) {
  while (cmdInFlight) {
    await new Promise(r => cmdWaiters.push(r));
  }
  cmdInFlight = true;
  try {
    Module.stringToUTF8(String(path || ''), bridgePtr + OFF_PATH, PATH_SIZE);
    Module.stringToUTF8(argsJson || '', bridgePtr + OFF_ARGS, ARGS_SIZE);

    Atomics.store(Module.HEAP32, (bridgePtr + OFF_DONE) >> 2, 0);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_PENDING) >> 2, kind);

    await waitForBridgeDone();

    return readBridgeOutput();
  } finally {
    cmdInFlight = false;
    const waiters = cmdWaiters.splice(0);
    waiters.forEach(r => r());
  }
}

export async function gsEval(path, args) {
  if (!Module || !moduleReady) return null;
  await waitForBridgeReady();
  const argsJson = (args === undefined || args === null) ? '' : JSON.stringify(args);
  try {
    return await executeGsRequest(String(path || ''), argsJson, 1);
  } catch (e) {
    return null;
  }
}

export async function gsInspect(path) {
  if (!Module || !moduleReady) return null;
  await waitForBridgeReady();
  try {
    return await executeGsRequest(String(path || ''), '', 2);
  } catch (e) {
    return null;
  }
}

// Probe a ROM file via rom.identify and return the parsed info map:
//   { recognised, compatible, checksum, name, size }
// Returns null when the path can't be opened (V_ERROR on the C side).
// recognised==false (with empty compatible / name) is a successful probe of
// an unrecognised ROM and is *not* an error.
export async function romIdentify(path) {
  const r = await gsEval('rom.identify', [path]);
  if (r === null || r === undefined) return null;
  if (typeof r === 'object' && 'error' in r) return null;
  if (typeof r !== 'string') return null;
  try { return JSON.parse(r); } catch (e) { return null; }
}

// Look up a static machine profile by id and return the parsed config map.
// Returns null when id doesn't match a registered profile.
export async function machineProfile(id) {
  const r = await gsEval('machine.profile', [id]);
  if (r === null || r === undefined) return null;
  if (typeof r === 'object' && 'error' in r) return null;
  if (typeof r !== 'string') return null;
  try { return JSON.parse(r); } catch (e) { return null; }
}
