// Reactive log buffer for the Logs panel view. Fed by bus/logSink.ts.
// The buffer is a capped ring (oldest lines drop when MAX is exceeded).
// Per-category levels mirror the C-side state; setCatLevel writes
// through via gsEval('log.<cat>.level = N').
//
// Perf: appendLog coalesces high-frequency emits through
// requestAnimationFrame so a burst of N lines only causes one reactive
// update per frame. Tests fall back to microtasks (queueMicrotask) so
// they can assert synchronously after `await Promise.resolve()`.

import { saveBlob } from '@/lib/saveBlob';
import { compactLocalStamp } from '@/lib/time';
import { entryText } from '@/lib/logView';

export interface LogEntry {
  ts: number; // wall-clock ms when JS observed the line
  cat: string; // category name from log.c
  level: number; // emission level
  msg: string; // body — preserves optional @ts / PC= prefixes
}

// The most lines the buffer keeps (the oldest drop beyond it).
export const LOG_BUFFER_MAX = 10000;
const MAX = LOG_BUFFER_MAX;

interface LogsState {
  entries: LogEntry[];
  autoscroll: boolean;
  catLevels: Record<string, number>;
  // Per category: whether the core also prints its lines in the Terminal
  // (the category's `stdout`, on by default -- docs/user/debugger.md §4).
  catStdout: Record<string, boolean>;
  // The view's filters (lib/logView): show entries up to this level (null:
  // all) and not of a hidden category.  The buffer keeps everything.
  maxLevel: number | null;
  hidden: Record<string, boolean>;
  // Paused: the view holds the lines it had (`frozen`) while the buffer
  // goes on collecting; resuming shows everything again.
  frozen: LogEntry[] | null;
}

export const logs: LogsState = $state({
  entries: [],
  autoscroll: true,
  catLevels: {},
  catStdout: {},
  maxLevel: null,
  hidden: {},
  frozen: null,
});

// Pause or resume the view.
export function setLogsPaused(paused: boolean): void {
  logs.frozen = paused ? logs.entries.slice() : null;
}

// Show or hide one category's lines in the view.
export function setCategoryShown(cat: string, shown: boolean): void {
  logs.hidden[cat] = !shown;
}

// Pending emits waiting for the next frame flush.
let pendingEmits: LogEntry[] = [];
let scheduled = false;
// Lines dropped since the last clear.  Once any are, the first entry is a
// notice saying how many (kept there, its count updated, on every later
// overflow: S1018-87).
let dropped = 0;
let noticeShown = false; // entries[0] is the notice

// The notice row for `dropped` lines.
function droppedNotice(): LogEntry {
  return {
    ts: Date.now(),
    cat: 'logs',
    level: 0,
    msg: `[${dropped} older entries dropped — buffer capped at ${MAX} lines]`,
  };
}

function scheduleFlush(): void {
  if (scheduled) return;
  scheduled = true;
  if (typeof requestAnimationFrame === 'function') {
    requestAnimationFrame(flushPending);
  } else {
    queueMicrotask(flushPending);
  }
}

function flushPending(): void {
  scheduled = false;
  if (!pendingEmits.length) return;
  let batch = pendingEmits;
  pendingEmits = [];
  // More than the buffer holds: only the newest MAX can be kept.
  if (batch.length > MAX) {
    dropped += batch.length - MAX;
    batch = batch.slice(batch.length - MAX);
  }
  // The notice (when there is one) sits before the lines it is about.
  const first = noticeShown ? 1 : 0;
  const overflow = logs.entries.length - first + batch.length - MAX;
  if (overflow > 0) {
    logs.entries.splice(first, overflow);
    dropped += overflow;
  }
  if (dropped > 0) {
    if (noticeShown) logs.entries[0] = droppedNotice();
    else logs.entries.unshift(droppedNotice());
    noticeShown = true;
  }
  logs.entries.push(...batch);
}

export function appendLog(e: LogEntry): void {
  // A frame can be long in coming (a hidden tab has none): hold at most
  // twice the buffer, dropping the oldest, so a flood cannot grow it
  // without bound (S1018-04).
  if (pendingEmits.push(e) > 2 * MAX) {
    dropped += pendingEmits.length - MAX;
    pendingEmits = pendingEmits.slice(pendingEmits.length - MAX);
  }
  scheduleFlush();
}

export function clearLogs(): void {
  logs.entries.length = 0;
  if (logs.frozen) logs.frozen = [];
  pendingEmits.length = 0;
  dropped = 0;
  noticeShown = false;
}

export function setAutoscroll(on: boolean): void {
  logs.autoscroll = on;
}

// Build a Blob of the current buffer and trigger a browser download.
export function downloadLogs(): void {
  const lines = logs.entries.map(entryText);
  const blob = new Blob([lines.join('\n') + '\n'], { type: 'text/plain' });
  saveBlob(blob, `logs-${compactLocalStamp()}.txt`);
}

// Reload the per-category level map from the C side. Best effort —
// when the module hasn't booted yet the read returns null and we just
// keep the existing (possibly empty) map.
export async function refreshCatLevels(): Promise<void> {
  const { gsEval, isModuleReady } = await import('@/bus/emulator');
  if (!isModuleReady()) return;
  // log.levels returns a native object {<category>: <level>, ...} of every
  // registered category. (Categories register lazily as subsystems first log, so
  // the set grows over a session.)
  const map = await gsEval('log.levels');
  if (!map || typeof map !== 'object' || 'error' in map) return;
  const next: Record<string, number> = {};
  for (const [name, lvl] of Object.entries(map)) {
    if (typeof lvl === 'number') next[name] = lvl;
  }
  logs.catLevels = next;
  // Whether each prints in the Terminal too (one read per category, on
  // the popover's open only).
  const echo: Record<string, boolean> = {};
  for (const name of Object.keys(next)) {
    const v = await gsEval(`log.category["${name}"].stdout`);
    if (typeof v === 'boolean') echo[name] = v;
  }
  logs.catStdout = echo;
}

// Turn a category's Terminal copy on or off (its `stdout`); every line
// still reaches the Logs panel.  Mirrors locally on success.
export async function setCatStdout(cat: string, on: boolean): Promise<boolean> {
  const { gsEval, gsOk, isModuleReady } = await import('@/bus/emulator');
  if (!isModuleReady()) return false;
  if (!gsOk(await gsEval(`log.category["${cat}"].stdout`, [on]))) return false;
  logs.catStdout[cat] = on;
  return true;
}

// Write a new level for one category and mirror locally on success.
export async function setCatLevel(cat: string, level: number): Promise<boolean> {
  const { gsEval, gsOk, isModuleReady } = await import('@/bus/emulator');
  if (!isModuleReady()) return false;
  // log.set(category, level) adjusts the per-subsystem level.  `category`
  // is a typed enum over the log manifest, so an unknown name is REJECTED
  // rather than silently created -- the names here come from
  // log.levels, which lists that manifest, so they are always valid.
  // Further options are named arguments: log.set(cat, stdout=, file=, ts=,
  // pc=).
  // Mirror the level only if the core took it.
  if (!gsOk(await gsEval('log.set', [cat, level]))) return false;
  logs.catLevels[cat] = level;
  return true;
}
