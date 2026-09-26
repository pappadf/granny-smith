// Real emulator bus — boots the WASM Module and exposes `gsEval` over the
// SAB-backed js_bridge_t slot. Port of app/web/js/emulator.js.
//
// THREADING — read before adding any new JS→C call site.
//
// With -sPROXY_TO_PTHREAD, main() / shell_init() / scheduler / device state
// all live on the WORKER thread. Direct Module.ccall from the main thread
// races that state. Every JS→C call MUST route through the SAB-backed
// bridge slot (`pending=1`, single in-flight). See docs/web.md.

import {
  machine,
  setSchedulerMode,
  setAcceleratedSpeed,
  setCheckpointSaved,
  setDriveActivity,
  setPerfStats,
  type MachineStatus,
  type SchedulerMode,
} from '@/state/machine.svelte';
import { onFloppyDriveChange } from '@/state/images.svelte';
import { onVideoInReady, onVideoInState } from '@/state/camera.svelte';
import { onAudioInReady, onAudioInState, onAudioInInjected } from '@/state/microphone.svelte';
import { showNotification } from '@/state/toasts.svelte';
import {
  onVoodooGpuAttach,
  onVoodooGpuDetach,
  onVoodooGpuOverlay,
  whenVoodooGpuReady,
} from '@/gpu/voodoo2Gpu.svelte';
import { onPrinterAttach } from '@/printer/platen';
// The audio-out worklet, bundled on its own (em_audio.c loads it).
import gsAudioWorkletUrl from '@/audio/gsAudio.worklet.ts?worker&url';
import { getOrCreateMachine } from '@/lib/machineId';
import { routePrintLine, routeLogEmit } from './logSink';
import { bridgeBusy } from '@/state/activity.svelte';

const BRIDGE_VERSION = 7;
const OFF_VERSION = 0;
const OFF_READY = 4;
const OFF_PENDING = 8;
const OFF_DONE = 12;
const OFF_PATH = 20;
const OFF_ARGS = 1044;
const OFF_OUTPUT = 9236;
const OFF_GPU_AVAILABLE = 271380; // int32: the Voodoo2 takeover has a WebGPU device
const PATH_SIZE = 1024;
const ARGS_SIZE = 8192;

// Minimal Emscripten module surface — enough to type-check what bus uses.
interface EmscriptenModule {
  HEAP16: Int16Array<ArrayBufferLike>;
  HEAP32: Int32Array<ArrayBufferLike>;
  HEAPU8: Uint8Array<ArrayBufferLike>;
  FS: {
    writeFile(path: string, data: Uint8Array<ArrayBufferLike>): void;
    unlink(path: string): void;
    mkdir(path: string): void;
    stat(path: string): unknown;
    readdir(path: string): string[];
    readFile(path: string): Uint8Array<ArrayBufferLike>;
    // Streaming write API (Emscripten legacy-compat FS surface). open() returns
    // a stream handle; write() copies `length` bytes from `buffer[offset..]` to
    // the file at `position`. Used to stage uploads chunk-by-chunk into OPFS on
    // the worker without buffering the whole file. See bus/upload.ts.
    open(path: string, flags: string): unknown;
    write(
      stream: unknown,
      buffer: Uint8Array<ArrayBufferLike>,
      offset: number,
      length: number,
      position?: number,
    ): number;
    read(
      stream: unknown,
      buffer: Uint8Array<ArrayBufferLike>,
      offset: number,
      length: number,
      position?: number,
    ): number;
    close(stream: unknown): void;
  };
  _get_js_bridge(): number;
  // Returns the bytes written, excluding the terminating NUL.
  stringToUTF8(s: string, ptr: number, max: number): number;
  UTF8ToString(ptr: number): string;
}

interface EmscriptenModuleConfig {
  canvas: HTMLCanvasElement;
  mainScriptUrlOrBlob?: string;
  locateFile?(path: string): string;
  print?(s: string): void;
  printErr?(s: string): void;
  // Called by the glue's abort() (needs "onAbort" in INCOMING_MODULE_JS_API).
  onAbort?(what: unknown): void;
  onRunStateChange?(running: boolean): void;
  onScreenResize?(w: number, h: number, parW?: number, parH?: number): void;
  onLogEmit?(line: string): void;
  onFloppyChange?(drive: number, present: boolean): void;
  onSchedulerSpeed?(speedX256: number): void;
  onPerfUpdate?(mipsX100: number, tpsX10: number): void;
  onCheckpointSaved?(elapsedMsX100: number): void;
  onDriveActivity?(kind: number, state: number): void;
  onVideoInReady?(ptr: number): void;
  onVideoInState?(active: boolean): void;
  onAudioInReady?(ptr: number): void;
  onAudioInState?(active: boolean): void;
  onAudioInInjected?(path: string): void;
  onVoodooGpuAttach?(ctrl: number, bytes: number): void;
  onVoodooGpuDetach?(ctrl: number): void;
  onVoodooGpuOverlay?(visible: number): void;
  onPrinterAttach?(ctrl: number, version: string): void;
  // The audio-out AudioWorklet module (em_audio.c addModule()s it).
  gsAudioWorkletUrl?: string;
}

type CreateModule = (config: EmscriptenModuleConfig) => Promise<EmscriptenModule>;

let Module: EmscriptenModule | null = null;
let moduleReady = false;
let bridgePtr = 0;
let cmdInFlight = false;
const cmdWaiters: Array<() => void> = [];

// Single-source-of-truth ready signal. Consumers `await whenModuleReady()`
// rather than polling isModuleReady() — bootstrap() resolves this exactly
// when moduleReady flips to true (and the machine.register bridge call
// has completed), and REJECTS it when the emulator cannot start: a bridge
// version mismatch, a module that fails to load, or a worker that never
// comes up.  Before this, a failure left the promise pending forever — URL
// media never ran, the New Machine dialog stayed on "Scanning ROMs…", and a
// worker that never started produced no message at all (F-37, N-58).
export type BootState =
  | { phase: 'starting' }
  | { phase: 'ready' }
  | { phase: 'failed'; reason: string };
let bootState: BootState = { phase: 'starting' };
let resolveReady: (() => void) | null = null;
let rejectReady: ((e: Error) => void) | null = null;
const readyPromise: Promise<void> = new Promise((res, rej) => {
  resolveReady = res;
  rejectReady = rej;
});
// A failure may land before anyone awaits: never report it as unhandled.
readyPromise.catch(() => undefined);

export function whenModuleReady(): Promise<void> {
  return readyPromise;
}

export function getBootState(): BootState {
  return bootState;
}

// Mark the boot failed, once, and say why to every waiter — including
// automation, which waits on window.__gsReady or __gsBootError.
function failBoot(reason: string): void {
  if (bootState.phase !== 'starting') return;
  bootState = { phase: 'failed', reason };
  (window as unknown as { __gsBootError?: string }).__gsBootError = reason;
  rejectReady?.(new Error(reason));
}

// The worker sets the bridge's `ready` word once it can dispatch.  A pthread
// that never starts (a stale worker script, a crash at load) never sets it,
// and nothing else would ever notice.  Wait in slices and fail after this
// much *visible* time: a background tab throttles the worker, so hidden time
// is not evidence of anything.
const WORKER_READY_BUDGET_MS = 30_000;
async function waitForWorkerReady(): Promise<void> {
  if (!Module || !bridgePtr) throw new Error('emulator module not loaded');
  const idx = (bridgePtr + OFF_READY) >> 2;
  let visibleMs = 0;
  while (Atomics.load(Module.HEAP32, idx) === 0) {
    const slice = 1_000;
    const w = Atomics.waitAsync(Module.HEAP32, idx, 0, slice);
    const outcome = w.async ? await w.value : w.value;
    if (outcome !== 'timed-out') continue;
    if (document.visibilityState === 'visible') visibleMs += slice;
    if (visibleMs >= WORKER_READY_BUDGET_MS)
      throw new Error(
        `the emulator worker did not start within ${WORKER_READY_BUDGET_MS / 1000} s`,
      );
  }
}

// Run-state mirror so we can ignore redundant transitions.
let isRunningUI = false;
let lastScreenW = 0;
let lastScreenH = 0;
let lastScreenParW = 0;
let lastScreenParH = 0;

// --- Bootstrap ----------------------------------------------------------

// Initialise the WASM module. The canvas is handed to Emscripten; subsequent
// resize callbacks update machine.screen so ScreenView can reflow.
export async function bootstrap(canvas: HTMLCanvasElement): Promise<void> {
  if (moduleReady || bootState.phase === 'failed') return;
  try {
    await bootstrapModule(canvas);
  } catch (e) {
    failBoot(e instanceof Error ? e.message : String(e));
    throw e;
  }
}

// bootstrap()'s body: load the module, check the bridge, wait for the worker.
async function bootstrapModule(canvas: HTMLCanvasElement): Promise<void> {
  const bust = Date.now();
  // Resolve main.mjs / main.wasm against the document base URL, not
  // origin-rooted. Dynamic `import()` resolves relative URLs against
  // the importing module's URL, not the document — so a bare
  // `./main.mjs` from inside the Vite-bundled chunk under
  // /gs-pages/latest/assets/ ends up at /gs-pages/latest/assets/main.mjs
  // (404). Hand it a fully-qualified URL instead so it works under
  // any deploy path. main.wasm is fetched via Emscripten's locateFile
  // (regular fetch — resolves against document.baseURI naturally) but
  // we treat it the same way for consistency.
  const url = new URL(`main.mjs?v=${bust}`, document.baseURI).href;
  const mod = (await import(/* @vite-ignore */ url)) as { default: CreateModule };
  const createModule = mod.default;

  Module = await createModule({
    canvas,
    // Pthread workers must load the exact same main.mjs URL as the main
    // thread. Without this, Emscripten spawns them with
    // `new URL('main.mjs', import.meta.url)` — the literal filename, which
    // drops the `?v=` cache-buster. The main thread then runs the freshly
    // deployed module while the worker gets a stale `main.mjs` from the
    // browser/CDN cache (GitHub Pages caches for 600 s), and instantiating
    // the new wasm against the old worker JS crashes the worker at load
    // ("__emscripten_thread_crashed is not a function").
    mainScriptUrlOrBlob: url,
    locateFile: (p: string) =>
      p.endsWith('.wasm') ? new URL(`main.wasm?v=${bust}`, document.baseURI).href : p,
    print: routePrintLine,
    printErr: routeErrLine,
    onAbort: (what: unknown) => markBridgeDead(`Aborted(${String(what ?? '')})`),
    onRunStateChange: handleRunStateChange,
    onScreenResize: handleScreenResize,
    onLogEmit: routeLogEmit,
    onFloppyChange: onFloppyDriveChange,
    onSchedulerSpeed: handleSchedulerSpeed,
    onPerfUpdate: handlePerfUpdate,
    onCheckpointSaved: handleCheckpointSaved,
    onDriveActivity: setDriveActivity,
    onVideoInReady,
    onVideoInState,
    onAudioInReady,
    onAudioInState,
    onAudioInInjected,
    onVoodooGpuAttach,
    onVoodooGpuDetach,
    onVoodooGpuOverlay,
    onPrinterAttach,
    gsAudioWorkletUrl,
  });

  bridgePtr = Module._get_js_bridge();
  const v = Module.HEAP32[(bridgePtr + OFF_VERSION) >> 2];
  if (v !== BRIDGE_VERSION) {
    throw new Error(`js_bridge version mismatch: C=${v}, JS=${BRIDGE_VERSION}`);
  }
  // Tell the core whether the Voodoo2 takeover has a WebGPU device
  // (the worker was started by ScreenView before the module; its answer
  // is normally in long before a machine boots).
  void whenVoodooGpuReady().then((ok) => {
    if (Module && bridgePtr)
      Atomics.store(Module.HEAP32, (bridgePtr + OFF_GPU_AVAILABLE) >> 2, ok ? 1 : 0);
  });
  // Nothing is sent until the worker says it can dispatch; a worker that
  // never comes up fails the boot here instead of parking the first
  // request forever.
  await waitForWorkerReady();
  moduleReady = true;

  // Activate per-machine checkpoint directory before anything that opens
  // images. Matches app/web/js/main.js:81-83.
  const id = getOrCreateMachine();
  await gsEval('machine.register', [id.id, id.created]);

  // Resolve the public ready signal — TerminalPane (and anyone else
  // who needs the bridge live) is awaiting this.
  bootState = { phase: 'ready' };
  resolveReady?.();
}

export function isModuleReady(): boolean {
  return moduleReady;
}

// --- gsEval -------------------------------------------------------------

// The result contract (A1):
//   - a value     — the method or attribute's result;
//   - null        — ONLY a successful method that returns nothing (V_NONE);
//   - { error }   — failure.  A C-side V_ERROR carries the core's message;
//                   a failure of the bridge itself (module not ready, a
//                   thrown request) also sets `transport: true`.
// So `r !== null` is never a success test: `{ error }` satisfies it.  Use
// gsOk() for "did it work", `=== true` for a V_BOOL method, and a shape check
// for a read.
export interface GsError {
  error: string;
  transport?: true;
}

// A bridge-level failure, distinct from an error the core returned.
function transportError(message: string): GsError {
  return { error: message, transport: true };
}

export async function gsEval(
  path: string,
  args?: unknown[] | Record<string, unknown>,
): Promise<unknown> {
  if (bridgeDead) return transportError(`emulator crashed: ${bridgeDead}`);
  if (!Module || !moduleReady) return transportError('emulator not ready');
  await waitForBridgeReady();
  // An array is positional; a plain object binds by declared argument name
  // (proposal-named-args-boot-config §3.4).
  const argsJson = args === undefined || args === null ? '' : JSON.stringify(args);
  // Refuse before touching the shared slot: a too-large request is the
  // caller's error, not the bridge's, so it carries no `transport` flag.
  const tooLarge = requestTooLarge(path || '', argsJson);
  if (tooLarge) return { error: tooLarge };
  try {
    return await executeGsRequest(path || '', argsJson);
  } catch (e) {
    return transportError(`bridge request failed: ${e instanceof Error ? e.message : String(e)}`);
  }
}

// Why a request cannot be sent, or null if it fits.  The bridge's path and
// args buffers are fixed-size (em.h), and stringToUTF8 silently truncates to
// fit — an args document cut after a ',' used to run with its trailing
// arguments dropped.  Measure in UTF-8 bytes, leaving room for the NUL.
const utf8 = new TextEncoder();
export function requestTooLarge(path: string, argsJson: string): string | null {
  const pathBytes = utf8.encode(path).length;
  if (pathBytes > PATH_SIZE - 1)
    return `request path too large (${pathBytes} bytes > ${PATH_SIZE - 1})`;
  const argsBytes = utf8.encode(argsJson).length;
  if (argsBytes > ARGS_SIZE - 1)
    return `request arguments too large (${argsBytes} bytes > ${ARGS_SIZE - 1})`;
  return null;
}

// --- Slow is not dead (A6) ---------------------------------
//
// The bridge has one slot and no request id, so a slow request is never
// abandoned on a timer: freeing the slot while the worker is still inside the
// old request would hand its reply to the next caller and erase that caller's
// request.  Instead a request that runs long raises a status-bar notice, and
// only a real crash — a wasm trap or abort on the worker — ends the bridge.
// (Cancelling a request needs a per-request id the bridge does not have; an
// out-of-band interrupt word was considered as a stopgap and declined.)

// Paths that are legitimately long: the notice waits longer for them.
const LONG_REQUEST =
  /^(checkpoint\.|machine\.(boot|restart)|storage\.(cp|mv|hd_create)|archive\.|download$|vfs\.)/;
const BUSY_AFTER_MS = 5_000;
const BUSY_AFTER_LONG_MS = 30_000;
// An ordinary request still in flight after this much visible time is not
// slow, it is wedged (on wasm `scheduler.run` returns at once, so only a
// runaway — a script loop that can never finish — gets here).  The caller is
// rejected and the bridge marked dead; the slot is never reused, so no later
// reply can be misattributed.  Known-long requests have no deadline.
const DEADLINE_MS = 120_000;

// Count visible time a request has been in flight; raise the notice past its
// threshold.  Background tabs throttle the worker, so hidden time is not
// counted.  Returns a stop function.
export function watchRequest(path: string): () => void {
  const limit = LONG_REQUEST.test(path) ? BUSY_AFTER_LONG_MS : BUSY_AFTER_MS;
  let visibleMs = 0;
  const tick = 1_000;
  const timer = setInterval(() => {
    if (document.visibilityState !== 'visible') return;
    visibleMs += tick;
    if (visibleMs >= limit) {
      bridgeBusy.path = path;
      bridgeBusy.seconds = Math.round(visibleMs / 1000);
    }
    if (!LONG_REQUEST.test(path) && visibleMs >= DEADLINE_MS)
      markBridgeDead(`'${path}' did not complete within ${DEADLINE_MS / 1000} s`);
  }, tick);
  return () => {
    clearInterval(timer);
    if (bridgeBusy.path === path) bridgeBusy.path = null;
  };
}

// A dead worker: a wasm trap (the glue's worker.onerror prints "worker sent
// an error!") or an explicit abort (Module.onAbort).  Once dead, every
// in-flight and queued request fails at once instead of waiting forever,
// and the page is told so it can say why (onEmulatorCrash).
let bridgeDead: string | null = null;
let resolveDead: ((reason: string) => void) | null = null;
const deadSignal: Promise<string> = new Promise((res) => {
  resolveDead = res;
});
const crashListeners: Array<(reason: string) => void> = [];

export function onEmulatorCrash(cb: (reason: string) => void): void {
  crashListeners.push(cb);
}

export function markBridgeDead(reason: string): void {
  if (bridgeDead) return;
  bridgeDead = reason;
  machine.status = 'crashed';
  resolveDead?.(reason);
  for (const cb of crashListeners) cb(reason);
}

// printErr hook: recognise the glue's crash lines, then route as usual.
const WORKER_CRASH = /worker sent an error!|^Aborted\(/;
export function isWorkerCrashLine(line: string): boolean {
  return WORKER_CRASH.test(line);
}
function routeErrLine(line: string): void {
  if (isWorkerCrashLine(line)) markBridgeDead(line);
  routePrintLine(line);
}

// True for any failure shape — the core's V_ERROR or a transport failure.
export function isGsError(res: unknown): res is GsError {
  return !!res && typeof res === 'object' && 'error' in res;
}

// "Did the call work?": not an error, and not a V_BOOL method's `false`.
// A V_NONE success (null) counts as success.
export function gsOk(res: unknown): boolean {
  return !isGsError(res) && res !== false;
}

// Human-readable reason from a failed gsEval result. Single home for the
// error-shape knowledge so callers don't each re-implement the check.
export function gsErrorText(res: unknown): string {
  if (isGsError(res)) return String(res.error);
  if (res === false) return 'the operation reported failure';
  if (res === null) return 'no result';
  return String(res);
}

// --- Shell line surface (Terminal pane only) ----------------------------
//
// The Terminal view is the single caller of `shell.run` — every other
// component reaches the core through typed object-model paths via
// gsEval. The proposal-shell-as-object-model-citizen.md §5.3 ESLint
// rule pins this; only TerminalPane.svelte may construct shell-line
// strings.

let cachedPrompt: string | null = null;

// Execute a free-form shell line. Returns 0 on success, -1 on dispatch
// failure. The new prompt is returned from `shell.run` as a V_STRING and
// cached for getRuntimePrompt(). This is the *only* call to `shell.run`
// allowed in src/bus/** — the no-restricted-syntax rule pins that, and
// the disable below is the single sanctioned exception (forwarded from
// TerminalPane.svelte, the only legitimate caller).
export async function gsEvalLine(line: string): Promise<number> {
  if (!moduleReady) return -1;
  const text = (line ?? '').toString();
  if (!text.trim()) return 0;
  // eslint-disable-next-line no-restricted-syntax
  const r = await gsEval('shell.run', [text]);
  if (typeof r === 'string') {
    cachedPrompt = r.length ? r : null;
    return 0;
  }
  return -1;
}

export function getRuntimePrompt(): string | null {
  return cachedPrompt;
}

// Seed the cached prompt from the C-side `shell.prompt` attribute.
// Called once by TerminalPane on mount so the first prompt is visible
// before any user input. After this, gsEvalLine keeps cachedPrompt in
// sync via the return value of `shell.run`.
export async function seedPrompt(): Promise<void> {
  if (!moduleReady) return;
  const r = await gsEval('shell.prompt');
  if (typeof r === 'string' && r.length) cachedPrompt = r;
}

export async function shellInterrupt(): Promise<void> {
  if (!moduleReady) return;
  await gsEval('shell.interrupt');
}

export interface CompletionResult {
  candidates: string[];
  span: { start: number; end: number };
}

export async function tabComplete(line: string, cursor: number): Promise<CompletionResult | null> {
  if (!moduleReady) return null;
  const r = await gsEval('shell.complete', [line, cursor]);
  if (r && typeof r === 'object') {
    const obj = r as { candidates?: unknown; span?: unknown };
    if (Array.isArray(obj.candidates) && obj.span && typeof obj.span === 'object') {
      const span = obj.span as { start?: unknown; end?: unknown };
      if (typeof span.start === 'number' && typeof span.end === 'number') {
        return {
          candidates: obj.candidates.filter((s): s is string => typeof s === 'string'),
          span: { start: span.start, end: span.end },
        };
      }
    }
  }
  return null;
}

async function waitForBridgeReady(): Promise<void> {
  if (!bridgePtr || !Module) return;
  const idx = (bridgePtr + OFF_READY) >> 2;
  const w = Atomics.waitAsync(Module.HEAP32, idx, 0);
  if (w.async) await w.value;
}

async function waitForBridgeDone(): Promise<void> {
  if (!Module) return;
  const doneIdx = (bridgePtr + OFF_DONE) >> 2;
  const w = Atomics.waitAsync(Module.HEAP32, doneIdx, 0);
  if (w.async) await w.value;
  Atomics.store(Module.HEAP32, doneIdx, 0);
}

function readBridgeOutput(): unknown {
  if (!Module || !bridgePtr) return null;
  const s = Module.UTF8ToString(bridgePtr + OFF_OUTPUT);
  if (!s) return null;
  try {
    return JSON.parse(s);
  } catch {
    return s;
  }
}

async function executeGsRequest(path: string, argsJson: string): Promise<unknown> {
  while (cmdInFlight) {
    await new Promise<void>((r) => cmdWaiters.push(r));
  }
  cmdInFlight = true;
  let stopWatch: (() => void) | null = null;
  try {
    // A queued request that wakes after a crash fails at once.
    if (bridgeDead) return transportError(`emulator crashed: ${bridgeDead}`);
    // Never write a request at heap address 0 (+offset): the bridge pointer
    // is set before moduleReady, so this is a guard, not a code path.
    if (!Module || !bridgePtr) return transportError('emulator not ready');
    Module.stringToUTF8(path, bridgePtr + OFF_PATH, PATH_SIZE);
    Module.stringToUTF8(argsJson, bridgePtr + OFF_ARGS, ARGS_SIZE);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_DONE) >> 2, 0);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_PENDING) >> 2, 1);
    stopWatch = watchRequest(path);
    // A dead worker never sets `done`: race the completion against the crash.
    const crashed = await Promise.race([waitForBridgeDone().then(() => null), deadSignal]);
    if (crashed !== null) return transportError(`emulator crashed: ${crashed}`);
    return readBridgeOutput();
  } finally {
    stopWatch?.();
    cmdInFlight = false;
    const next = cmdWaiters.shift();
    if (next) next();
  }
}

// --- C→JS push callbacks -----------------------------------------------

function handleRunStateChange(running: boolean): void {
  const r = Boolean(running);
  if (r === isRunningUI) return;
  isRunningUI = r;
  // Don't clobber 'stopped' / 'no-machine' here; only mirror running↔paused
  // when we know a machine is live.
  if (r) machine.status = 'running';
  else if (machine.status === 'running') machine.status = 'paused';
}

// Core-pushed accelerated-mode effective CPU speed (x256; 256 = 1x). Edge-
// driven on the governor's rung transitions — the status bar shows it in
// Accelerated mode. Divide by 256 for the multiplier.
function handleSchedulerSpeed(speedX256: number): void {
  setAcceleratedSpeed((speedX256 | 0) / 256);
}

// Core-pushed performance metrics, ~1 Hz (perf proposal P12): emulated MIPS
// from instr_count deltas and the RAF tick rate. Fixed-point on the wire
// (x100 / x10) since MAIN_THREAD_ASYNC_EM_ASM carries ints.
function handlePerfUpdate(mipsX100: number, tpsX10: number): void {
  setPerfStats((mipsX100 | 0) / 100, (tpsX10 | 0) / 10);
}

// Core-pushed quick/background checkpoint completion (elapsed ms x100 —
// MAIN_THREAD_ASYNC_EM_ASM carries ints). The status bar flashes its CP
// glyph and carries the time + duration in the tooltip.
function handleCheckpointSaved(elapsedMsX100: number): void {
  setCheckpointSaved((elapsedMsX100 | 0) / 100);
}

// Also called with the live geometry after a checkpoint restore, where no
// resize is pushed (bus/boot.ts reconcileUiWithMachine).
export function handleScreenResize(w: number, h: number, parW?: number, parH?: number): void {
  const width = w | 0;
  const height = h | 0;
  // Pixel aspect ratio (display pixel width:height). 0/undefined => square 1:1.
  const pw = (parW ?? 0) | 0 || 1;
  const ph = (parH ?? 0) | 0 || 1;
  if (
    width === lastScreenW &&
    height === lastScreenH &&
    pw === lastScreenParW &&
    ph === lastScreenParH
  )
    return;
  lastScreenW = width;
  lastScreenH = height;
  lastScreenParW = pw;
  lastScreenParH = ph;
  machine.screen.width = width;
  machine.screen.height = height;
  machine.screen.parW = pw;
  machine.screen.parH = ph;
}

// --- Module access for upload pipeline (FS writes to /tmp) -------------

export function getModule(): EmscriptenModule | null {
  return Module;
}

// Fresh heap views for direct shared-memory writers (the camera frame
// transport). Fetched per use — under ALLOW_MEMORY_GROWTH the underlying
// buffer can be replaced, so callers must never cache these.
export function getModuleHeap(): { u8: Uint8Array; i16: Int16Array; i32: Int32Array } | null {
  if (!Module || !moduleReady) return null;
  return { u8: Module.HEAPU8, i16: Module.HEAP16, i32: Module.HEAP32 };
}

// --- Lifecycle wrappers --------------------------------------------------

// === PRAM seeding ============================================================
// Never boot with blank PRAM.  A real Mac's PRAM is battery-backed; ours
// starts empty on every machine.boot, so the guest takes its
// PRAM-is-invalid paths every session.  On the PDM machines that means
// the Start Manager's full startup-drive discovery wait, and under
// Mac OS 8.1 additionally a one-time "select the DR 68k emulator and
// restart" (MMFlags bit 5) — the reported double boot with two chimes on
// every session.  Seeding a deterministic, valid PRAM at boot removes
// both, with no persistent state to go stale (integration tests seed the
// same way; see docs/core/memory/pram.md).
//
// Verified on the pm6100 against both System 7.5 (boots straight to the
// Finder, no discovery wait) and Mac OS 8.1 (single chime, no restart).
// Other families keep their ROM's own PRAM init until they get the same
// verification — their ROMs initialize PRAM without restarting.
const PRAM_SEEDED_MODELS = new Set(['pm6100', 'pm7100', 'pm8100']);

export async function seedPram(model: string, scsiId = 0): Promise<void> {
  if (!PRAM_SEEDED_MODELS.has(model)) return;
  // Stamp the two boot-ROM validity tokens ($A8 + 'NuMc') so the ROM's
  // PRAMInit leaves the seeded bytes alone (pram.md §3).
  await gsEval('machine.rtc.pram.validate');
  // XPRAM $01 is the Start Manager wait byte (StartSearch.a): bits 0-4 =
  // spin-up timeout seconds (0 = pristine -> 20 s default), bit 7 =
  // disable the dynamic wait.  On single-Curio machines the startup-device
  // poll can never succeed (a ROM HAL bug — scsi-53c96.md §8.2), so the
  // wait always runs to full expiry before the drive-queue fallback boots;
  // our disk is ready instantly, so skip the wait outright.
  await gsEvalLine('machine.rtc.pram.poke 0x01 0x80:1');
  // Start Manager defaults (pram.md §4.2): default OS, and the boot
  // device as the SCSI driver refnum (-(33+id)) of the configured disk so
  // the Start Manager goes straight to it.
  const refnum = (0xffdf - (scsiId & 7)).toString(16).toUpperCase().padStart(4, '0');
  await gsEvalLine('machine.rtc.pram.poke 0x77 0x01:1');
  await gsEvalLine(`machine.rtc.pram.poke 0x78 0xFFFF${refnum}:4`);
  // MMFlags: the PDM ROM's own default ($05) plus bit 5, which Mac OS 8.1
  // reads as "the DR emulator is already selected" — without it 8.1 sets
  // the bit and soft-restarts on every boot.  System 7.5 ignores it.
  await gsEvalLine('machine.rtc.pram.poke 0x8A 0x25:1');
}

// Toggle the Caps Lock latch: UI state plus an immediate push to the live
// machine (down latches, up releases). The latch is re-asserted after every
// boot/restart, which is how Copland D11E4's diverted boot is reached from
// the UI: turn the latch on, then Restart (or boot the machine).
export async function setCapsLock(on: boolean): Promise<void> {
  machine.capsLock = on;
  if (!isModuleReady() || machine.status === 'no-machine') return;
  const r = await gsEval(on ? 'machine.adb.keyboard.down' : 'machine.adb.keyboard.up', [
    'capslock',
  ]);
  if (r !== true) showNotification(`Caps Lock: ${gsErrorText(r)}`, 'warning');
}

export async function shutdownEmulator(): Promise<void> {
  await gsEval('scheduler.stop');
  machine.status = 'stopped' as MachineStatus;
  showNotification('Machine stopped', 'info');
}

export async function pauseEmulator(): Promise<void> {
  await gsEval('scheduler.stop');
  // onRunStateChange handler reflects the new state.
}

export async function resumeEmulator(): Promise<void> {
  await gsEval('scheduler.run');
}

// UI mode name → core `scheduler.mode` value.
const CORE_MODE: Record<SchedulerMode, string> = {
  live: 'paced',
  accel: 'accelerated',
  turbo: 'turbo',
};

// Push a pacing-mode change to the core and mirror it into UI state. The
// toolbar buttons route through here so they actually reach the scheduler
// (the pre-two-modes buttons only flipped local UI state).
export async function applySchedulerMode(mode: SchedulerMode): Promise<void> {
  const res = await gsEval('scheduler.mode', [CORE_MODE[mode]]);
  if (res && typeof res === 'object' && 'error' in res) {
    showNotification(`Scheduler mode failed: ${gsErrorText(res)}`, 'warning');
    return;
  }
  setSchedulerMode(mode);
}

// Save State button path. Writes to /tmp/saved-state-<ts>.bin, then triggers
// a browser download via the C-side `download` shell command.
