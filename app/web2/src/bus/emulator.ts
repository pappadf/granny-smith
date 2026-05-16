// Real emulator bus — boots the WASM Module and exposes `gsEval` over the
// SAB-backed js_bridge_t slot. Port of app/web/js/emulator.js.
//
// THREADING — read before adding any new JS→C call site.
//
// With -sPROXY_TO_PTHREAD, main() / shell_init() / scheduler / device state
// all live on the WORKER thread. Direct Module.ccall from the main thread
// races that state. Every JS→C call MUST route through the SAB-backed
// bridge slot (`pending=1`, single in-flight). See docs/web.md.

import { machine, type MachineStatus } from '@/state/machine.svelte';
import { showNotification } from '@/state/toasts.svelte';
import { getOrCreateMachine } from '@/lib/machineId';
import { routePrintLine, routeLogEmit } from './logSink';
import type { MachineConfig } from './types';

const BRIDGE_VERSION = 5;
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
  };
  _get_js_bridge(): number;
  stringToUTF8(s: string, ptr: number, max: number): void;
  UTF8ToString(ptr: number): string;
}

interface EmscriptenModuleConfig {
  canvas: HTMLCanvasElement;
  arguments?: string[];
  locateFile?(path: string): string;
  print?(s: string): void;
  printErr?(s: string): void;
  onRunStateChange?(running: boolean): void;
  onScreenResize?(w: number, h: number): void;
  onLogEmit?(line: string): void;
}

type CreateModule = (config: EmscriptenModuleConfig) => Promise<EmscriptenModule>;

let Module: EmscriptenModule | null = null;
let moduleReady = false;
let bridgePtr = 0;
let cmdInFlight = false;
const cmdWaiters: Array<() => void> = [];

// Run-state mirror so we can ignore redundant transitions.
let isRunningUI = false;
let lastScreenW = 0;
let lastScreenH = 0;

// --- Bootstrap ----------------------------------------------------------

// Initialise the WASM module. The canvas is handed to Emscripten; subsequent
// resize callbacks update machine.screen so ScreenView can reflow.
export async function bootstrap(canvas: HTMLCanvasElement, wasmArgs: string[] = []): Promise<void> {
  if (moduleReady) return;
  const bust = Date.now();
  // The /main.mjs URL is served by the Vite middleware in vite.config.ts
  // (dev) or copied into dist/ at build time (prod). Either way the path
  // is stable from the browser's point of view.
  const url = /* @vite-ignore */ `/main.mjs?v=${bust}`;
  const mod = (await import(/* @vite-ignore */ url)) as { default: CreateModule };
  const createModule = mod.default;

  Module = await createModule({
    canvas,
    arguments: wasmArgs,
    locateFile: (p: string) => (p.endsWith('.wasm') ? `/main.wasm?v=${bust}` : p),
    print: routePrintLine,
    printErr: routePrintLine,
    onRunStateChange: handleRunStateChange,
    onScreenResize: handleScreenResize,
    onLogEmit: routeLogEmit,
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

function handleScreenResize(w: number, h: number): void {
  const width = w | 0;
  const height = h | 0;
  if (width === lastScreenW && height === lastScreenH) return;
  lastScreenW = width;
  lastScreenH = height;
  machine.screen.width = width;
  machine.screen.height = height;
}

// --- Module access for upload pipeline (FS writes to /tmp) -------------

export function getModule(): EmscriptenModule | null {
  return Module;
}

// --- Lifecycle wrappers --------------------------------------------------

// Boot a machine from a config. Sequence mirrors app/web/js/config-dialog.js
// bootFromConfig (the happy path; model-specific quirks land as bugs surface).
export async function initEmulator(config: MachineConfig): Promise<void> {
  // VROM and video-mode setup happen before machine.boot so card factories
  // can consume the sense lines during boot.
  if (config.vrom && config.vrom !== '(auto)') {
    await gsEval('vrom.load', [config.vrom]);
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
    const ok = await gsEval('rom.load', [config.rom]);
    if (ok !== true) {
      showNotification('Failed to load ROM', 'error');
      return;
    }
  }
  if (config.fd && config.fd !== '(none)') {
    await gsEval('floppy.drives[0].insert', [config.fd, true]);
  }
  if (config.hd && config.hd !== '(none)') {
    await gsEval('scsi.attach_hd', [config.hd, 0]);
  }
  if (config.cd && config.cd !== '(none)') {
    await gsEval('scsi.attach_cdrom', [config.cd, 3]);
  }

  machine.model = config.model;
  machine.ram = config.ram;
  machine.mmuEnabled = /SE\/30|II/i.test(config.model);

  await gsEval('scheduler.run');
  // onRunStateChange will flip machine.status to 'running' once the
  // worker pushes the transition.
  showNotification('Machine started', 'info');
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
