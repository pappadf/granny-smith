// Reactive log buffer for the Logs panel view. Fed by bus/logSink.ts.
// The buffer is a capped ring (oldest lines drop when MAX is exceeded).
// Per-category levels mirror the C-side state; setCatLevel writes
// through via gsEval('log.<cat>.level = N').
//
// Perf: appendLog coalesces high-frequency emits through
// requestAnimationFrame so a burst of N lines only causes one reactive
// update per frame. Tests fall back to microtasks (queueMicrotask) so
// they can assert synchronously after `await Promise.resolve()`.

export interface LogEntry {
  ts: number; // wall-clock ms when JS observed the line
  cat: string; // category name from log.c
  level: number; // emission level
  msg: string; // body — preserves optional @ts / PC= prefixes
}

const MAX = 5000;

interface LogsState {
  entries: LogEntry[];
  autoscroll: boolean;
  catLevels: Record<string, number>;
}

export const logs: LogsState = $state({
  entries: [],
  autoscroll: true,
  catLevels: {},
});

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
  pendingEmits.length = 0;
  dropped = 0;
  noticeShown = false;
}

export function setAutoscroll(on: boolean): void {
  logs.autoscroll = on;
}

// Build a Blob of the current buffer and trigger a browser download.
export function downloadLogs(): void {
  const lines = logs.entries.map((e) => `[${e.cat}] ${e.level} ${e.msg}`);
  const blob = new Blob([lines.join('\n') + '\n'], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `logs-${compactStamp()}.txt`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function compactStamp(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-` +
    `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
  );
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
