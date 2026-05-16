// Reactive log buffer for the Logs panel view. Fed by bus/logSink.ts.
// The buffer is a capped ring (oldest lines drop when MAX is exceeded).
// Per-category levels mirror the C-side state; setCatLevel writes
// through via gsEval('log.<cat>.level = N').
//
// Phase 7 perf: appendLog coalesces high-frequency emits through
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
// One-shot sentinel — emitted on the first overflow after a fresh boot
// so the user knows oldest entries were dropped.
let overflowAnnounced = false;

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
  const batch = pendingEmits;
  pendingEmits = [];
  const overflow = logs.entries.length + batch.length - MAX;
  if (overflow > 0) {
    logs.entries.splice(0, overflow);
    if (!overflowAnnounced) {
      overflowAnnounced = true;
      logs.entries.unshift({
        ts: Date.now(),
        cat: 'logs',
        level: 0,
        msg: `[${overflow} older entries dropped — buffer capped at ${MAX} lines]`,
      });
    }
  }
  logs.entries.push(...batch);
}

export function appendLog(e: LogEntry): void {
  pendingEmits.push(e);
  scheduleFlush();
}

export function clearLogs(): void {
  logs.entries.length = 0;
  pendingEmits.length = 0;
  overflowAnnounced = false;
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
  const children = await gsEval('log.meta.children');
  if (!Array.isArray(children)) return;
  const next: Record<string, number> = {};
  for (const name of children) {
    if (typeof name !== 'string') continue;
    const lvl = await gsEval(`log.${name}.level`);
    if (typeof lvl === 'number') next[name] = lvl;
    else if (typeof lvl === 'string') {
      const n = parseInt(lvl, 10);
      if (Number.isFinite(n)) next[name] = n;
    }
  }
  logs.catLevels = next;
}

// Write a new level for one category and mirror locally on success.
export async function setCatLevel(cat: string, level: number): Promise<boolean> {
  const { gsEval, isModuleReady } = await import('@/bus/emulator');
  if (!isModuleReady()) return false;
  // Use the typed setter form. gsEval accepts a `path = value` shape
  // via the bridge dispatcher (see docs/shell.md "Path forms").
  await gsEval(`log.${cat}.level = ${level}`);
  logs.catLevels[cat] = level;
  return true;
}
