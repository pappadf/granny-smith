// Real emulator bus — boots the WASM Module and exposes `gsEval` over the
// mailbox in shared memory (src/core/mailbox/mailbox.h; bus/mailbox.ts).
//
// THREADING — read before adding any new JS→C call site.
//
// With -sPROXY_TO_PTHREAD, main() / shell_init() / scheduler / device state
// all live on the WORKER thread. Direct Module.ccall from the main thread
// races that state. Every JS→C call MUST route through the SAB-backed
// bridge slot (`pending=1`, single in-flight). See docs/guide/web.md.

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
import { setPrinterStatus } from '@/state/printer.svelte';
import { onDownloadChunk } from './download';
// The audio-out worklet, bundled on its own (em_audio.c loads it).
import gsAudioWorkletUrl from '@/audio/gsAudio.worklet.ts?worker&url';
import { getOrCreateMachine } from '@/lib/machineId';
import { routePrintLine, routeErrorLine, routeConsole, routeLogEmit } from './logSink';
import { utf16ToUtf8, utf8ToUtf16 } from '@/lib/utf8';
import { bridgeBusy } from '@/state/activity.svelte';
import {
  Mailbox,
  PATH_MAX,
  ARGS_MAX,
  EVT_STATE,
  EVT_NOTIFY,
  EVT_LOG,
  type MailboxFailure,
} from './mailbox';

// The client id this page writes into every request (the daemon and the
// script runner will have their own).
const CLIENT_PAGE = 1;

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
  _get_gs_mailbox(): number;
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
  onScreenResize?(w: number, h: number, parW?: number, parH?: number): void;
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
// The mailbox: the control block and two rings in the wasm heap through
// which every request travels (bus/mailbox.ts).  Bound in bootstrap once
// the module is up; null before that and after a crash.
let mailbox: Mailbox | null = null;

// Single-source-of-truth ready signal. Consumers `await whenModuleReady()`
// rather than polling isModuleReady() — bootstrap() resolves this exactly
// when moduleReady flips to true (and the machine.register bridge call
// has completed), and REJECTS it when the emulator cannot start: a bridge
// version mismatch, a module that fails to load, or a worker that never
// comes up.  Before this, a failure left the promise pending forever — URL
// media never ran, the New Machine dialog stayed on "Scanning ROMs…", and a
// worker that never started produced no message at all.
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

// The worker sets the mailbox's READY word once it can dispatch.  A pthread
// that never starts (a stale worker script, a crash at load) never sets it,
// and nothing else would ever notice.  Wait in slices and fail after this
// much *visible* time: a background tab throttles the worker, so hidden time
// is not evidence of anything.
const WORKER_READY_BUDGET_MS = 30_000;
async function waitForWorkerReady(): Promise<void> {
  if (!Module || !mailbox) throw new Error('emulator module not loaded');
  let visibleMs = 0;
  while (!mailbox.isReady()) {
    const slice = 1_000;
    const outcome = await mailbox.waitReady(slice);
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
    onScreenResize: handleScreenResize,
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

  // Bind the mailbox (throws on a MAGIC / VERSION mismatch: page and core
  // out of step).  The control block is laid out by a constructor in the
  // core, so it is valid before main() runs.
  // The rings are static and below the boot-time heap size; a staged buffer
  // (a spilled result, a download chunk) is anywhere in the heap, so the
  // mailbox reads those through the memory as it is at that moment.
  const memMod = Module as unknown as { wasmMemory?: WebAssembly.Memory; HEAPU8: Uint8Array };
  mailbox = new Mailbox(
    Module.HEAP32.buffer,
    Module._get_gs_mailbox(),
    CLIENT_PAGE,
    () => memMod.wasmMemory?.buffer ?? memMod.HEAPU8.buffer,
  );
  mailbox.setLostHandler((why) => markBridgeDead(why));
  mailbox.on(dispatchCoreEvent);
  // Tell the core whether the Voodoo2 takeover has a WebGPU device
  // (the worker was started by ScreenView before the module; its answer
  // is normally in long before a machine boots).
  void whenVoodooGpuReady().then((ok) => mailbox?.setGpuAvailable(ok));
  // Nothing is sent until the worker says it can dispatch; a worker that
  // never comes up fails the boot here instead of parking the first
  // request forever.
  await waitForWorkerReady();
  moduleReady = true;
  startHeartbeatWatch();

  // Activate per-machine checkpoint directory before anything that opens
  // images. Matches app/web/js/main.js:81-83.
  const id = getOrCreateMachine();
  await gsEval('machine.register', [id.id, id.created]);

  // Resolve the public ready signal — the console (and anyone else
  // who needs the bridge live) is awaiting this.
  bootState = { phase: 'ready' };
  resolveReady?.();
}

export function isModuleReady(): boolean {
  return moduleReady;
}

// --- gsEval -------------------------------------------------------------

// The result contract:
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
  if (!Module || !moduleReady || !mailbox) return transportError('emulator not ready');
  // An array is positional; a plain object binds by declared argument name.
  const argsJson = args === undefined || args === null ? '' : JSON.stringify(args);
  // Refuse before touching the ring: a too-large request is the caller's
  // error, not the mailbox's, so it carries no `transport` flag.
  const tooLarge = requestTooLarge(path || '', argsJson);
  if (tooLarge) return { error: tooLarge };
  return executeMailboxRequest(path || '', argsJson);
}

// gsEval for an I/O job (meta.method_info `io`: a copy, an export, an
// extraction, a download): `onProgress` gets `done` of `total` as the core
// reports it, and the answer comes when the work ends.
export async function gsEvalWithProgress(
  path: string,
  args: unknown[] | Record<string, unknown> | undefined,
  onProgress: (done: number, total: number) => void,
): Promise<unknown> {
  if (bridgeDead) return transportError(`emulator crashed: ${bridgeDead}`);
  if (!Module || !moduleReady || !mailbox) return transportError('emulator not ready');
  const argsJson = args === undefined || args === null ? '' : JSON.stringify(args);
  const tooLarge = requestTooLarge(path || '', argsJson);
  if (tooLarge) return { error: tooLarge };
  return executeMailboxRequest(path || '', argsJson, onProgress);
}

// A view of `len` bytes of the core's heap at `ptr`, fresh (a staged
// buffer named by an event), and the acknowledgement that releases it.
export function heapBytes(ptr: number, len: number): Uint8Array | null {
  return mailbox ? mailbox.heapBytes(ptr, len) : null;
}
export async function ackStagedBuffer(handle: number): Promise<boolean> {
  return mailbox ? mailbox.ackBuf(handle) : false;
}

// Refuse a request beyond the core's limits (mailbox.h): the core would
// answer it with an error anyway, and refusing here keeps the ring for
// requests that can be served.  Sizes are UTF-8 bytes, not characters.
// Returns the reason, or null when it fits.
const utf8 = new TextEncoder();
export function requestTooLarge(path: string, argsJson: string): string | null {
  const pathBytes = utf8.encode(path).length;
  if (pathBytes > PATH_MAX) return `request path too large (${pathBytes} bytes > ${PATH_MAX})`;
  const argsBytes = utf8.encode(argsJson).length;
  if (argsBytes > ARGS_MAX) return `request arguments too large (${argsBytes} bytes > ${ARGS_MAX})`;
  return null;
}

// --- Slow is not dead --------------------------------------
//
// Every request carries an id, so any number can be in flight and a slow
// one is nobody else's problem: a request that runs long raises a
// status-bar notice; an ordinary request past its deadline fails for its
// caller alone (the late answer, if it ever comes, is dropped by id); and
// a *dead* worker -- a wasm trap or abort, or a heartbeat that stops while
// requests are pending -- fails every request at once.

// Paths that are legitimately long: the notice waits longer for them.
const LONG_REQUEST =
  /^(checkpoint\.|machine\.(boot|restart|scsi\.device\[\d+\]\.image\.export|hd\.save)|files\.(cp|mv|import|export_raw|hd_create|xfer_|archive\.|download$|ls|list|mkdir|cat))/;
const BUSY_AFTER_MS = 5_000;
const BUSY_AFTER_LONG_MS = 30_000;
// An ordinary request still in flight after this long is not slow, it is
// stuck (a script loop that can never finish, until Phase 2 makes scripts
// cancellable): its caller gets a transport error and its id is forgotten.
// Known-long requests have no deadline.
export const DEADLINE_MS = 120_000;

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
  }, tick);
  return () => {
    clearInterval(timer);
    if (bridgeBusy.path === path) bridgeBusy.path = null;
  };
}

// The heartbeat: the core bumps a control word once per tick and once per
// idle-wait slice.  A page with requests pending that sees it stand still
// for this many visible seconds has a wedged worker -- a runaway script,
// a leaf that never returns -- and marks it dead, which fails every
// request at once (bridgeCrash.test.ts covers the crash half, this covers
// the silent one).  Hidden tabs are excluded: the RAF loop legitimately
// stops there.  `sample` is injected so the watch is unit-testable.
export const STALL_AFTER_S = 3;
export function watchHeartbeat(
  sample: () => { heartbeat: number; inFlight: number },
  onStall: (reason: string) => void,
): () => void {
  let last = -1;
  let still = 0;
  const timer = setInterval(() => {
    if (document.visibilityState !== 'visible') return;
    const s = sample();
    if (s.inFlight === 0 || s.heartbeat !== last) {
      last = s.heartbeat;
      still = 0;
      return;
    }
    still++;
    if (still >= STALL_AFTER_S) {
      onStall(`emulator not responding: no heartbeat for ${STALL_AFTER_S} s with requests pending`);
      clearInterval(timer);
    }
  }, 1_000);
  return () => clearInterval(timer);
}

function startHeartbeatWatch(): void {
  watchHeartbeat(
    () => ({ heartbeat: mailbox?.heartbeat() ?? -1, inFlight: mailbox?.inFlight() ?? 0 }),
    (reason) => markBridgeDead(reason),
  );
}

// A dead worker: a wasm trap (the glue's worker.onerror prints "worker sent
// an error!") or an explicit abort (Module.onAbort).  Once dead, every
// in-flight and queued request fails at once instead of waiting forever,
// and the page is told so it can say why (onEmulatorCrash).
let bridgeDead: string | null = null;
const crashListeners: Array<(reason: string) => void> = [];

export function onEmulatorCrash(cb: (reason: string) => void): void {
  crashListeners.push(cb);
}

export function markBridgeDead(reason: string): void {
  if (bridgeDead) return;
  bridgeDead = reason;
  machine.status = 'crashed';
  // Fails every pending and future request; the lost handler is this
  // function, and the guard above makes the re-entry a no-op.
  mailbox?.markLost('lost', reason);
  for (const cb of crashListeners) cb(reason);
}

// printErr hook: recognise the glue's crash lines, then route as usual.
const WORKER_CRASH = /worker sent an error!|^Aborted\(/;
export function isWorkerCrashLine(line: string): boolean {
  return WORKER_CRASH.test(line);
}
function routeErrLine(line: string): void {
  if (isWorkerCrashLine(line)) markBridgeDead(line);
  routeErrorLine(line);
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
// gsEval. An ESLint rule (eslint.config.js) pins this; only the console (state/console.svelte.ts) may construct shell-line
// strings.

let cachedPrompt: string | null = null;

// Execute a free-form shell line. Returns 0 on success, -1 on dispatch
// failure. The new prompt is returned from `shell.run` as a V_STRING and
// cached for getRuntimePrompt(). This is the *only* call to `shell.run`
// allowed in src/bus/** — the no-restricted-syntax rule pins that, and
// the disable below is the single sanctioned exception (forwarded from
// state/console.svelte.ts, the only legitimate caller).
// The terminal is its own client: a run it starts (`scheduler.run`) is
// its mode, and its Ctrl-C stops that and nothing else.
export const CLIENT_TERMINAL = 2;

// The terminal's foreground job: the script of the line it last
// submitted, until its result arrives.  Ctrl-C cancels it.
let foregroundJob: number | null = null;

// Runs a terminal line as a script job (REQ_SCRIPT): the answer is the
// shell's new prompt when the job ends -- after every `scheduler.run` in
// it has run to its stop -- or an error.  Returns 0 on success, -1 on
// failure (the interpreter has printed the reason).
export async function gsEvalLine(line: string): Promise<number> {
  if (!moduleReady || !mailbox) return -1;
  const text = (line ?? '').toString();
  if (!text.trim()) return 0;
  const stopWatch = watchRequest('shell.run');
  try {
    const r = await mailbox.script(text, CLIENT_TERMINAL, (id) => {
      foregroundJob = id;
      routeConsole({ kind: 'job_start', job: id });
    });
    // The job's output records precede its result on the ring, so what it
    // printed is in by now: the console ends the job (a last line without a
    // newline, a value whose marker never came).
    if (foregroundJob !== null) routeConsole({ kind: 'job_end', job: foregroundJob });
    if (r.ok) {
      const prompt: unknown = JSON.parse(r.json);
      if (typeof prompt === 'string') cachedPrompt = prompt.length ? prompt : null;
      return 0;
    }
    return -1;
  } catch {
    return -1;
  } finally {
    foregroundJob = null;
    stopWatch();
  }
}

// True while a terminal line is still running.
export function hasForegroundJob(): boolean {
  return foregroundJob !== null;
}

export function getRuntimePrompt(): string | null {
  return cachedPrompt;
}

// Seed the cached prompt from the C-side `shell.prompt` attribute.
// Called by the console on mount so the first prompt is visible
// before any user input. After this, gsEvalLine keeps cachedPrompt in
// sync via the return value of `shell.run`.
export async function seedPrompt(): Promise<void> {
  if (!moduleReady) return;
  const r = await gsEval('shell.prompt');
  if (typeof r === 'string' && r.length) cachedPrompt = r;
}

// Ctrl-C, exactly: cancel the terminal's foreground job if it has one;
// else stop a run the terminal itself started; else nothing (the machine
// running because the toolbar or a resume started it is not the
// terminal's to stop).  Returns what it did.
export async function shellInterrupt(): Promise<'cancelled' | 'stopped' | 'nothing'> {
  if (!moduleReady || !mailbox) return 'nothing';
  if (foregroundJob !== null) {
    const id = foregroundJob;
    await mailbox.cancel(CLIENT_TERMINAL, id);
    return 'cancelled';
  }
  return (await mailbox.modeStop(CLIENT_TERMINAL, CLIENT_TERMINAL)) ? 'stopped' : 'nothing';
}

// One completion candidate, as `shell.complete(…, true)` reports it.
export interface CompletionCandidate {
  text: string;
  kind: string; // object, collection, attr, method, alias, keyword, value, …
  doc: string;
}

export interface CompletionResult {
  candidates: CompletionCandidate[];
  // UTF-16 offsets into the line (the core's are UTF-8 bytes).
  span: { start: number; end: number };
  // Set when the cursor is in an argument of a resolved method.
  context: { method: string | null; argIndex: number | null; argName: string | null };
}

// Tab completion with detail.  `cursor` is a UTF-16 offset.
export async function tabComplete(line: string, cursor: number): Promise<CompletionResult | null> {
  if (!moduleReady) return null;
  const r = await gsEval('shell.complete', [line, utf16ToUtf8(line, cursor), true]);
  if (!r || typeof r !== 'object') return null;
  const obj = r as { candidates?: unknown; span?: unknown; context?: unknown };
  if (!Array.isArray(obj.candidates) || !obj.span || typeof obj.span !== 'object') return null;
  const span = obj.span as { start?: unknown; end?: unknown };
  if (typeof span.start !== 'number' || typeof span.end !== 'number') return null;
  const str = (v: unknown): string | null => (typeof v === 'string' ? v : null);
  const candidates: CompletionCandidate[] = [];
  for (const c of obj.candidates) {
    if (typeof c === 'string') candidates.push({ text: c, kind: '', doc: '' });
    else if (c && typeof c === 'object' && typeof (c as { text?: unknown }).text === 'string') {
      const o = c as Record<string, unknown>;
      candidates.push({
        text: o.text as string,
        kind: str(o.kind) ?? '',
        doc: str(o.doc) ?? '',
      });
    }
  }
  const ctx = (obj.context && typeof obj.context === 'object' ? obj.context : {}) as Record<
    string,
    unknown
  >;
  return {
    candidates,
    span: { start: utf8ToUtf16(line, span.start), end: utf8ToUtf16(line, span.end) },
    context: {
      method: str(ctx.method),
      argIndex: typeof ctx.arg_index === 'number' ? ctx.arg_index : null,
      argName: str(ctx.arg_name),
    },
  };
}

// Whether Enter should continue the input on a new line (an open block,
// bracket or string) rather than submit it.
export async function needsContinuation(text: string): Promise<boolean> {
  if (!moduleReady) return false;
  return (await gsEval('shell.needs_continuation', [text])) === true;
}

// One request through the mailbox: post, await the answer by id, decode.
// A failure of the mailbox itself (deadline, crash) is a transport error;
// the core's own {error} documents pass through as they are.
async function executeMailboxRequest(
  path: string,
  argsJson: string,
  onProgress?: (done: number, total: number) => void,
): Promise<unknown> {
  if (!mailbox) return transportError('emulator not ready');
  const stopWatch = watchRequest(path);
  try {
    const deadline = LONG_REQUEST.test(path) ? 0 : DEADLINE_MS;
    const r = await mailbox.request(path, argsJson, deadline, { onProgress });
    // What the leaf printed goes to the terminal, as it did when stdout
    // reached it directly.
    if (r.output) routeConsole({ kind: 'output', text: r.output, job: null });
    if (!r.json) return null;
    try {
      return JSON.parse(r.json);
    } catch {
      return r.json;
    }
  } catch (e) {
    const why = e as MailboxFailure | Error;
    if (why === 'deadline')
      return transportError(`'${path}' did not complete within ${DEADLINE_MS / 1000} s`);
    if (why === 'lost' || why === 'detached')
      return transportError(`emulator crashed: ${bridgeDead ?? 'mailbox lost'}`);
    return transportError(
      `mailbox request failed: ${why instanceof Error ? why.message : String(why)}`,
    );
  } finally {
    stopWatch();
  }
}

// --- Events from the core ------------------------------------------------

// What the core emits on its own (src/core/event/gs_event.h), decoded off
// the mailbox's event ring: `kind` is the ring's family, `data` the JSON
// object the emitter wrote, whose `event` names it.  Today: 'state' with
// `mode_started {mode, owner, budget}` and `mode_ended {mode, owner,
// reason, pc, instr_count}` from the scheduler.
export type CoreEventKind = 'state' | 'notify' | 'log';
export interface CoreEvent {
  kind: CoreEventKind;
  event: string;
  data: Record<string, unknown>;
}

const coreEventListeners = new Set<(ev: CoreEvent) => void>();

// Subscribes to core events; returns the unsubscribe.
export function onCoreEvent(cb: (ev: CoreEvent) => void): () => void {
  coreEventListeners.add(cb);
  return () => {
    coreEventListeners.delete(cb);
  };
}

const CORE_EVENT_KINDS: Record<number, CoreEventKind> = {
  [EVT_STATE]: 'state',
  [EVT_NOTIFY]: 'notify',
  [EVT_LOG]: 'log',
};

// The last few events, for automation and the browser console
// (window.__gsCoreEvents): the e2e tests assert on them.
const CORE_EVENT_TRACE_MAX = 64;
const coreEventTrace: CoreEvent[] = [];

export function dispatchCoreEvent(kindWord: number, json: string): void {
  const kind = CORE_EVENT_KINDS[kindWord];
  if (!kind) return;
  let data: Record<string, unknown>;
  try {
    const parsed: unknown = JSON.parse(json);
    if (!parsed || typeof parsed !== 'object') return;
    data = parsed as Record<string, unknown>;
  } catch {
    return;
  }
  const ev: CoreEvent = { kind, event: typeof data.event === 'string' ? data.event : '', data };
  if (coreEventTrace.push(ev) > CORE_EVENT_TRACE_MAX) coreEventTrace.shift();
  if (typeof window !== 'undefined')
    (window as unknown as { __gsCoreEvents?: CoreEvent[] }).__gsCoreEvents = coreEventTrace;
  routeCoreEvent(ev);
  for (const cb of coreEventListeners) cb(ev);
}

const num = (v: unknown): number => (typeof v === 'number' ? v : 0);

// What the page does with each event the core emits (docs/guide/web.md,
// "Events from the core").
function routeCoreEvent(ev: CoreEvent): void {
  const d = ev.data;
  const job = jobOf(d);
  switch (`${ev.kind}:${ev.event}`) {
    case 'state:mode_started':
      handleRunStateChange(true);
      break;
    case 'state:mode_ended':
      handleRunStateChange(false);
      break;
    case 'state:speed':
      setAcceleratedSpeed(num(d.x256) / 256);
      break;
    case 'state:perf':
      setPerfStats(num(d.mips), num(d.tps), {
        tickMaxMs: num(d.tick_max_ms),
        tickP50Ms: num(d.tick_p50_ms),
        pollMaxMs: num(d.poll_max_ms),
      });
      break;
    case 'notify:floppy':
      onFloppyDriveChange(num(d.drive), d.present === true);
      break;
    case 'notify:drive_activity':
      setDriveActivity(num(d.kind), num(d.state));
      break;
    case 'notify:checkpoint_saved':
      setCheckpointSaved(num(d.elapsed_ms));
      break;
    case 'notify:printer_status':
      if (typeof d.status === 'string') setPrinterStatus(d.status);
      break;
    case 'log:log':
      if (typeof d.line === 'string') routeLogEmit(d.line);
      break;
    case 'log:output':
      // A job's printed text, in order: the console shows it.
      if (typeof d.text === 'string') routeConsole({ kind: 'output', text: d.text, job });
      break;
    case 'notify:download_chunk':
      onDownloadChunk(d);
      break;
    // Annotation records in a job's stream, at the positions they describe:
    // the console turns the text around them into value / error entries.
    case 'log:value_begin':
      if (job !== null) routeConsole({ kind: 'value_begin', job });
      break;
    case 'log:value':
      if (job !== null)
        routeConsole({ kind: 'value', job, json: d.truncated ? undefined : d.json });
      break;
    case 'log:error':
      if (job !== null && Array.isArray(d.lines))
        routeConsole({
          kind: 'error',
          job,
          lines: (d.lines as unknown[]).map((l) => String(l)),
          truncated: d.truncated === true,
        });
      break;
    default:
      break;
  }
}

// The job id of a job-stream record, when it carries one.
function jobOf(d: Record<string, unknown>): number | null {
  return typeof d.id === 'number' ? d.id : null;
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
  // The mode_ended event reflects the new state.
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

// Measurement builds only (VITE_GS_MEASURE=1 at build time): the
// checkpoint-stall spec probes request latency through the page's own
// gsEval.  Not a shipped surface -- the typed UI path to the object model is
// the terminal (tests/e2e/README.md).
if (import.meta.env.VITE_GS_MEASURE && typeof window !== 'undefined')
  (window as unknown as { __gsEval?: unknown }).__gsEval = gsEval;
