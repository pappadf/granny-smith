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
  setPerfStats,
  type MachineStatus,
  type MmuKind,
  type SchedulerMode,
} from '@/state/machine.svelte';
import { onFloppyDriveChange } from '@/state/images.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { getOrCreateMachine } from '@/lib/machineId';
import { routePrintLine, routeLogEmit } from './logSink';
import { resetDebugSections } from '@/state/debug.svelte';
import type { MachineConfig } from './types';

const BRIDGE_VERSION = 6;
const OFF_VERSION = 0;
const OFF_READY = 4;
const OFF_PENDING = 8;
const OFF_DONE = 12;
const OFF_PATH = 20;
const OFF_ARGS = 1044;
const OFF_OUTPUT = 9236;
const PATH_SIZE = 1024;
const ARGS_SIZE = 8192;

// Minimal Emscripten module surface — enough to type-check what bus uses.
interface EmscriptenModule {
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
    close(stream: unknown): void;
  };
  _get_js_bridge(): number;
  stringToUTF8(s: string, ptr: number, max: number): void;
  UTF8ToString(ptr: number): string;
}

interface EmscriptenModuleConfig {
  canvas: HTMLCanvasElement;
  arguments?: string[];
  mainScriptUrlOrBlob?: string;
  locateFile?(path: string): string;
  print?(s: string): void;
  printErr?(s: string): void;
  onRunStateChange?(running: boolean): void;
  onScreenResize?(w: number, h: number, parW?: number, parH?: number): void;
  onLogEmit?(line: string): void;
  onFloppyChange?(drive: number, present: boolean): void;
  onSchedulerSpeed?(speedX256: number): void;
  onPerfUpdate?(mipsX100: number, tpsX10: number): void;
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
// has completed).
let resolveReady: (() => void) | null = null;
const readyPromise: Promise<void> = new Promise((res) => {
  resolveReady = res;
});

export function whenModuleReady(): Promise<void> {
  return readyPromise;
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
export async function bootstrap(canvas: HTMLCanvasElement, wasmArgs: string[] = []): Promise<void> {
  if (moduleReady) return;
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
    arguments: wasmArgs,
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
    printErr: routePrintLine,
    onRunStateChange: handleRunStateChange,
    onScreenResize: handleScreenResize,
    onLogEmit: routeLogEmit,
    onFloppyChange: onFloppyDriveChange,
    onSchedulerSpeed: handleSchedulerSpeed,
    onPerfUpdate: handlePerfUpdate,
  });

  bridgePtr = Module._get_js_bridge();
  const v = Module.HEAP32[(bridgePtr + OFF_VERSION) >> 2];
  if (v !== BRIDGE_VERSION) {
    throw new Error(`js_bridge version mismatch: C=${v}, JS=${BRIDGE_VERSION}`);
  }
  moduleReady = true;

  // Activate per-machine checkpoint directory before anything that opens
  // images. Matches app/web/js/main.js:81-83.
  const id = getOrCreateMachine();
  await gsEval('machine.register', [id.id, id.created]);

  // Resolve the public ready signal — TerminalPane (and anyone else
  // who needs the bridge live) is awaiting this.
  resolveReady?.();
}

export function isModuleReady(): boolean {
  return moduleReady;
}

// --- gsEval -------------------------------------------------------------

export async function gsEval(path: string, args?: unknown[]): Promise<unknown> {
  if (!Module || !moduleReady) return null;
  await waitForBridgeReady();
  const argsJson = args === undefined || args === null ? '' : JSON.stringify(args);
  try {
    return await executeGsRequest(path || '', argsJson);
  } catch {
    return null;
  }
}

// Human-readable reason from a gsEval result. The bridge encodes a C-side
// V_ERROR as {"error": "..."}; null means the worker/module wasn't available;
// anything else stringifies. Single home for the error-shape knowledge so
// callers don't each re-implement the check.
export function gsErrorText(res: unknown): string {
  if (res === null) return 'emulator not ready';
  if (res && typeof res === 'object' && 'error' in res) {
    return String((res as { error: unknown }).error);
  }
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
  try {
    if (!Module) return null;
    Module.stringToUTF8(path, bridgePtr + OFF_PATH, PATH_SIZE);
    Module.stringToUTF8(argsJson, bridgePtr + OFF_ARGS, ARGS_SIZE);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_DONE) >> 2, 0);
    Atomics.store(Module.HEAP32, (bridgePtr + OFF_PENDING) >> 2, 1);
    await waitForBridgeDone();
    return readBridgeOutput();
  } finally {
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

function handleScreenResize(w: number, h: number, parW?: number, parH?: number): void {
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

// --- Lifecycle wrappers --------------------------------------------------

// Remember the most recent boot config so `restart()` can re-apply it
// without a C-side reset method. Cleared on shutdown so a stale config
// from a previous machine doesn't restart unexpectedly.
let lastBootConfig: MachineConfig | null = null;

export function getLastBootConfig(): MachineConfig | null {
  return lastBootConfig;
}

// Read a model's capability probe from `machine.profile().capabilities` and
// apply it to the shared machine state. Replaces the old display-name regex
// that silently misclassified any MMU machine whose name didn't match the
// hardcoded pattern. `mmuEnabled` stays the boolean the debug panels gate on,
// but it is now derived from the typed kind (only a 68030 PMMU enables the
// register views; the Lisa segment MMU and "none" leave them off); `mmuKind`
// carries the full typed kind for display, and `fpu` gates the FPU panel.
export async function applyCapabilities(model: string): Promise<void> {
  let kind: MmuKind = 'none';
  let fpu = false;
  try {
    const r = await gsEval('machine.profile', [model]);
    if (typeof r === 'string') {
      const parsed = JSON.parse(r) as {
        capabilities?: { mmu?: { kind?: string }; cpu?: { fpu?: boolean } };
      };
      const k = parsed.capabilities?.mmu?.kind;
      if (k === '68030_pmmu' || k === 'lisa_segment') kind = k;
      fpu = parsed.capabilities?.cpu?.fpu === true;
    }
  } catch {
    /* leave kind = 'none', fpu = false */
  }
  machine.mmuKind = kind;
  machine.mmuEnabled = kind === '68030_pmmu';
  machine.fpu = fpu;
}

// Boot a machine from a config. Sequence mirrors app/web/js/config-dialog.js
// bootFromConfig (the happy path; model-specific quirks land as bugs surface).
export async function initEmulator(config: MachineConfig): Promise<void> {
  // VROM and video-mode setup happen before machine.boot so card factories
  // can consume the sense lines during boot.
  if (config.vrom && config.vrom !== '(auto)') {
    await gsEval('machine.vrom.load', [config.vrom]);
  }
  // Select the NuBus video card before boot. Without this the slot falls back
  // to its default card (e.g. the IIcx's "mdc_8_24"), so an uploaded 24AC vROM
  // would boot an 8•24 instead. The id comes from the dialog's probe of the
  // chosen card; the card factory content-matches its vROM among the offered
  // files (the explicit vrom.load above being the preferred pick).
  if (config.videoCard) {
    await gsEval('machine.nubus.video_card', [config.videoCard]);
  }
  // Seed the JMFB video mode before machine.boot so the card factory consumes
  // it (sets the sense lines + slot-PRAM/video defaults).  web-legacy's
  // bootFromConfig does this; omitting it left the JMFB unseeded and A/UX hung
  // enabling its device drivers on real hardware.
  if (config.videoMode) {
    await gsEval('machine.nubus.video_mode', [config.videoMode]);
  }
  // Map the human-readable RAM string ('4 MB') to KB the boot path wants.
  const ramKB = ramStringToKb(config.ram);
  if (config.model) {
    await gsEval('machine.boot', [config.model, ramKB]);
  }
  // ROM: in this Phase 2/3 shape the user's MachineConfig stores the
  // OPFS path of the ROM directly (Welcome's scan populates the select
  // with paths). When the path is missing or '(auto)' we skip the load —
  // useful for the URL-media path where rom.load happens elsewhere.
  if (config.rom && config.rom !== '(auto)') {
    const ok = await gsEval('machine.rom.load', [config.rom]);
    if (ok !== true) {
      showNotification('Failed to load ROM', 'error');
      return;
    }
  }
  for (let i = 0; i < (config.floppies?.length ?? 0); i++) {
    const path = config.floppies[i];
    if (!path || path === '(none)') continue;
    await gsEval(`machine.floppy.drive[${i}].insert`, [path, true]);
  }
  if (config.hd && config.hd !== '(none)') {
    if (config.hdBus === 'profile') {
      // Lisa/XL: the hard disk is the parallel-port ProFile, not a SCSI device.
      await gsEval('machine.hd.attach', [config.hd, true]);
    } else {
      await gsEval('machine.scsi.attach_hd', [config.hd, 0]);
    }
  }
  if (config.cd && config.cd !== '(none)') {
    await gsEval('machine.scsi.attach_cdrom', [config.cd, 3]);
  }

  machine.model = config.modelName ?? config.model;
  machine.ram = config.ram;
  await applyCapabilities(config.model);

  // A fresh core boots paced; re-assert the user's toolbar selection so a
  // pre-selected Turbo survives machine (re)creation.
  await applySchedulerMode(machine.scheduler);
  await gsEval('scheduler.run');
  // onRunStateChange will flip machine.status to 'running' once the
  // worker pushes the transition.
  lastBootConfig = config;
  // Every new boot starts with the Debug-tab sections collapsed —
  // only the always-visible Disassembly pane shows by default.
  // Persisted localStorage state is overwritten by this reset, which
  // is the user-requested behaviour (each new machine gets a clean
  // debug layout).
  resetDebugSections();
  showNotification('Machine started', 'info');
}

export async function shutdownEmulator(): Promise<void> {
  await gsEval('scheduler.stop');
  machine.status = 'stopped' as MachineStatus;
  lastBootConfig = null;
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
export async function saveCheckpoint(): Promise<string> {
  const ts = compactTimestamp();
  const tmpPath = `/tmp/saved-state-${ts}.bin`;
  await gsEval('checkpoint.save', [tmpPath]);
  await gsEval('download', [tmpPath]);
  return tmpPath;
}

// --- Small helpers ------------------------------------------------------

function ramStringToKb(ram: string): number {
  const m = /(\d+)\s*MB/i.exec(ram || '');
  if (!m) return 4096;
  return parseInt(m[1], 10) * 1024;
}

function compactTimestamp(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-` +
    `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
  );
}

export { ramStringToKb };
